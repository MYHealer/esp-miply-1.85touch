/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp/tca9555_io_expander.h"

#include <stdio.h>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "TCA9555";

enum {
    TCA9555_REG_INPUT_PORT0 = 0x00,
    TCA9555_REG_OUTPUT_PORT0 = 0x02,
    TCA9555_REG_POLARITY_PORT0 = 0x04,
    TCA9555_REG_CONFIG_PORT0 = 0x06,
};

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static uint16_t s_output_levels = BSP_IO_EXPANDER_BOOT_OUTPUT_LEVELS;
static uint16_t s_input_mask = BSP_IO_EXPANDER_DEFAULT_INPUT_MASK;

enum {
    LTE_POWER_RST_ASSERT_MS = 20,
    LTE_POWER_OFF_SETTLE_MS = 50,
    LTE_POWER_ON_SETTLE_MS = 1500,
};

#define BSP_IO_EXPANDER_LTE_OUTPUT_MASK \
    ((uint16_t)((1U << BSP_IO_EXPANDER_VDD38_CTL) | \
                (1U << BSP_IO_EXPANDER_LTE_RST_N)))

#define BSP_IO_EXPANDER_LTE_POWER_OFF_OUTPUT_LEVELS \
    ((uint16_t)(BSP_IO_EXPANDER_BOOT_OUTPUT_LEVELS & \
                (uint16_t)(~BSP_IO_EXPANDER_LTE_OUTPUT_MASK)))

#define BSP_IO_EXPANDER_LOW_POWER_OFF_MASK \
    ((uint16_t)(BSP_IO_EXPANDER_LTE_OUTPUT_MASK | \
                (1U << BSP_IO_EXPANDER_MOTOR_CTRL) | \
                (1U << BSP_IO_EXPANDER_CODEC_PWR_CTL)))

static esp_err_t take_lock(void)
{
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE, ESP_FAIL, TAG, "Take lock failed");
    return ESP_OK;
}

static void give_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static bool is_valid_pin(bsp_io_expander_pin_t pin)
{
    return (pin >= 0) && (pin < BSP_IO_EXPANDER_PIN_MAX);
}

static bool is_active_low_pin(bsp_io_expander_pin_t pin)
{
    switch (pin) {
    case BSP_IO_EXPANDER_W_RST_CTL:
    case BSP_IO_EXPANDER_CHRG_N:
    case BSP_IO_EXPANDER_DOWN_N:
    case BSP_IO_EXPANDER_SD_DET:
    case BSP_IO_EXPANDER_LTE_RST_N:
        return true;
    default:
        return false;
    }
}

static esp_err_t write_word(uint8_t reg, uint16_t value)
{
    uint8_t data[] = {
        reg,
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
    };
    return i2c_master_transmit(s_dev, data, sizeof(data), 1000);
}

static esp_err_t read_word(uint8_t reg, uint16_t *value)
{
    uint8_t data[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_dev, &reg, 1, data, sizeof(data), 1000);
    if (ret != ESP_OK) {
        return ret;
    }
    *value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return ESP_OK;
}

static uint16_t merge_charge_led_outputs_from_inputs(uint16_t output_levels, uint16_t input_levels)
{
    const bool charging = ((input_levels & (uint16_t)(1U << BSP_IO_EXPANDER_CHRG_N)) == 0);
    const bool full = ((input_levels & (uint16_t)(1U << BSP_IO_EXPANDER_DOWN_N)) == 0);

    output_levels = (uint16_t)(output_levels & (uint16_t)(~BSP_IO_EXPANDER_CHARGE_LED_OUTPUT_MASK));
    if (full) {
        output_levels |= (uint16_t)(1U << BSP_IO_EXPANDER_DOWN_POW);
    } else if (charging) {
        output_levels |= (uint16_t)(1U << BSP_IO_EXPANDER_CHRG_POW);
    }

    return output_levels;
}

static esp_err_t init_with_output_levels(uint16_t initial_output_levels)
{
    esp_err_t ret = ESP_OK;

    if (s_dev != NULL) {
        return ESP_OK;
    }

    /* Create mutex before first lock. Guaranteed single-threaded: called from
       app_main → bsp_power_init before any other tasks exist. */
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "Create lock failed");
    }

    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    if (s_dev != NULL) {
        give_lock();
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_GOTO_ON_FALSE(bus != NULL, ESP_FAIL, err, TAG, "I2C bus is not ready");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_IO_EXPANDER_I2C_ADDR_7BIT,
        .scl_speed_hz = BSP_IO_EXPANDER_I2C_SPEED_HZ,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), err, TAG, "Add I2C device failed");

    /* Program outputs first, then direction, to avoid output glitches.
       Preserve charger LED state in the first output write so wakeup does not
       briefly clear LEDs that the ULP kept on during deep sleep. */
    uint16_t output_levels = initial_output_levels;
    uint16_t input_levels = 0;
    esp_err_t input_ret = read_word(TCA9555_REG_INPUT_PORT0, &input_levels);
    if (input_ret == ESP_OK) {
        output_levels = merge_charge_led_outputs_from_inputs(output_levels, input_levels);
    } else {
        ESP_LOGW(TAG, "Read charger inputs before output init failed: %s", esp_err_to_name(input_ret));
    }

    s_output_levels = output_levels;
    s_input_mask = BSP_IO_EXPANDER_DEFAULT_INPUT_MASK;
    ESP_GOTO_ON_ERROR(write_word(TCA9555_REG_OUTPUT_PORT0, s_output_levels), err_remove, TAG, "Write output failed");
    ESP_GOTO_ON_ERROR(write_word(TCA9555_REG_POLARITY_PORT0, 0x0000), err_remove, TAG, "Write polarity failed");
    ESP_GOTO_ON_ERROR(write_word(TCA9555_REG_CONFIG_PORT0, s_input_mask), err_remove, TAG, "Write config failed");

    ESP_LOGI(
        TAG,
        "TCA9555 ready: addr7=0x%02x, write=0x%02x, read=0x%02x, input_mask=0x%04x, output=0x%04x",
        BSP_IO_EXPANDER_I2C_ADDR_7BIT,
        BSP_IO_EXPANDER_I2C_WRITE_ADDR,
        BSP_IO_EXPANDER_I2C_READ_ADDR,
        s_input_mask,
        s_output_levels
    );

    give_lock();
    return ESP_OK;

err_remove:
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
err:
    give_lock();
    return ESP_FAIL;
}

esp_err_t bsp_io_expander_init(void)
{
    return init_with_output_levels(BSP_IO_EXPANDER_BOOT_OUTPUT_LEVELS);
}

bool bsp_io_expander_is_initialized(void)
{
    return s_dev != NULL;
}

esp_err_t bsp_io_expander_apply_stable_levels(void)
{
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    /* Build from cache to preserve runtime pin state. Only clear W_RST_CTL. */
    uint16_t output_levels = s_output_levels;
    output_levels &= (uint16_t)~(1U << BSP_IO_EXPANDER_W_RST_CTL);

    /* Restore chip registers in init order: outputs → polarity → direction.
       TCA9555 may have lost power during deep sleep. */
    esp_err_t ret = write_word(TCA9555_REG_OUTPUT_PORT0, output_levels);
    if (ret == ESP_OK) {
        ret = write_word(TCA9555_REG_POLARITY_PORT0, 0x0000);
    }
    if (ret == ESP_OK) {
        ret = write_word(TCA9555_REG_CONFIG_PORT0, s_input_mask);
    }
    if (ret == ESP_OK) {
        s_output_levels = output_levels;
        ESP_LOGI(TAG, "Applied stable output levels: output=0x%04x", s_output_levels);
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_prepare_low_power_sleep(void)
{
    ESP_RETURN_ON_ERROR(init_with_output_levels(BSP_IO_EXPANDER_LTE_POWER_OFF_OUTPUT_LEVELS),
                        TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    uint16_t output_levels = s_output_levels;
    output_levels &= (uint16_t)(~BSP_IO_EXPANDER_LOW_POWER_OFF_MASK);
    /* W_RST_CTL is active low; keep it enabled so charging stays available in low power. */
    output_levels &= (uint16_t)(~(1U << BSP_IO_EXPANDER_W_RST_CTL));
    output_levels |= (uint16_t)(1U << BSP_IO_EXPANDER_CHRG_CE);

    esp_err_t ret = write_word(TCA9555_REG_OUTPUT_PORT0, output_levels);
    if (ret == ESP_OK) {
        s_output_levels = output_levels;
        ESP_LOGI(TAG, "Prepared low-power output levels: output=0x%04x", s_output_levels);
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_set_charge_leds(bool charging, bool full)
{
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    uint16_t output_levels = (uint16_t)(s_output_levels & (uint16_t)(~BSP_IO_EXPANDER_CHARGE_LED_OUTPUT_MASK));
    if (full) {
        output_levels |= (uint16_t)(1U << BSP_IO_EXPANDER_DOWN_POW);
    } else if (charging) {
        output_levels |= (uint16_t)(1U << BSP_IO_EXPANDER_CHRG_POW);
    }

    esp_err_t ret = write_word(TCA9555_REG_OUTPUT_PORT0, output_levels);
    if (ret == ESP_OK) {
        s_output_levels = output_levels;
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_set_lte_power(bool enabled)
{
    esp_err_t ret = ESP_OK;

    ret = init_with_output_levels(BSP_IO_EXPANDER_LTE_POWER_OFF_OUTPUT_LEVELS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Init IO expander for LTE power control failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (enabled) {
        ret = bsp_io_expander_set_pin_active(BSP_IO_EXPANDER_LTE_RST_N, true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Assert LTE_RST_N before power on failed: %s", esp_err_to_name(ret));
            return ret;
        }

        vTaskDelay(pdMS_TO_TICKS(LTE_POWER_RST_ASSERT_MS));

        ret = bsp_io_expander_set_pin_level(BSP_IO_EXPANDER_VDD38_CTL, true);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Enable LTE VDD38_CTL failed: %s", esp_err_to_name(ret));
            return ret;
        }

        vTaskDelay(pdMS_TO_TICKS(LTE_POWER_ON_SETTLE_MS));

        ret = bsp_io_expander_set_pin_active(BSP_IO_EXPANDER_LTE_RST_N, false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Release LTE_RST_N failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ESP_LOGI(TAG, "LTE VDD38 control enabled, reset released");
        return ESP_OK;
    }

    ret = bsp_io_expander_set_pin_active(BSP_IO_EXPANDER_LTE_RST_N, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Assert LTE_RST_N failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(LTE_POWER_RST_ASSERT_MS));

    ret = bsp_io_expander_set_pin_level(BSP_IO_EXPANDER_VDD38_CTL, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Disable LTE VDD38_CTL failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(LTE_POWER_OFF_SETTLE_MS));
    ESP_LOGI(TAG, "LTE VDD38 control disabled, reset asserted");
    return ESP_OK;
}

esp_err_t bsp_io_expander_set_direction_mask(uint16_t input_mask)
{
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    esp_err_t ret = write_word(TCA9555_REG_CONFIG_PORT0, input_mask);
    if (ret == ESP_OK) {
        s_input_mask = input_mask;
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_get_direction_mask(uint16_t *input_mask)
{
    ESP_RETURN_ON_FALSE(input_mask != NULL, ESP_ERR_INVALID_ARG, TAG, "input_mask is null");
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    esp_err_t ret = read_word(TCA9555_REG_CONFIG_PORT0, input_mask);
    if (ret == ESP_OK) {
        s_input_mask = *input_mask;
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_write_outputs(uint16_t output_levels)
{
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    esp_err_t ret = write_word(TCA9555_REG_OUTPUT_PORT0, output_levels);
    if (ret == ESP_OK) {
        s_output_levels = output_levels;
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_read_outputs(uint16_t *output_levels)
{
    ESP_RETURN_ON_FALSE(output_levels != NULL, ESP_ERR_INVALID_ARG, TAG, "output_levels is null");
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    esp_err_t ret = read_word(TCA9555_REG_OUTPUT_PORT0, output_levels);
    if (ret == ESP_OK) {
        s_output_levels = *output_levels;
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_read_inputs(uint16_t *input_levels)
{
    ESP_RETURN_ON_FALSE(input_levels != NULL, ESP_ERR_INVALID_ARG, TAG, "input_levels is null");
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    esp_err_t ret = read_word(TCA9555_REG_INPUT_PORT0, input_levels);

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_set_pin_level(bsp_io_expander_pin_t pin, bool high)
{
    ESP_RETURN_ON_FALSE(is_valid_pin(pin), ESP_ERR_INVALID_ARG, TAG, "Invalid pin: %d", (int)pin);
    ESP_RETURN_ON_ERROR(bsp_io_expander_init(), TAG, "Init IO expander failed");
    ESP_RETURN_ON_ERROR(take_lock(), TAG, "Lock failed");

    uint16_t output_levels = s_output_levels;
    uint16_t pin_mask = (uint16_t)(1U << pin);
    if (high) {
        output_levels |= pin_mask;
    } else {
        output_levels &= (uint16_t)(~pin_mask);
    }

    esp_err_t ret = write_word(TCA9555_REG_OUTPUT_PORT0, output_levels);
    if (ret == ESP_OK) {
        s_output_levels = output_levels;
    }

    give_lock();
    return ret;
}

esp_err_t bsp_io_expander_get_pin_level(bsp_io_expander_pin_t pin, bool *high)
{
    ESP_RETURN_ON_FALSE(is_valid_pin(pin), ESP_ERR_INVALID_ARG, TAG, "Invalid pin: %d", (int)pin);
    ESP_RETURN_ON_FALSE(high != NULL, ESP_ERR_INVALID_ARG, TAG, "high is null");

    uint16_t input_levels = 0;
    esp_err_t ret = bsp_io_expander_read_inputs(&input_levels);
    if (ret != ESP_OK) {
        return ret;
    }

    *high = ((input_levels & (uint16_t)(1U << pin)) != 0);
    return ESP_OK;
}

esp_err_t bsp_io_expander_set_pin_active(bsp_io_expander_pin_t pin, bool active)
{
    bool high = is_active_low_pin(pin) ? !active : active;
    return bsp_io_expander_set_pin_level(pin, high);
}

void bsp_io_expander_dump_pins(void)
{
    if (!bsp_io_expander_is_initialized()) {
        ESP_LOGW(TAG, "Not initialized, skip pin dump");
        return;
    }

    uint16_t inputs = 0, outputs = 0;
    if (read_word(TCA9555_REG_INPUT_PORT0, &inputs) != ESP_OK) {
        ESP_LOGW(TAG, "Read inputs failed, skip pin dump");
        return;
    }
    if (read_word(TCA9555_REG_OUTPUT_PORT0, &outputs) != ESP_OK) {
        ESP_LOGW(TAG, "Read outputs failed, skip pin dump");
        return;
    }

    static const char *const kNames[] = {
        [BSP_IO_EXPANDER_MOTOR_CTRL]    = "MOTOR_CTRL",
        [BSP_IO_EXPANDER_CODEC_PWR_CTL] = "CODEC_PWR",
        [BSP_IO_EXPANDER_W_RST_CTL]     = "W_RST_CTL",
        [BSP_IO_EXPANDER_CHRG_CE]       = "CHRG_CE",
        [BSP_IO_EXPANDER_CHRG_N]        = "CHRG_N",
        [BSP_IO_EXPANDER_DOWN_N]        = "DOWN_N",
        [BSP_IO_EXPANDER_IMU_INT]       = "IMU_INT",
        [BSP_IO_EXPANDER_SD_DET]        = "SD_DET",
        [BSP_IO_EXPANDER_CHRG_POW]      = "CHRG_POW",
        [BSP_IO_EXPANDER_DOWN_POW]      = "DOWN_POW",
        [BSP_IO_EXPANDER_VDD38_CTL]     = "VDD38_CTL",
        [BSP_IO_EXPANDER_P1_3]          = "P1_3",
        [BSP_IO_EXPANDER_P1_4]          = "P1_4",
        [BSP_IO_EXPANDER_P1_5]          = "P1_5",
        [BSP_IO_EXPANDER_P1_6]          = "P1_6",
        [BSP_IO_EXPANDER_LTE_RST_N]     = "LTE_RST_N",
    };

    printf("\n"
           "  +----------+------------+-------+-------+---------+\n"
           "  | 引脚     | 名称       | 方向  | 电平 | 有效    |\n"
           "  +----------+------------+-------+-------+---------+\n");

    for (int p = 0; p < BSP_IO_EXPANDER_PIN_MAX; p++) {
        const int port = (p >= 8) ? 1 : 0;
        const int bit  = p & 7;
        const bool is_input = (s_input_mask & (1U << p)) != 0;
        const uint16_t word = is_input ? inputs : outputs;
        const bool level = (word & (1U << p)) != 0;
        const bool active = is_active_low_pin((bsp_io_expander_pin_t)p) ? !level : level;
        const char *dir = is_input ? "I" : "O";
        const char *act = is_input ? (active ? "*" : "-") : " ";

        printf("  | P%d.%-4d  | %-10s |  %s    |   %d   |   %s     |\n",
               port, bit, kNames[p], dir, level, act);
    }

    printf("  +----------+------------+-------+-------+---------+\n\n");
}

esp_err_t bsp_io_expander_get_pin_active(bsp_io_expander_pin_t pin, bool *active)
{
    ESP_RETURN_ON_FALSE(active != NULL, ESP_ERR_INVALID_ARG, TAG, "active is null");

    bool high = false;
    esp_err_t ret = bsp_io_expander_get_pin_level(pin, &high);
    if (ret != ESP_OK) {
        return ret;
    }

    *active = is_active_low_pin(pin) ? !high : high;
    return ESP_OK;
}

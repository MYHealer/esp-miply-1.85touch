#include "echopal_board.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ECHOPAL_BOARD";

/* TCA9554 registers (8-bit I/O expander) */
#define TCA9554_INPUT_REG   0x00
#define TCA9554_OUTPUT_REG  0x01
#define TCA9554_POLARITY_REG 0x02
#define TCA9554_CONFIG_REG  0x03

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_io_expander;
static uint8_t s_tca_output_levels;

static esp_err_t tca9554_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_io_expander, data, sizeof(data), 100);
}

static esp_err_t tca9554_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_io_expander, &reg, 1, value, 1, 100);
}

esp_err_t echopal_board_init(void)
{
    if (s_io_expander != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = ECHOPAL_I2C_SDA_GPIO,
        .scl_io_num = ECHOPAL_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG,
                        "Init I2C bus failed");

    const i2c_device_config_t expander_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ECHOPAL_TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &expander_config, &s_io_expander);
    if (ret != ESP_OK) {
        goto error;
    }

    /* Configure TCA9554: all pins as output (matching demo project) */
    const uint8_t config_mask = 0x00;  /* All 8 pins = output */
    ret = tca9554_write(TCA9554_OUTPUT_REG, 0x00);
    if (ret == ESP_OK) {
        ret = tca9554_write(TCA9554_POLARITY_REG, 0x00);
    }
    if (ret == ESP_OK) {
        ret = tca9554_write(TCA9554_CONFIG_REG, config_mask);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "Init TCA9554 failed");
    s_tca_output_levels = 0x00;

    ESP_LOGI(TAG, "EchoPal board initialized (TCA9554 @ I2C SDA=%d SCL=%d)",
             ECHOPAL_I2C_SDA_GPIO, ECHOPAL_I2C_SCL_GPIO);
    return ESP_OK;

error:
    if (s_io_expander != NULL) {
        i2c_master_bus_rm_device(s_io_expander);
        s_io_expander = NULL;
    }
    i2c_del_master_bus(s_i2c_bus);
    s_i2c_bus = NULL;
    return ret;
}

i2c_master_bus_handle_t echopal_board_get_i2c(void)
{
    return s_i2c_bus;
}

esp_err_t echopal_board_set_touch_reset(bool asserted)
{
    ESP_RETURN_ON_FALSE(s_io_expander != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Board is not initialized");
    uint8_t value = s_tca_output_levels;
    if (asserted) {
        value &= (uint8_t)~(1U << ECHOPAL_TOUCH_RST_PIN);
    } else {
        value |= (uint8_t)(1U << ECHOPAL_TOUCH_RST_PIN);
    }
    ESP_RETURN_ON_ERROR(tca9554_write(TCA9554_OUTPUT_REG, value), TAG,
                        "Set touch reset failed");
    s_tca_output_levels = value;
    return ESP_OK;
}

esp_err_t echopal_board_set_lcd_reset(bool asserted)
{
    ESP_RETURN_ON_FALSE(s_io_expander != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Board is not initialized");
    uint8_t value = s_tca_output_levels;
    if (asserted) {
        value &= (uint8_t)~(1U << ECHOPAL_LCD_RST_PIN);
    } else {
        value |= (uint8_t)(1U << ECHOPAL_LCD_RST_PIN);
    }
    ESP_RETURN_ON_ERROR(tca9554_write(TCA9554_OUTPUT_REG, value), TAG,
                        "Set LCD reset failed");
    s_tca_output_levels = value;
    return ESP_OK;
}

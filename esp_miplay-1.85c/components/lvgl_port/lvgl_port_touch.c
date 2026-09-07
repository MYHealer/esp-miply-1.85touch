#include "lvgl_port.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "echopal_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "LVGL_TOUCH";

#define CST816S_I2C_ADDRESS 0x15  /* Demo uses CST816S at 0x15 */
#define CST816S_DATA_REG    0x02
#define CST816S_CHIP_ID_REG 0xA7
#define CST816S_AUTOSLEEP_REG 0xFE
#define CST816S_INT_GPIO    GPIO_NUM_4
#define TOUCH_SAMPLE_STACK_BYTES (12U * 1024U)
#define TOUCH_PROBE_RETRIES  3
#define TOUCH_PROBE_DELAY_MS 50

static i2c_master_dev_handle_t s_touch_device;
static lv_indev_t *s_touch_indev;
static TaskHandle_t s_touch_task;
static volatile bool s_touch_interrupt_ready;
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_touch_pressed;
static volatile int s_touch_x;
static volatile int s_touch_y;
static uint32_t s_read_failures;
static bool s_read_callback_logged;
static bool s_poll_logged;

static void touch_set_released(void)
{
    bool was_pressed;
    int x;
    int y;
    portENTER_CRITICAL(&s_touch_lock);
    was_pressed = s_touch_pressed;
    x = s_touch_x;
    y = s_touch_y;
    s_touch_pressed = false;
    portEXIT_CRITICAL(&s_touch_lock);
    if (was_pressed) ESP_LOGI(TAG, "touch released at (%d,%d)", x, y);
}

static bool touch_poll(void)
{
    if (!s_poll_logged) {
        s_poll_logged = true;
        ESP_LOGI(TAG, "CST816S direct poll active");
    }

    uint8_t reg = CST816S_DATA_REG;
    uint8_t point[5] = {0};
    if (!s_touch_device) return false;

    esp_err_t ret = i2c_master_transmit_receive(s_touch_device, &reg, 1, point,
                                                sizeof(point), 5);
    if (ret != ESP_OK) {
        touch_set_released();
        if ((++s_read_failures % 100) == 1) {
            ESP_LOGW(TAG, "CST816S read failed: %s", esp_err_to_name(ret));
        }
        return false;
    }
    s_read_failures = 0;

    /* 诊断：按住屏幕时打印原始寄存器字节，定位数据布局 */
    if ((point[0] & 0x0F) > 0 || point[0] > 0) {
        static uint32_t diag_count;
        if ((++diag_count % 50) == 1) {
            ESP_LOGW(TAG, "raw[0..4]: %02X %02X %02X %02X %02X",
                     point[0], point[1], point[2], point[3], point[4]);
        }
    }

    /* CST816S stores the number of active points in the low nibble. */
    if ((point[0] & 0x0F) == 0) {
        touch_set_released();
        return true;
    }

    int x = ((point[1] & 0x0F) << 8) | point[2];
    int y = ((point[3] & 0x0F) << 8) | point[4];
    if (x >= 0 && x < 360 && y >= 0 && y < 360) {
        bool was_pressed;
        portENTER_CRITICAL(&s_touch_lock);
        was_pressed = s_touch_pressed;
        s_touch_x = x;
        s_touch_y = y;
        s_touch_pressed = true;
        portEXIT_CRITICAL(&s_touch_lock);
        if (!was_pressed) ESP_LOGI(TAG, "touch pressed at (%d,%d)", x, y);
    } else {
        touch_set_released();
    }
    return true;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    portENTER_CRITICAL(&s_touch_lock);
    data->continue_reading = false;
    data->point.x = s_touch_x;
    data->point.y = s_touch_y;
    data->state = s_touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&s_touch_lock);

    if (!s_read_callback_logged) {
        s_read_callback_logged = true;
        ESP_LOGI(TAG, "CST816S read callback active (snapshot)");
    }
}

static void IRAM_ATTR touch_interrupt_handler(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_touch_task) {
        vTaskNotifyGiveFromISR(s_touch_task, &higher_priority_task_woken);
        if (higher_priority_task_woken) portYIELD_FROM_ISR();
    }
}

static void touch_sample_task(void *arg)
{
    (void)arg;
    uint32_t loops = 0;
    while (true) {
        /* INT 边沿只用来缩短延迟；无论有无中断都 20ms 轮询一次。
         * CST816T 复位后 INT 线可能保持低电平（开机日志 INT=0），
         * 纯边沿触发会错过第一条边沿导致触摸永远不响应。 */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        touch_poll();
        /* 心跳：10 秒一条，确认任务活着（放在 poll 之后，不阻塞触摸读取） */
        if ((++loops % 500) == 0) {
            uint8_t base_reg = 0x00;
            uint8_t dump[16] = {0};
            esp_err_t err = i2c_master_transmit_receive(s_touch_device, &base_reg, 1,
                                                        dump, sizeof(dump), 20);
            uint8_t pressed_now = gpio_get_level(CST816S_INT_GPIO);
            ESP_LOGI(TAG, "alive loop=%lu i2c=%s INT=%d | 00-07: %02X %02X %02X %02X %02X %02X %02X %02X | 08-0F: %02X %02X %02X %02X %02X %02X %02X %02X",
                     (unsigned long)loops, esp_err_to_name(err), pressed_now,
                     dump[0], dump[1], dump[2], dump[3], dump[4], dump[5], dump[6], dump[7],
                     dump[8], dump[9], dump[10], dump[11], dump[12], dump[13], dump[14], dump[15]);
        }
    }
}

esp_err_t lvgl_port_touch_init(lv_display_t *disp)
{
    if (!disp) return ESP_ERR_INVALID_ARG;
    if (s_touch_indev) return ESP_OK;

    i2c_master_bus_handle_t bus = echopal_board_get_i2c();
    if (!bus) return ESP_ERR_INVALID_STATE;

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816S_I2C_ADDRESS,
        .scl_speed_hz = 400000,  /* 400kHz — match demo */
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &config, &s_touch_device);
    if (ret != ESP_OK) return ret;

    /* Reset touch via TCA9554 EXIO1 (active low).
     * Hold reset low for 20ms (longer than demo's 10ms) to ensure clean boot. */
    echopal_board_set_touch_reset(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    echopal_board_set_touch_reset(false);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Probe CST816S with retries */
    uint8_t chip_id = 0;
    uint8_t id_reg = CST816S_CHIP_ID_REG;
    bool probe_ok = false;
    for (int attempt = 0; attempt < TOUCH_PROBE_RETRIES; attempt++) {
        ret = i2c_master_transmit_receive(s_touch_device, &id_reg, 1, &chip_id, 1, 50);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "CST816S probe OK (chip_id=0x%02X, INT=%d, I2C=0x%02X)",
                     chip_id, gpio_get_level(CST816S_INT_GPIO), CST816S_I2C_ADDRESS);
            probe_ok = true;
            break;
        }
        ESP_LOGW(TAG, "CST816S probe attempt %d/%d failed: %s",
                 attempt + 1, TOUCH_PROBE_RETRIES, esp_err_to_name(ret));
        if (attempt < TOUCH_PROBE_RETRIES - 1) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_PROBE_DELAY_MS));
        }
    }
    if (!probe_ok) {
        ESP_LOGW(TAG, "CST816S probe failed (I2C=0x%02X, SDA=%d, SCL=%d)",
                 CST816S_I2C_ADDRESS, ECHOPAL_I2C_SDA_GPIO, ECHOPAL_I2C_SCL_GPIO);
    } else {
        /* Disable auto-sleep like the demo does (reg 0xFE = 0x01).
         * Without this the chip auto-sleeps after a few idle seconds and
         * stops reporting touch points. */
        const uint8_t autosleep_cfg[2] = {CST816S_AUTOSLEEP_REG, 0x0A};
        esp_err_t as_ret = i2c_master_transmit(s_touch_device, autosleep_cfg,
                                               sizeof(autosleep_cfg), 50);
        if (as_ret != ESP_OK) {
            ESP_LOGW(TAG, "CST816S auto-sleep disable failed: %s", esp_err_to_name(as_ret));
        } else {
            ESP_LOGI(TAG, "CST816S auto-sleep disabled");
        }
    }

    const gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << CST816S_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ret = gpio_config(&int_config);
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(s_touch_device);
        s_touch_device = NULL;
        return ret;
    }

    s_touch_indev = lv_indev_create();
    if (!s_touch_indev) {
        i2c_master_bus_rm_device(s_touch_device);
        s_touch_device = NULL;
        return ESP_FAIL;
    }
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, disp);
    lv_indev_set_read_cb(s_touch_indev, touch_read);

    /* 内部 SRAM 栈（PSRAM 栈 + flash 操作 = 断言崩溃）*/
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        touch_sample_task, "touch_sample", TOUCH_SAMPLE_STACK_BYTES, NULL, 6,
        &s_touch_task, 0);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "CST816S sampling task creation failed");
        return ESP_ERR_NO_MEM;
    }

    ret = gpio_install_isr_service(0);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        ret = gpio_isr_handler_add(CST816S_INT_GPIO, touch_interrupt_handler, NULL);
        if (ret == ESP_OK) {
            s_touch_interrupt_ready = true;
            xTaskNotifyGive(s_touch_task);
        }
    }
    if (!s_touch_interrupt_ready) {
        ESP_LOGW(TAG, "CST816S interrupt unavailable, using background polling: %s",
                 esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "CST816S pointer registered (I2C=0x%02X, INT=%d, event=%d)",
             CST816S_I2C_ADDRESS, CST816S_INT_GPIO, s_touch_interrupt_ready);
    return ESP_OK;
}

bool lvgl_port_touch_get_snapshot(bool *pressed, int *x, int *y)
{
    if (!s_touch_indev || !pressed || !x || !y) return false;
    portENTER_CRITICAL(&s_touch_lock);
    *pressed = s_touch_pressed;
    *x = s_touch_x;
    *y = s_touch_y;
    portEXIT_CRITICAL(&s_touch_lock);
    return true;
}

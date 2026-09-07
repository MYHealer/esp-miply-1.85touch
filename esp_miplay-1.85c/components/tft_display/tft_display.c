/*
 * ST77916 QSPI TFT 显示驱动 — ESP-IDF SPI Master 实现
 * 360x360 RGB565, DMA 传输, 内置5x8 ASCII 字体
 */

#include "tft_display.h"
#include "echopal_board.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "TFT";

#define TFT_QSPI_HOST       SPI2_HOST
#define TFT_PIN_PCLK        40
#define TFT_PIN_DATA0       46
#define TFT_PIN_DATA1       45
#define TFT_PIN_DATA2       42
#define TFT_PIN_DATA3       41
#define TFT_PIN_CS          21
#define TFT_PIN_DC          -1
#define TFT_PIN_RST         -1   /* Reset via TCA9554 EXIO2 */
#define TFT_PIN_BL          5
#define TFT_PIN_TE          18
#define TFT_QSPI_FREQ_HZ    (80 * 1000 * 1000)
/* LVGL flushes at most 360 x 20 RGB565 pixels. */
#define TFT_MAX_TRANSFER_SZ (16 * 1024)

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_bus_ready;
static SemaphoreHandle_t s_lcd_te_semaphore = NULL;
static bool s_lcd_te_initialized = false;

/* ── 5×8 ASCII 字体 (32-126) ── */
static const uint8_t font5x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 (space) */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x00,0x08,0x14,0x22,0x41}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x41,0x22,0x14,0x08,0x00}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x01,0x01}, /* 70 F */
    {0x3E,0x41,0x41,0x51,0x32}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x03,0x04,0x78,0x04,0x03}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
    {0x00,0x00,0x7F,0x41,0x41}, /* 91 [ */
    {0x02,0x04,0x08,0x10,0x20}, /* 92 \ */
    {0x41,0x41,0x7F,0x00,0x00}, /* 93 ] */
    {0x04,0x02,0x01,0x02,0x04}, /* 94 ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* 95 _ */
    {0x00,0x01,0x02,0x04,0x00}, /* 96 ` */
    {0x20,0x54,0x54,0x54,0x78}, /* 97 a */
    {0x7F,0x48,0x44,0x44,0x38}, /* 98 b */
    {0x38,0x44,0x44,0x44,0x20}, /* 99 c */
    {0x38,0x44,0x44,0x48,0x7F}, /* 100 d */
    {0x38,0x54,0x54,0x54,0x18}, /* 101 e */
    {0x08,0x7E,0x09,0x01,0x02}, /* 102 f */
    {0x08,0x14,0x54,0x54,0x3C}, /* 103 g */
    {0x7F,0x08,0x04,0x04,0x78}, /* 104 h */
    {0x00,0x44,0x7D,0x40,0x00}, /* 105 i */
    {0x20,0x40,0x44,0x3D,0x00}, /* 106 j */
    {0x00,0x7F,0x10,0x28,0x44}, /* 107 k */
    {0x00,0x41,0x7F,0x40,0x00}, /* 108 l */
    {0x7C,0x04,0x18,0x04,0x78}, /* 109 m */
    {0x7C,0x08,0x04,0x04,0x78}, /* 110 n */
    {0x38,0x44,0x44,0x44,0x38}, /* 111 o */
    {0x7C,0x14,0x14,0x14,0x08}, /* 112 p */
    {0x08,0x14,0x14,0x18,0x7C}, /* 113 q */
    {0x7C,0x08,0x04,0x04,0x08}, /* 114 r */
    {0x48,0x54,0x54,0x54,0x20}, /* 115 s */
    {0x04,0x3F,0x44,0x40,0x20}, /* 116 t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 117 u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 118 v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 119 w */
    {0x44,0x28,0x10,0x28,0x44}, /* 120 x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 121 y */
    {0x44,0x64,0x54,0x4C,0x44}, /* 122 z */
    {0x00,0x08,0x36,0x41,0x00}, /* 123 { */
    {0x00,0x00,0x7F,0x00,0x00}, /* 124 | */
    {0x00,0x41,0x36,0x08,0x00}, /* 125 } */
    {0x08,0x08,0x2A,0x1C,0x08}, /* 126 ~ */
};

/* ── 公共 API ── */

static void IRAM_ATTR tft_display_te_isr_handler(void *arg)
{
    (void)arg;
    if (s_lcd_te_semaphore != NULL) {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xSemaphoreGiveFromISR(s_lcd_te_semaphore, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

static esp_err_t tft_display_te_init(void)
{
    if (s_lcd_te_initialized) {
        return ESP_OK;
    }

    if (s_lcd_te_semaphore == NULL) {
        s_lcd_te_semaphore = xSemaphoreCreateBinary();
        if (s_lcd_te_semaphore == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const gpio_config_t te_config = {
        .pin_bit_mask = 1ULL << TFT_PIN_TE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&te_config), TAG, "Configure LCD TE GPIO failed");

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if ((isr_ret != ESP_OK) && (isr_ret != ESP_ERR_INVALID_STATE)) {
        return isr_ret;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(TFT_PIN_TE, tft_display_te_isr_handler, NULL), TAG,
                        "Install LCD TE GPIO ISR failed");

    s_lcd_te_initialized = true;
    ESP_LOGI(TAG, "LCD TE synchronization enabled on GPIO%d", TFT_PIN_TE);
    return ESP_OK;
}

bool tft_wait_for_te(void)
{
    if (!s_lcd_te_initialized || (s_lcd_te_semaphore == NULL)) {
        return true;
    }

    /* Discard an edge captured before this frame was ready. */
    while (xSemaphoreTake(s_lcd_te_semaphore, 0) == pdTRUE) {
    }

    /* Keep rendering alive if the panel does not produce TE pulses. */
    return xSemaphoreTake(s_lcd_te_semaphore, pdMS_TO_TICKS(25)) == pdTRUE;
}

void tft_write_rect(int x, int y, int w, int h, const uint16_t *data)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || !data) return;
    if (x + w > TFT_W) w = TFT_W - x;
    if (y + h > TFT_H) h = TFT_H - y;
    if (!s_panel) return;
    esp_lcd_panel_draw_bitmap(s_panel, x + TFT_X_OFFSET, y + TFT_Y_OFFSET,
                              x + w + TFT_X_OFFSET, y + h + TFT_Y_OFFSET,
                              (void *)data);
}

esp_err_t tft_init(void)
{
    ESP_RETURN_ON_ERROR(echopal_board_init(), TAG, "Init board failed");
    /* Reset LCD via TCA9554 EXIO2 (active low) */
    echopal_board_set_lcd_reset(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    echopal_board_set_lcd_reset(false);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = TFT_PIN_PCLK,
        .data0_io_num = TFT_PIN_DATA0,
        .data1_io_num = TFT_PIN_DATA1,
        .data2_io_num = TFT_PIN_DATA2,
        .data3_io_num = TFT_PIN_DATA3,
        .max_transfer_sz = TFT_MAX_TRANSFER_SZ,
    };
    esp_err_t ret = spi_bus_initialize(TFT_QSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_OK) {
        s_bus_ready = true;
    } else if (ret != ESP_ERR_INVALID_STATE || !s_bus_ready) {
        ESP_RETURN_ON_ERROR(ret, TAG, "Init QSPI bus failed");
    };

    esp_err_t te_ret = tft_display_te_init();
    if (te_ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD TE synchronization unavailable: %s", esp_err_to_name(te_ret));
    }

    const ledc_channel_config_t backlight_channel = {
        .gpio_num = TFT_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "Config backlight timer failed");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "Config backlight channel failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = TFT_PIN_CS,
        .dc_gpio_num = TFT_PIN_DC,
        .pclk_hz = TFT_QSPI_FREQ_HZ,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 2,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_QSPI_HOST,
                                                 &io_config, &s_panel_io), TAG,
                        "Create panel IO failed");

    /* Demo ESP32-S3-Touch-LCD-1.85C case-2 panel init table (vendor_specific_init_new).
     * Demo reads panel ID via reg 0x04 at 3MHz, matches (00 02 7F 7F), then applies
     * THIS table. Gamma/GVDD differ from the reference-project table — using the
     * wrong variant gives washed-out / low-contrast output. */
    const st77916_lcd_init_cmd_t init_cmds[] = {
        {0xF0, (uint8_t[]){0x28}, 1, 0},
        {0xF2, (uint8_t[]){0x28}, 1, 0},
        {0x7C, (uint8_t[]){0xD1}, 1, 0},
        {0x83, (uint8_t[]){0xE0}, 1, 0},
        {0x84, (uint8_t[]){0x61}, 1, 0},
        {0xF2, (uint8_t[]){0x82}, 1, 0},
        {0xF0, (uint8_t[]){0x00}, 1, 0},
        {0xF0, (uint8_t[]){0x01}, 1, 0},
        {0xF1, (uint8_t[]){0x01}, 1, 0},
        {0xB0, (uint8_t[]){0x49}, 1, 0},
        {0xB1, (uint8_t[]){0x4A}, 1, 0},
        {0xB2, (uint8_t[]){0x1F}, 1, 0},
        {0xB4, (uint8_t[]){0x46}, 1, 0},
        {0xB5, (uint8_t[]){0x34}, 1, 0},
        {0xB6, (uint8_t[]){0xD5}, 1, 0},
        {0xB7, (uint8_t[]){0x30}, 1, 0},
        {0xB8, (uint8_t[]){0x04}, 1, 0},
        {0xBA, (uint8_t[]){0x00}, 1, 0},
        {0xBB, (uint8_t[]){0x08}, 1, 0},
        {0xBC, (uint8_t[]){0x08}, 1, 0},
        {0xBD, (uint8_t[]){0x00}, 1, 0},
        {0xC0, (uint8_t[]){0x80}, 1, 0},
        {0xC1, (uint8_t[]){0x10}, 1, 0},
        {0xC2, (uint8_t[]){0x37}, 1, 0},
        {0xC3, (uint8_t[]){0x80}, 1, 0},
        {0xC4, (uint8_t[]){0x10}, 1, 0},
        {0xC5, (uint8_t[]){0x37}, 1, 0},
        {0xC6, (uint8_t[]){0xA9}, 1, 0},
        {0xC7, (uint8_t[]){0x41}, 1, 0},
        {0xC8, (uint8_t[]){0x01}, 1, 0},
        {0xC9, (uint8_t[]){0xA9}, 1, 0},
        {0xCA, (uint8_t[]){0x41}, 1, 0},
        {0xCB, (uint8_t[]){0x01}, 1, 0},
        {0xD0, (uint8_t[]){0x91}, 1, 0},
        {0xD1, (uint8_t[]){0x68}, 1, 0},
        {0xD2, (uint8_t[]){0x68}, 1, 0},
        {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
        {0xF1, (uint8_t[]){0x10}, 1, 0},
        {0xF0, (uint8_t[]){0x00}, 1, 0},
        {0xF0, (uint8_t[]){0x02}, 1, 0},
        {0xE0, (uint8_t[]){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
        {0xE1, (uint8_t[]){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
        {0xF0, (uint8_t[]){0x10}, 1, 0},
        {0xF3, (uint8_t[]){0x10}, 1, 0},
        {0xE0, (uint8_t[]){0x08}, 1, 0},
        {0xE1, (uint8_t[]){0x00}, 1, 0},
        {0xE2, (uint8_t[]){0x0B}, 1, 0},
        {0xE3, (uint8_t[]){0x00}, 1, 0},
        {0xE4, (uint8_t[]){0xE0}, 1, 0},
        {0xE5, (uint8_t[]){0x06}, 1, 0},
        {0xE6, (uint8_t[]){0x21}, 1, 0},
        {0xE7, (uint8_t[]){0x00}, 1, 0},
        {0xE8, (uint8_t[]){0x05}, 1, 0},
        {0xE9, (uint8_t[]){0x82}, 1, 0},
        {0xEA, (uint8_t[]){0xDF}, 1, 0},
        {0xEB, (uint8_t[]){0x89}, 1, 0},
        {0xEC, (uint8_t[]){0x20}, 1, 0},
        {0xED, (uint8_t[]){0x14}, 1, 0},
        {0xEE, (uint8_t[]){0xFF}, 1, 0},
        {0xEF, (uint8_t[]){0x00}, 1, 0},
        {0xF8, (uint8_t[]){0xFF}, 1, 0},
        {0xF9, (uint8_t[]){0x00}, 1, 0},
        {0xFA, (uint8_t[]){0x00}, 1, 0},
        {0xFB, (uint8_t[]){0x30}, 1, 0},
        {0xFC, (uint8_t[]){0x00}, 1, 0},
        {0xFD, (uint8_t[]){0x00}, 1, 0},
        {0xFE, (uint8_t[]){0x00}, 1, 0},
        {0xFF, (uint8_t[]){0x00}, 1, 0},
        {0x60, (uint8_t[]){0x42}, 1, 0},
        {0x61, (uint8_t[]){0xE0}, 1, 0},
        {0x62, (uint8_t[]){0x40}, 1, 0},
        {0x63, (uint8_t[]){0x40}, 1, 0},
        {0x64, (uint8_t[]){0x02}, 1, 0},
        {0x65, (uint8_t[]){0x00}, 1, 0},
        {0x66, (uint8_t[]){0x40}, 1, 0},
        {0x67, (uint8_t[]){0x03}, 1, 0},
        {0x68, (uint8_t[]){0x00}, 1, 0},
        {0x69, (uint8_t[]){0x00}, 1, 0},
        {0x6A, (uint8_t[]){0x00}, 1, 0},
        {0x6B, (uint8_t[]){0x00}, 1, 0},
        {0x70, (uint8_t[]){0x42}, 1, 0},
        {0x71, (uint8_t[]){0xE0}, 1, 0},
        {0x72, (uint8_t[]){0x40}, 1, 0},
        {0x73, (uint8_t[]){0x40}, 1, 0},
        {0x74, (uint8_t[]){0x02}, 1, 0},
        {0x75, (uint8_t[]){0x00}, 1, 0},
        {0x76, (uint8_t[]){0x40}, 1, 0},
        {0x77, (uint8_t[]){0x03}, 1, 0},
        {0x78, (uint8_t[]){0x00}, 1, 0},
        {0x79, (uint8_t[]){0x00}, 1, 0},
        {0x7A, (uint8_t[]){0x00}, 1, 0},
        {0x7B, (uint8_t[]){0x00}, 1, 0},
        {0x80, (uint8_t[]){0x38}, 1, 0},
        {0x81, (uint8_t[]){0x00}, 1, 0},
        {0x82, (uint8_t[]){0x04}, 1, 0},
        {0x83, (uint8_t[]){0x02}, 1, 0},
        {0x84, (uint8_t[]){0xDC}, 1, 0},
        {0x85, (uint8_t[]){0x00}, 1, 0},
        {0x86, (uint8_t[]){0x00}, 1, 0},
        {0x87, (uint8_t[]){0x00}, 1, 0},
        {0x88, (uint8_t[]){0x38}, 1, 0},
        {0x89, (uint8_t[]){0x00}, 1, 0},
        {0x8A, (uint8_t[]){0x06}, 1, 0},
        {0x8B, (uint8_t[]){0x02}, 1, 0},
        {0x8C, (uint8_t[]){0xDE}, 1, 0},
        {0x8D, (uint8_t[]){0x00}, 1, 0},
        {0x8E, (uint8_t[]){0x00}, 1, 0},
        {0x8F, (uint8_t[]){0x00}, 1, 0},
        {0x90, (uint8_t[]){0x38}, 1, 0},
        {0x91, (uint8_t[]){0x00}, 1, 0},
        {0x92, (uint8_t[]){0x08}, 1, 0},
        {0x93, (uint8_t[]){0x02}, 1, 0},
        {0x94, (uint8_t[]){0xE0}, 1, 0},
        {0x95, (uint8_t[]){0x00}, 1, 0},
        {0x96, (uint8_t[]){0x00}, 1, 0},
        {0x97, (uint8_t[]){0x00}, 1, 0},
        {0x98, (uint8_t[]){0x38}, 1, 0},
        {0x99, (uint8_t[]){0x00}, 1, 0},
        {0x9A, (uint8_t[]){0x0A}, 1, 0},
        {0x9B, (uint8_t[]){0x02}, 1, 0},
        {0x9C, (uint8_t[]){0xE2}, 1, 0},
        {0x9D, (uint8_t[]){0x00}, 1, 0},
        {0x9E, (uint8_t[]){0x00}, 1, 0},
        {0x9F, (uint8_t[]){0x00}, 1, 0},
        {0xA0, (uint8_t[]){0x38}, 1, 0},
        {0xA1, (uint8_t[]){0x00}, 1, 0},
        {0xA2, (uint8_t[]){0x03}, 1, 0},
        {0xA3, (uint8_t[]){0x02}, 1, 0},
        {0xA4, (uint8_t[]){0xDB}, 1, 0},
        {0xA5, (uint8_t[]){0x00}, 1, 0},
        {0xA6, (uint8_t[]){0x00}, 1, 0},
        {0xA7, (uint8_t[]){0x00}, 1, 0},
        {0xA8, (uint8_t[]){0x38}, 1, 0},
        {0xA9, (uint8_t[]){0x00}, 1, 0},
        {0xAA, (uint8_t[]){0x05}, 1, 0},
        {0xAB, (uint8_t[]){0x02}, 1, 0},
        {0xAC, (uint8_t[]){0xDD}, 1, 0},
        {0xAD, (uint8_t[]){0x00}, 1, 0},
        {0xAE, (uint8_t[]){0x00}, 1, 0},
        {0xAF, (uint8_t[]){0x00}, 1, 0},
        {0xB0, (uint8_t[]){0x38}, 1, 0},
        {0xB1, (uint8_t[]){0x00}, 1, 0},
        {0xB2, (uint8_t[]){0x07}, 1, 0},
        {0xB3, (uint8_t[]){0x02}, 1, 0},
        {0xB4, (uint8_t[]){0xDF}, 1, 0},
        {0xB5, (uint8_t[]){0x00}, 1, 0},
        {0xB6, (uint8_t[]){0x00}, 1, 0},
        {0xB7, (uint8_t[]){0x00}, 1, 0},
        {0xB8, (uint8_t[]){0x38}, 1, 0},
        {0xB9, (uint8_t[]){0x00}, 1, 0},
        {0xBA, (uint8_t[]){0x09}, 1, 0},
        {0xBB, (uint8_t[]){0x02}, 1, 0},
        {0xBC, (uint8_t[]){0xE1}, 1, 0},
        {0xBD, (uint8_t[]){0x00}, 1, 0},
        {0xBE, (uint8_t[]){0x00}, 1, 0},
        {0xBF, (uint8_t[]){0x00}, 1, 0},
        {0xC0, (uint8_t[]){0x22}, 1, 0},
        {0xC1, (uint8_t[]){0xAA}, 1, 0},
        {0xC2, (uint8_t[]){0x65}, 1, 0},
        {0xC3, (uint8_t[]){0x74}, 1, 0},
        {0xC4, (uint8_t[]){0x47}, 1, 0},
        {0xC5, (uint8_t[]){0x56}, 1, 0},
        {0xC6, (uint8_t[]){0x00}, 1, 0},
        {0xC7, (uint8_t[]){0x88}, 1, 0},
        {0xC8, (uint8_t[]){0x99}, 1, 0},
        {0xC9, (uint8_t[]){0x33}, 1, 0},
        {0xD0, (uint8_t[]){0x11}, 1, 0},
        {0xD1, (uint8_t[]){0xAA}, 1, 0},
        {0xD2, (uint8_t[]){0x65}, 1, 0},
        {0xD3, (uint8_t[]){0x74}, 1, 0},
        {0xD4, (uint8_t[]){0x47}, 1, 0},
        {0xD5, (uint8_t[]){0x56}, 1, 0},
        {0xD6, (uint8_t[]){0x00}, 1, 0},
        {0xD7, (uint8_t[]){0x88}, 1, 0},
        {0xD8, (uint8_t[]){0x99}, 1, 0},
        {0xD9, (uint8_t[]){0x33}, 1, 0},
        {0xF3, (uint8_t[]){0x01}, 1, 0},
        {0xF0, (uint8_t[]){0x00}, 1, 0},
        {0x21, (uint8_t[]){0x00}, 0, 0},
        {0x11, (uint8_t[]){0x00}, 0, 120},  /* SLPOUT — 120ms before next command */
        {0x35, (uint8_t[]){0x00}, 1, 0},    /* TE output on — flush uses TE sync */
        {0x29, (uint8_t[]){0x00}, 0, 0},    /* DISPON */
    };
    const st77916_vendor_config_t vendor_config = {
        .init_cmds = init_cmds,
        .init_cmds_size = sizeof(init_cmds) / sizeof(init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_PIN_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
        .flags = {
            .reset_active_high = false,
        },
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_panel_io, &panel_config, &s_panel),
                        TAG, "Create ST77916 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Reset panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Init panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "Turn panel on failed");

    /* Turn backlight on FIRST so diagnostic fill is visible. */
    tft_set_backlight(100);

    /* Diagnostic: fill blue briefly to confirm panel is physically writable. */
    ESP_LOGI(TAG, "LCD DIAGNOSTIC: filling blue...");
    tft_fill_screen(COLOR_BLUE);
    ESP_LOGI(TAG, "LCD DIAGNOSTIC: blue fill sent to panel");
    vTaskDelay(pdMS_TO_TICKS(2000));
    tft_fill_screen(COLOR_BLACK);

    ESP_LOGI(TAG, "ST77916 ready (%dx%d panel, %dx%d UI)",
             TFT_PANEL_W, TFT_PANEL_H, TFT_W, TFT_H);
    return ESP_OK;
}

void tft_set_backlight(uint8_t brightness)
{
    uint32_t duty = ((uint32_t)brightness * 1023U) / 100U;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void tft_fill_screen(uint16_t color)
{
    tft_fill_rect(0, 0, TFT_W, TFT_H, color);
}

void tft_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > TFT_W) w = TFT_W - x;
    if (y + h > TFT_H) h = TFT_H - y;
    if (!s_panel) return;

    uint16_t line[TFT_W];
    for (int i = 0; i < w; ++i) {
        line[i] = __builtin_bswap16(color);
    }
    for (int row = 0; row < h; ++row) {
        esp_lcd_panel_draw_bitmap(s_panel, x + TFT_X_OFFSET, y + row + TFT_Y_OFFSET,
                                  x + w + TFT_X_OFFSET, y + row + 1 + TFT_Y_OFFSET, line);
    }
}

void tft_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= TFT_W || y >= TFT_H) return;
    uint16_t pixel = __builtin_bswap16(color);
    esp_lcd_panel_draw_bitmap(s_panel, x + TFT_X_OFFSET, y + TFT_Y_OFFSET,
                              x + 1 + TFT_X_OFFSET, y + 1 + TFT_Y_OFFSET, &pixel);
}

void tft_draw_hline(int x, int y, int w, uint16_t color)
{
    tft_fill_rect(x, y, w, 1, color);
}

void tft_draw_vline(int x, int y, int h, uint16_t color)
{
    tft_fill_rect(x, y, 1, h, color);
}

void tft_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    tft_draw_hline(x, y, w, color);
    tft_draw_hline(x, y + h - 1, w, color);
    tft_draw_vline(x, y, h, color);
    tft_draw_vline(x + w - 1, y, h, color);
}

int tft_draw_char(int x, int y, char ch, uint16_t color, uint16_t bg, uint8_t size)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font5x8[ch - 32];

    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 8; row++) {
            uint16_t c = (line & 0x01) ? color : bg;
            if (size == 1) {
                tft_draw_pixel(x + col, y + row, c);
            } else {
                tft_fill_rect(x + col * size, y + row * size, size, size, c);
            }
            line >>= 1;
        }
    }
    /* 字符间距1像素 */
    if (bg != color) {
        for (int row = 0; row < 8 * size; row++) {
            tft_draw_pixel(x + 5 * size, y + row, bg);
        }
    }
    return 6 * size;
}

void tft_draw_string(int x, int y, const char *str, uint16_t color, uint16_t bg, uint8_t size)
{
    while (*str) {
        x += tft_draw_char(x, y, *str, color, bg, size);
        str++;
    }
}

void tft_draw_string_truncate(int x, int y, const char *str, int max_width,
                               uint16_t color, uint16_t bg, uint8_t size)
{
    int cx = x;
    while (*str) {
        int char_w = 6 * size;
        if (cx + char_w - x > max_width) break;
        cx += tft_draw_char(cx, y, *str, color, bg, size);
        str++;
    }
    /* 填充剩余区域 */
    if (cx - x < max_width) {
        tft_fill_rect(cx, y, max_width - (cx - x), 8 * size, bg);
    }
}

/* ══════════════════════════════════════════════
 *  Hermes 风格 UI 组件
 * ══════════════════════════════════════════════ */

/* 粗体字符：偏移1px重绘（Hermes embolden 技巧） */
int tft_draw_char_bold(int x, int y, char ch, uint16_t color, uint16_t bg, uint8_t size)
{
    tft_draw_char(x, y, ch, color, bg, size);
    tft_draw_char(x + 1, y, ch, color, bg, size);
    return 6 * size + 1;
}

void tft_draw_string_bold(int x, int y, const char *str, uint16_t color, uint16_t bg, uint8_t size)
{
    while (*str) {
        x += tft_draw_char_bold(x, y, *str, color, bg, size);
        str++;
    }
}

/* Hermes 进度条：细线 + 圆点指示器 */
void tft_draw_progress_bar(int x, int y, int w, int permille)
{
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;

    /* 背景轨道（深灰） */
    tft_draw_hline(x, y + 1, w, COLOR_DARK_GRAY);

    /* 已播放部分（亮色） */
    int filled = w * permille / 1000;
    if (filled > 0) {
        tft_draw_hline(x, y + 1, filled, COLOR_ACCENT);
    }

    /* 圆点指示器（Hermes 风格：3×3 圆） */
    int dot_x = x + filled;
    int dot_y = y;
    tft_fill_rect(dot_x - 1, dot_y, 3, 3, COLOR_WHITE);
}

/* ── 差异更新的缓存状态 ── */
static struct {
    char title[32];
    char subtitle[32];
    int  state;
    int  volume;
    int  position_sec;
    int  duration_sec;
    int  permille;
    char lyric_lines[UI_LYRIC_LINES][LYRIC_MAX_LEN];
    int  lyric_current;
    bool initialized;
} s_prev;

static const char *state_str[] = { "[ ]", ">>", "||" };
static const uint16_t state_clr[] = { COLOR_GRAY, COLOR_GREEN, COLOR_YELLOW };

/* 格式化时间 mm:ss */
static void fmt_time(char *buf, int buflen, int sec)
{
    if (sec < 0) sec = 0;
    snprintf(buf, buflen, "%d:%02d", sec / 60, sec % 60);
}

void tft_ui_init_screen(void)
{
    tft_fill_screen(COLOR_BG);

    /* 分隔线 */
    tft_draw_hline(UI_MARGIN, UI_DIVIDER_Y, TFT_W - UI_MARGIN * 2, COLOR_DARK_GRAY);
    tft_draw_hline(UI_MARGIN, UI_BAR_Y, TFT_W - UI_MARGIN * 2, COLOR_DARK_GRAY);

    /* 初始文字 */
    tft_draw_string(UI_MARGIN, UI_META_Y, "DLNA Player", COLOR_META_TITLE, COLOR_BG, 1);
    tft_draw_string(UI_MARGIN, UI_META_SUB_Y, "Waiting...", COLOR_META_SUB, COLOR_BG, 1);

    memset(&s_prev, 0, sizeof(s_prev));
    s_prev.initialized = true;
}

void tft_ui_update(const tft_ui_state_t *ui)
{
    if (!ui || !s_prev.initialized) return;
    int max_w = TFT_W - UI_MARGIN * 2;

    /* ── 标题 ── */
    const char *title = ui->title ? ui->title : "";
    if (strncmp(s_prev.title, title, sizeof(s_prev.title) - 1) != 0) {
        strncpy(s_prev.title, title, sizeof(s_prev.title) - 1);
        s_prev.title[sizeof(s_prev.title) - 1] = '\0';
        tft_draw_string_truncate(UI_MARGIN, UI_META_Y, title, max_w,
                                  COLOR_META_TITLE, COLOR_BG, 1);
    }

    /* ── 副标题（歌手-专辑）── */
    const char *sub = ui->subtitle ? ui->subtitle : "";
    if (strncmp(s_prev.subtitle, sub, sizeof(s_prev.subtitle) - 1) != 0) {
        strncpy(s_prev.subtitle, sub, sizeof(s_prev.subtitle) - 1);
        s_prev.subtitle[sizeof(s_prev.subtitle) - 1] = '\0';
        tft_draw_string_truncate(UI_MARGIN, UI_META_SUB_Y, sub, max_w,
                                  COLOR_META_SUB, COLOR_BG, 1);
    }

    /* ── 歌词区（Hermes 风格：当前行白+粗体，其余灰色）── */
    if (ui->lyrics) {
        const tft_lyric_t *lyr = ui->lyrics;
        int base_y = UI_LYRIC_START_Y;

        for (int i = 0; i < UI_LYRIC_LINES; i++) {
            /* 计算源歌词行索引（当前行居中显示） */
            int src = lyr->current_line - UI_LYRIC_ACT_IDX + i;
            const char *text = "";
            if (src >= 0 && src < LYRIC_LINES && lyr->lines[src][0]) {
                text = lyr->lines[src];
            }

            /* 检查是否变化 */
            bool changed = (lyr->current_line != s_prev.lyric_current) ||
                           (strcmp(s_prev.lyric_lines[i], text) != 0);
            if (!changed) continue;

            strncpy(s_prev.lyric_lines[i], text, LYRIC_MAX_LEN - 1);
            s_prev.lyric_current = lyr->current_line;

            int ly = base_y + i * UI_LYRIC_LINE_H;
            int max_lw = TFT_W - UI_MARGIN * 2;

            if (i == UI_LYRIC_ACT_IDX && text[0]) {
                /* 当前行：白色粗体（Hermes 风格高亮） */
                tft_draw_string_truncate(UI_MARGIN, ly, text, max_lw,
                                          COLOR_LYRIC_ACT, COLOR_BG, 1);
                /* 粗体效果：再画一遍偏移1px */
                tft_draw_string_truncate(UI_MARGIN + 1, ly, text, max_lw - 1,
                                          COLOR_LYRIC_ACT, COLOR_BG, 1);
            } else {
                /* 非当前行：灰色 */
                tft_draw_string_truncate(UI_MARGIN, ly, text, max_lw,
                                          COLOR_LYRIC_INACT, COLOR_BG, 1);
            }
        }
    }

    /* ── 播放状态 + 音量 ── */
    if (ui->state != s_prev.state || ui->volume != s_prev.volume) {
        s_prev.state = ui->state;
        s_prev.volume = ui->volume;

        int st = (ui->state >= 0 && ui->state <= 2) ? ui->state : 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "%s  VOL:%3d%%", state_str[st], ui->volume);
        tft_draw_string_truncate(UI_MARGIN, UI_STATUS_Y, buf, max_w,
                                  state_clr[st], COLOR_BG, 1);
    }

    /* ── 进度条（Hermes 风格）── */
    int permille = (ui->duration_sec > 0) ?
                   (ui->position_sec * 1000 / ui->duration_sec) : 0;
    if (permille != s_prev.permille || ui->duration_sec != s_prev.duration_sec) {
        s_prev.permille = permille;
        s_prev.duration_sec = ui->duration_sec;
        s_prev.position_sec = ui->position_sec;

        /* 清除旧进度条 */
        tft_fill_rect(UI_MARGIN, UI_PROGRESS_Y, max_w, 4, COLOR_BG);
        tft_draw_progress_bar(UI_MARGIN, UI_PROGRESS_Y, max_w, permille);

        /* 时间标签 */
        char t1[12], t2[12];
        fmt_time(t1, sizeof(t1), ui->position_sec);
        fmt_time(t2, sizeof(t2), ui->duration_sec);
        char time_str[28];
        snprintf(time_str, sizeof(time_str), "%s / %s", t1, t2);
        tft_draw_string_truncate(UI_MARGIN, UI_TIME_Y, time_str, max_w,
                                  COLOR_LIGHT_GRAY, COLOR_BG, 1);
    }
}

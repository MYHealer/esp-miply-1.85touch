/*
 * LVGL 显示驱动 — ST7735S SPI flush 回调
 *
 * 使用整屏双缓冲 + PSRAM，通过 tft_write_rect 写入 SPI
 */

#include "lvgl_port.h"
#include "lvgl.h"
#include "tft_display.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "LVGL_DISP";

/* LVGL 画布放 PSRAM。ESP32-S3 的 GDMA (EDMA) 可以直接读 PSRAM —— demo 就是把
 * LVGL 缓冲指针直接交给 esp_lcd_panel_draw_bitmap 的，无需 SRAM bounce。
 * 1/20 屏小块渲染（demo 同款）：小块 flush 只要几 ms，撕裂不可见。 */
#define DISP_BUF_ROWS  (TFT_H / 20)   /* 18 rows per LVGL draw buffer, like demo */
#define DISP_PIXEL_BYTES 2      /* RGB565 */
#define LVGL_TASK_STACK_BYTES (32U * 1024U)
static EXT_RAM_BSS_ATTR uint8_t s_buf1_data[TFT_W * DISP_BUF_ROWS * DISP_PIXEL_BYTES] __attribute__((aligned(32)));
static EXT_RAM_BSS_ATTR uint8_t s_buf2_data[TFT_W * DISP_BUF_ROWS * DISP_PIXEL_BYTES] __attribute__((aligned(32)));
static lv_display_t *s_disp;

/* ── LVGL 互斥锁 ── */
static SemaphoreHandle_t s_lvgl_mux;

/* ── SPI flush 回调 ── */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);
    /* ST77916 QSPI expects big-endian RGB565; LVGL native is little-endian on
     * ESP32-S3. Swap in place, then hand the PSRAM buffer straight to EDMA. */
    lv_draw_sw_rgb565_swap(px_map, w * h);
    tft_write_rect(area->x1, area->y1, w, h, (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ── LVGL 定时器处理任务 ── */
static void lvgl_tick_task(void *arg)
{
    int tick = 0;
    while (1) {
        lvgl_port_lock();
        lv_timer_handler();
        lvgl_port_unlock();
        if ((++tick % 200) == 0) {
            ESP_LOGI("LVGL_TICK", "alive (heap=%d stack_hwm=%u)",
                     esp_get_free_heap_size(),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ── 初始化 ── */
esp_err_t lvgl_port_init(int task_priority)
{
    /* 互斥锁 */
    s_lvgl_mux = xSemaphoreCreateMutex();
    if (!s_lvgl_mux) return ESP_FAIL;

    /* LVGL 初始化 */
    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);

    /* 注册显示驱动 */
    s_disp = lv_display_create(TFT_W, TFT_H);
    if (!s_disp) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, disp_flush);
    lv_display_set_buffers(s_disp, s_buf1_data, s_buf2_data, sizeof(s_buf1_data),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* CST816S pointer input shares the board I2C bus with the expander/codec. */
    esp_err_t touch_ret = lvgl_port_touch_init(s_disp);
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch input unavailable: %s", esp_err_to_name(touch_ret));
    }

    /* 创建 UI */
    lvgl_port_ui_create();

    /* 创建 LVGL 处理任务 */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        lvgl_tick_task, "lvgl_tick", LVGL_TASK_STACK_BYTES, NULL,
        task_priority, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "lvgl_tick_task PSRAM stack create failed; trying internal stack");
        ret = xTaskCreatePinnedToCore(lvgl_tick_task, "lvgl_tick",
                                      LVGL_TASK_STACK_BYTES, NULL,
                                      task_priority, NULL, 0);
    }
    if (ret != pdPASS) ESP_LOGE(TAG, "lvgl_tick_task create FAILED (heap=%d)", esp_get_free_heap_size());
    else ESP_LOGI(TAG, "lvgl_tick_task created OK");

    ESP_LOGI(TAG, "LVGL ready (%dx%d, %dKB PSRAM canvas, EDMA direct flush)",
             TFT_W, TFT_H,
             (int)(2 * sizeof(s_buf1_data) / 1024));
    return ESP_OK;
}

/* ── 互斥锁 ── */
void lvgl_port_lock(void)
{
    if (s_lvgl_mux) xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);
}

void lvgl_port_unlock(void)
{
    if (s_lvgl_mux) xSemaphoreGive(s_lvgl_mux);
}

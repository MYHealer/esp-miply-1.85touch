/*
 * LVGL UI — 360x360 round display
 * Keep all content inside the circular safe area.
 */

#include "lvgl_port.h"
#include "tft_display.h"
#include "wifi_provision.h"
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#include "lvgl.h"
#include "src/widgets/label/lv_label_private.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

static const char *TAG = "LVGL_UI";

#define LVGL_BTN_ACTION_STACK_BYTES (48U * 1024U)
#define LVGL_WLAN_SCAN_STACK_BYTES  (16U * 1024U)

static void lvgl_ui_delete_current_task(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    StackType_t *stack = NULL;
    StaticTask_t *tcb = NULL;

    if (xTaskGetStaticBuffers(current, &stack, &tcb) == pdTRUE) {
        vTaskDeleteWithCaps(current);
    } else {
        vTaskDelete(NULL);
    }
}

static BaseType_t lvgl_ui_create_task(TaskFunction_t task, const char *name,
                                      uint32_t stack_bytes, void *arg,
                                      UBaseType_t priority, BaseType_t core)
{
    const uint32_t stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(task, name, stack_bytes,
                                                     arg, priority, NULL,
                                                     core, stack_caps);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "Task %s created with PSRAM stack (%u bytes)",
                 name, (unsigned)stack_bytes);
        return ret;
    }

    ESP_LOGW(TAG, "Task %s PSRAM stack creation failed; trying internal stack",
             name);
    return xTaskCreatePinnedToCore(task, name, stack_bytes, arg,
                                   priority, NULL, core);
}

/* 参考项目图片声明 */
LV_IMG_DECLARE(ui_img_haibao_png);
LV_IMG_DECLARE(ui_img_bofang1_png);
LV_IMG_DECLARE(ui_img_zanting1_png);
LV_IMG_DECLARE(ui_img_shangyi1_png);
LV_IMG_DECLARE(ui_img_xiyi1_png);
LV_IMG_DECLARE(ui_img_jiaopian_png);
LV_IMG_DECLARE(ui_img_citou_png);
LV_FONT_DECLARE(lv_font_simsun_16_cjk);

/* Brookesia assets used by the copied system UI. */
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_12);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_16);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_20);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_22);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_24);
LV_FONT_DECLARE(esp_brookesia_font_maison_neue_book_30);

LV_IMG_DECLARE(speaker_image_middle_quick_settings_wifi_48_48);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_wifi_close_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_wifi_level1_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_wifi_level2_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_wifi_level3_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_battery_level1_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_battery_level2_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_battery_level3_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_battery_level4_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_battery_charge_20_20);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_volume_high_48_48);
LV_IMG_DECLARE(speaker_image_middle_quick_settings_brightness_high_48_48);

LV_IMG_DECLARE(esp_brookesia_app_icon_arrow_left_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_arrow_right_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_wireless_wlan_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_media_sound_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_media_display_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_input_touch_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_more_about_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_more_developer_mode_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_more_restart_48_48);
LV_IMG_DECLARE(esp_brookesia_app_icon_wlan_level1_36_36);
LV_IMG_DECLARE(esp_brookesia_app_icon_wlan_level2_36_36);
LV_IMG_DECLARE(esp_brookesia_app_icon_wlan_level3_36_36);
LV_IMG_DECLARE(esp_brookesia_app_icon_wlan_lock_48_48);

/* 控件 */
static lv_obj_t *s_disc_img;            /* 黑胶唱片转盘 */
static lv_obj_t *s_cover_img;
static lv_obj_t *s_cover_container;      /* 圆形裁剪容器 */
static lv_obj_t *s_img_citou;           /* 唱片唱针 */
static lv_img_dsc_t *s_cover_prev = NULL; /* 前一个动态封面，用于释放 */
#define COVER_BUF_SIZE (LVGL_PORT_COVER_SRC_SIZE * LVGL_PORT_COVER_SRC_SIZE)
static EXT_RAM_BSS_ATTR uint16_t s_cover_buf[COVER_BUF_SIZE] __attribute__((aligned(4)));
static lv_obj_t *s_label_title;
static lv_obj_t *s_label_artist;
static lv_obj_t *s_bar_progress;
static lv_obj_t *s_label_time1;
static lv_obj_t *s_label_time2;
static lv_obj_t *s_btn_play;
static lv_obj_t *s_btn_next;
static lv_obj_t *s_btn_prev;

/* 按钮回调存储 */
static lvgl_btn_cb_t s_cb_btn_prev = NULL;
static lvgl_btn_cb_t s_cb_btn_play = NULL;
static lvgl_btn_cb_t s_cb_btn_next = NULL;

/* 按钮动作执行任务（独立栈，不阻塞 LVGL 任务） */
static void _btn_action_task(void *arg)
{
    lvgl_btn_cb_t cb = (lvgl_btn_cb_t)arg;
    if (cb) cb();
    lvgl_ui_delete_current_task();
}

/* 按钮点击事件处理 */
static void _btn_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lvgl_btn_cb_t cb = NULL;
    if (btn == s_btn_prev) cb = s_cb_btn_prev;
    else if (btn == s_btn_play) cb = s_cb_btn_play;
    else if (btn == s_btn_next) cb = s_cb_btn_next;

    /* 在独立任务中执行回调，不阻塞 LVGL 任务 */
    if (cb) {
        lvgl_ui_create_task(_btn_action_task, "btn_act",
                            LVGL_BTN_ACTION_STACK_BYTES, (void*)cb, 6, 1);
    }

}

/* 封面点击 → 切换歌词界面 */
static void _cover_click_cb(lv_event_t *e)
{
    (void)e;
    lvgl_port_ui_toggle_lyrics();
}

/* 主屏幕引用（用于切换回主界面） */
static lv_obj_t *s_main_scr = NULL;

/* 歌词界面 */
static lv_obj_t *s_lyrics_scr = NULL;
static lv_obj_t *s_lyrics_placeholder = NULL;
static lv_obj_t *s_lyrics_prev = NULL;    /* 上一句 */
static lv_obj_t *s_lyrics_curr = NULL;    /* 当前句 */
static lv_obj_t *s_lyrics_next = NULL;    /* 下一句 */
static int s_lyrics_current = -1;
static int s_lyrics_prev_line = -1;        /* 上一次的行号，用于检测切换动画 */
static bool s_lyrics_visible = false;
static bool s_cover_ready = false;         /* 封面图片已加载就绪 */
static lv_obj_t *s_lyrics_bg_img = NULL; /* 歌词界面背景图 */
/* 背景渐变（预抖动图片，消除 RGB565 色阶） */
static lv_obj_t *s_bg_img = NULL;        /* 背景图片控件 */
static lv_img_dsc_t *s_bg_dsc = NULL;    /* 背景图片描述符（PSRAM 像素数据） */
static void _generate_dithered_bg(uint8_t r_top, uint8_t g_top, uint8_t b_top,
                                   uint8_t r_bot, uint8_t g_bot, uint8_t b_bot);

/* Brookesia 360x360 system UI */
#define GESTURE_EDGE_PX       20
#define GESTURE_DISTANCE_PX   20
#define GESTURE_SHORT_MS      800
#define STATUS_PANEL_H         TFT_H
#define STATUS_PANEL_HIDDEN_Y  (-(STATUS_PANEL_H - 20))
#define STATUS_SNAP_SPEED_PX_S 2400U
#define STATUS_SNAP_MIN_MS     80U
#define STATUS_SNAP_MAX_MS     180U
#define RETURN_BAR_W           108
#define RETURN_BAR_H           6
#define RETURN_BAR_Y           (TFT_H - 20)
#define WLAN_SCAN_MAX_ITEMS    12

typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wlan_scan_item_t;

static lv_obj_t *s_status_panel;
static lv_obj_t *s_status_time_label;
static lv_obj_t *s_status_wifi_label;
static lv_obj_t *s_status_battery_icon;
static lv_obj_t *s_status_battery_label;
static lv_obj_t *s_status_wifi_btn;
static lv_obj_t *s_status_volume_btn;
static lv_obj_t *s_status_brightness_btn;
static lv_obj_t *s_status_sram_bar;
static lv_obj_t *s_status_psram_bar;
static lv_obj_t *s_settings_scr;
static lv_obj_t *s_wlan_scr;
static lv_obj_t *s_softap_scr;
static lv_obj_t *s_wlan_connected_label;
static lv_obj_t *s_wlan_available_label;
static lv_obj_t *s_softap_qrcode;
static lv_obj_t *s_softap_info_label;
static lv_obj_t *s_settings_wlan_cell;
static lv_obj_t *s_settings_wlan_status_label;
static lv_obj_t *s_wlan_back_btn;
static lv_obj_t *s_wlan_switch;
static lv_obj_t *s_wlan_softap_cell;
static lv_obj_t *s_wlan_connected_main_label;
static lv_obj_t *s_wlan_connected_minor_label;
static lv_obj_t *s_wlan_connected_signal_icon;
static lv_obj_t *s_wlan_connected_lock_icon;
static lv_obj_t *s_wlan_connected_group;
static lv_obj_t *s_wlan_available_group;
static lv_obj_t *s_wlan_available_container;
static lv_obj_t *s_wlan_provisioning_group;
static lv_obj_t *s_softap_back_btn;
static lv_obj_t *s_softap_content_back_btn;
static lv_obj_t *s_softap_ssid_label;
static lv_obj_t *s_return_bar;
static lv_obj_t *s_return_target;
static lv_obj_t *s_parent_screen;
static lv_timer_t *s_gesture_timer;
static lv_timer_t *s_wifi_status_timer;
static bool s_status_visible;
static bool s_return_visible;
static portMUX_TYPE s_wlan_scan_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR wlan_scan_item_t s_wlan_scan_results[WLAN_SCAN_MAX_ITEMS];
static uint16_t s_wlan_scan_count;
static bool s_wlan_scan_running;
static bool s_wlan_scan_pending;
static lv_font_t s_wlan_ssid_font;

typedef enum {
    GESTURE_MODE_NONE = 0,
    GESTURE_MODE_STATUS_OPEN,
    GESTURE_MODE_STATUS_CLOSE,
    GESTURE_MODE_RETURN,
} gesture_mode_t;

static struct {
    bool active;
    int start_x;
    int start_y;
    int last_x;
    int last_y;
    uint32_t start_tick;
    gesture_mode_t mode;
} s_gesture;

static void _show_settings(void);
static void _show_wlan(void);
static void _show_softap(void);
static void _show_main_screen(void);
static void _show_parent_screen(void);
static void _update_system_status(void);
static void _gesture_timer_cb(lv_timer_t *timer);
static void _wifi_status_timer_cb(lv_timer_t *timer);
static void _request_wlan_scan(void);
static void _apply_wlan_scan_results(void);
static void _show_status_panel(bool visible);
static void _show_return_bar(bool visible);
static void _system_button_event_cb(lv_event_t *event);

/* 当前句滚动状态（LV_LABEL_LONG_SCROLL 内部管理） */

/* 配色 */
#define C_TOP_BAR  lv_color_hex(0x021F33)
#define C_BG_TOP   lv_color_hex(0x0B283D)
#define C_BG_MID   lv_color_hex(0x263D4D)
#define C_BG_BOT   lv_color_hex(0x04263E)
#define C_WHITE    lv_color_hex(0xFFFFFF)
#define C_WHITE80  lv_color_hex(0xCCCCCC)
#define C_DIM      lv_color_hex(0x506070)
#define C_ACCENT   lv_color_hex(0x00CCFF)
#define C_GREEN    lv_color_hex(0x00FF00)
#define C_RED      lv_color_hex(0xFF3034)
#define C_BTN_BG   lv_color_hex(0x1A3A50)

#define UI_CENTER_X       (TFT_W / 2)
#define UI_CENTER_Y       (TFT_H / 2)
#define UI_SAFE_X         30
#define UI_SAFE_W         (TFT_W - UI_SAFE_X * 2)
#define UI_COVER_SIZE     148
#define UI_COVER_ZOOM     768
#define UI_COVER_ART_ZOOM 408
#define UI_BTN_PLAY_SIZE    42
#define UI_BTN_SIDE_SIZE    36
#define UI_BUTTON_GAP       18
#define UI_BUTTON_CLICK_PAD 6
/* 上下首按钮触摸热区更大（36px 按钮太小，手指容易偏） */
#define UI_BUTTON_CLICK_PAD_SIDE 14
#define UI_BUTTON_CENTER_Y  316

static void _style_screen(lv_obj_t *screen)
{
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, TFT_W, TFT_H);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(screen, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(screen, true, 0);
}

static lv_obj_t *_create_text(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

static lv_obj_t *_create_image(lv_obj_t *parent, const lv_img_dsc_t *source,
                               lv_coord_t size, lv_color_t recolor, bool recolor_enabled)
{
    lv_obj_t *image = lv_img_create(parent);
    lv_img_set_src(image, source);
    /* LVGL 9 centers a scaled source inside the configured image box. In
     * LVGL 8, VIRTUAL mode scales that box a second time and clips the image. */
    lv_img_set_pivot(image, source->header.w / 2, source->header.h / 2);
    lv_img_set_zoom(image, (uint16_t)((size * 256U) / source->header.w));
    lv_obj_set_size(image, size, size);
    if (recolor_enabled) {
        lv_obj_set_style_img_recolor(image, recolor, 0);
        lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, 0);
    }
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return image;
}

static void _style_switch(lv_obj_t *sw);

static lv_obj_t *_create_settings_cell(lv_obj_t *parent, const lv_img_dsc_t *left_icon,
                                        const char *left_text, const char *left_minor_text,
                                        const char *right_text, bool arrow, bool switch_control,
                                        bool split_line, lv_obj_t **switch_out,
                                        lv_obj_t **right_label_out)
{
    if (switch_out) *switch_out = NULL;
    if (right_label_out) *right_label_out = NULL;

    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, 256, left_minor_text ? 72 : 48);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(cell, C_WHITE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);

    if (left_text || left_minor_text || left_icon) {
        lv_obj_t *left_area = lv_obj_create(cell);
        lv_obj_remove_style_all(left_area);
        lv_obj_set_width(left_area, LV_SIZE_CONTENT);
        lv_obj_set_height(left_area, lv_obj_get_height(cell));
        lv_obj_align(left_area, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_set_flex_flow(left_area, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(left_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(left_area, 16, 0);
        lv_obj_clear_flag(left_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        if (left_icon) {
            lv_obj_t *icon_area = lv_obj_create(left_area);
            lv_obj_remove_style_all(icon_area);
            lv_obj_set_size(icon_area, 36, 36);
            lv_obj_set_flex_align(icon_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(icon_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *icon_image = _create_image(icon_area, left_icon, 36, C_WHITE, false);
            lv_obj_center(icon_image);
        }

        if (left_text || left_minor_text) {
            lv_obj_t *left_labels = lv_obj_create(left_area);
            lv_obj_remove_style_all(left_labels);
            lv_obj_set_width(left_labels, LV_SIZE_CONTENT);
            lv_obj_set_height(left_labels, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(left_labels, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(left_labels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(left_labels, 8, 0);
            lv_obj_clear_flag(left_labels, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            if (left_text) {
                _create_text(left_labels, left_text, &esp_brookesia_font_maison_neue_book_22, C_WHITE);
            }
            if (left_minor_text) {
                _create_text(left_labels, left_minor_text, &esp_brookesia_font_maison_neue_book_20,
                             lv_color_hex(0xAAAAAA));
            }
        }
    }

    if (right_text || arrow || switch_control) {
        lv_obj_t *right_area = lv_obj_create(cell);
        lv_obj_remove_style_all(right_area);
        lv_obj_set_width(right_area, LV_SIZE_CONTENT);
        lv_obj_set_height(right_area, lv_obj_get_height(cell));
        lv_obj_align(right_area, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_obj_set_flex_flow(right_area, LV_FLEX_FLOW_ROW_REVERSE);
        lv_obj_set_flex_align(right_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(right_area, 8, 0);
        lv_obj_clear_flag(right_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        if (switch_control) {
            lv_obj_t *sw = lv_switch_create(right_area);
            _style_switch(sw);
            if (switch_out) *switch_out = sw;
        }
        if (arrow) {
            lv_obj_t *right_icons = lv_obj_create(right_area);
            lv_obj_remove_style_all(right_icons);
            lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW_REVERSE);
            lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(right_icons, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *icon_area = lv_obj_create(right_icons);
            lv_obj_remove_style_all(icon_area);
            lv_obj_set_size(icon_area, 24, 24);
            lv_obj_clear_flag(icon_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *icon_image = _create_image(icon_area, &esp_brookesia_app_icon_arrow_right_48_48,
                                                  24, C_WHITE, false);
            lv_obj_center(icon_image);
        }
        if (right_text) {
            lv_obj_t *right_label = lv_obj_create(right_area);
            lv_obj_remove_style_all(right_label);
            lv_obj_set_size(right_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(right_label, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(right_label, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(right_label, 4, 0);
            lv_obj_clear_flag(right_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *label = _create_text(right_label, right_text,
                                            &esp_brookesia_font_maison_neue_book_20,
                                            lv_color_hex(0xAAAAAA));
            if (right_label_out) *right_label_out = label;
        }
    }

    if (split_line) {
        static lv_point_precise_t line_points[] = {{72, 0}, {236, 0}};
        lv_obj_t *line = lv_line_create(cell);
        lv_line_set_points(line, line_points, 2);
        lv_obj_align(line, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_color(line, C_WHITE, 0);
        lv_obj_set_style_line_opa(line, (lv_opa_t)64, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    return cell;
}

static const lv_img_dsc_t *_wlan_signal_icon(int rssi)
{
    if (rssi >= -50) return &esp_brookesia_app_icon_wlan_level3_36_36;
    if (rssi >= -70) return &esp_brookesia_app_icon_wlan_level2_36_36;
    return &esp_brookesia_app_icon_wlan_level1_36_36;
}

static lv_obj_t *_create_wlan_network_cell(lv_obj_t *parent, const char *ssid,
                                            const char *minor_text, int rssi, bool locked,
                                            bool split_line, bool clickable,
                                            lv_obj_t **main_label_out,
                                            lv_obj_t **minor_label_out,
                                            lv_obj_t **signal_icon_out,
                                            lv_obj_t **lock_icon_out)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, 256, minor_text ? 72 : 48);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell, C_WHITE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    if (clickable) lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *left_area = lv_obj_create(cell);
    lv_obj_remove_style_all(left_area);
    lv_obj_set_size(left_area, 200, lv_obj_get_height(cell));
    lv_obj_align(left_area, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_flex_flow(left_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left_area, 8, 0);
    lv_obj_clear_flag(left_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_label = _create_text(left_area, ssid ? ssid : "",
                                        &s_wlan_ssid_font, C_WHITE);
    lv_obj_set_size(main_label, 200, 24);
    lv_label_set_long_mode(main_label, LV_LABEL_LONG_SCROLL);
    lv_obj_t *minor_label = NULL;
    if (minor_text) {
        minor_label = _create_text(left_area, minor_text,
                                   &esp_brookesia_font_maison_neue_book_20,
                                   lv_color_hex(0xAAAAAA));
    }

    lv_obj_t *right_area = lv_obj_create(cell);
    lv_obj_remove_style_all(right_area);
    lv_obj_set_size(right_area, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(right_area, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_flex_flow(right_area, LV_FLEX_FLOW_ROW_REVERSE);
    lv_obj_set_flex_align(right_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_area, 8, 0);
    lv_obj_clear_flag(right_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *signal_icon = _create_image(right_area, _wlan_signal_icon(rssi), 24,
                                          C_WHITE, false);
    lv_obj_t *lock_icon = _create_image(right_area, &esp_brookesia_app_icon_wlan_lock_48_48,
                                        24, C_WHITE, false);
    if (!locked) lv_obj_add_flag(lock_icon, LV_OBJ_FLAG_HIDDEN);

    if (split_line) {
        static lv_point_precise_t line_points[] = {{20, 0}, {236, 0}};
        lv_obj_t *line = lv_line_create(cell);
        lv_line_set_points(line, line_points, 2);
        lv_obj_align(line, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_color(line, C_WHITE, 0);
        lv_obj_set_style_line_opa(line, (lv_opa_t)64, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    if (main_label_out) *main_label_out = main_label;
    if (minor_label_out) *minor_label_out = minor_label;
    if (signal_icon_out) *signal_icon_out = signal_icon;
    if (lock_icon_out) *lock_icon_out = lock_icon;
    lv_obj_update_layout(cell);
    lv_obj_align(left_area, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_align(right_area, LV_ALIGN_RIGHT_MID, -20, 0);
    return cell;
}

static lv_obj_t *_create_settings_group(lv_obj_t *parent, const char *title, lv_obj_t **main_out)
{
    lv_obj_t *main = lv_obj_create(parent);
    lv_obj_remove_style_all(main);
    lv_obj_set_width(main, 288);
    lv_obj_set_height(main, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(main, 8, 0);
    lv_obj_clear_flag(main, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = _create_text(main, title ? title : "",
                                          &esp_brookesia_font_maison_neue_book_20,
                                          lv_color_hex(0xAAAAAA));
    if (!title || title[0] == '\0') lv_obj_add_flag(title_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *container = lv_obj_create(main);
    lv_obj_remove_style_all(container);
    lv_obj_set_width(container, 288);
    lv_obj_set_height(container, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(container, 16, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x38393A), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(container, 8, 0);
    lv_obj_set_style_pad_bottom(container, 8, 0);
    lv_obj_set_style_pad_left(container, 16, 0);
    lv_obj_set_style_pad_right(container, 16, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    if (main_out) *main_out = main;
    return container;
}

static lv_obj_t *_create_header_button(lv_obj_t *screen, const char *parent_title)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, TFT_W, 48);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *button = lv_obj_create(header);
    lv_obj_remove_style_all(button);
    lv_obj_set_width(button, LV_SIZE_CONTENT);
    lv_obj_set_height(button, 48);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(button, 0, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);

    lv_obj_t *icon_area = lv_obj_create(button);
    lv_obj_remove_style_all(icon_area);
    lv_obj_set_size(icon_area, 24, 24);
    lv_obj_clear_flag(icon_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *icon_image = _create_image(icon_area, &esp_brookesia_app_icon_arrow_left_48_48,
                                          24, C_RED, true);
    lv_obj_center(icon_image);
    _create_text(button, parent_title, &esp_brookesia_font_maison_neue_book_24, C_RED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(header);
    lv_obj_center(button);
    return button;
}

static void _style_switch(lv_obj_t *sw)
{
    lv_obj_set_size(sw, 64, 32);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xAAAAAA), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, C_RED, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_width(sw, 26, LV_PART_KNOB);
    lv_obj_set_style_height(sw, 26, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, C_WHITE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 16, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 16, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, 13, LV_PART_KNOB);
}

static lv_obj_t *_create_quick_button(lv_obj_t *parent, const lv_img_dsc_t *icon_source,
                                      const char *caption, bool checkable)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_width(button, LV_SIZE_CONTENT);
    lv_obj_set_height(button, LV_SIZE_CONTENT);
    lv_obj_set_x(button, -97);
    lv_obj_set_y(button, 20);
    lv_obj_set_align(button, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(button, 10, 0);
    lv_obj_set_style_pad_column(button, 0, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_obj_create(button);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 80, 80);
    lv_obj_set_align(icon, LV_ALIGN_TOP_MID);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    if (checkable) lv_obj_add_flag(icon, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_radius(icon, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(0x38393A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_img_src(icon, icon_source, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, C_RED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    if (checkable) {
        lv_obj_set_style_bg_color(icon, C_RED, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
    }

    lv_obj_t *caption_label = _create_text(button, caption, &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    lv_obj_set_style_pad_bottom(caption_label, 2, 0);
    return icon;
}

static void _system_overlay_y_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void _system_overlay_hide_ready_cb(lv_anim_t *anim)
{
    if (anim && anim->var) lv_obj_add_flag((lv_obj_t *)anim->var, LV_OBJ_FLAG_HIDDEN);
}

static void _animate_system_overlay(lv_obj_t *obj, lv_coord_t from, lv_coord_t to,
                                    bool hide_after)
{
    if (!obj) return;
    lv_anim_del(obj, _system_overlay_y_anim_cb);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, _system_overlay_y_anim_cb);
    lv_anim_set_values(&anim, from, to);
    uint32_t distance = (uint32_t)abs(to - from);
    uint32_t duration_ms = distance * 1000U / STATUS_SNAP_SPEED_PX_S;
    if (duration_ms < STATUS_SNAP_MIN_MS) duration_ms = STATUS_SNAP_MIN_MS;
    if (duration_ms > STATUS_SNAP_MAX_MS) duration_ms = STATUS_SNAP_MAX_MS;
    lv_anim_set_duration(&anim, duration_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    if (hide_after) lv_anim_set_ready_cb(&anim, _system_overlay_hide_ready_cb);
    lv_anim_start(&anim);
}

static void _show_status_panel(bool visible)
{
    if (!s_status_panel) return;
    if (visible) {
        s_status_visible = true;
        lv_anim_del(s_status_panel, _system_overlay_y_anim_cb);
        lv_obj_clear_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_status_panel);
        _animate_system_overlay(s_status_panel, lv_obj_get_y(s_status_panel), 0, false);
        _update_system_status();
    } else {
        s_status_visible = false;
        if (lv_obj_has_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN)) return;
        _animate_system_overlay(s_status_panel, lv_obj_get_y(s_status_panel),
                                STATUS_PANEL_HIDDEN_Y, true);
    }
}

static void _show_return_bar(bool visible)
{
    if (!s_return_bar) return;
    s_return_visible = visible;
    if (visible) {
        lv_obj_clear_flag(s_return_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_return_bar);
        lv_obj_set_width(s_return_bar, RETURN_BAR_W);
        lv_obj_set_x(s_return_bar, (TFT_W - RETURN_BAR_W) / 2);
    } else {
        lv_obj_add_flag(s_return_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_return_bar, RETURN_BAR_W);
        lv_obj_set_x(s_return_bar, (TFT_W - RETURN_BAR_W) / 2);
    }
}

static void _create_system_ui(void)
{
    lv_obj_t *layer = lv_layer_top();
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

    s_status_panel = lv_obj_create(layer);
    lv_obj_remove_style_all(s_status_panel);
    lv_obj_set_size(s_status_panel, TFT_W, STATUS_PANEL_H);
    lv_obj_set_pos(s_status_panel, -1, STATUS_PANEL_HIDDEN_Y);
    lv_obj_set_flex_flow(s_status_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_status_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_status_panel, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(s_status_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_status_panel, 0, 0);
    lv_obj_set_style_pad_right(s_status_panel, 0, 0);
    lv_obj_set_style_pad_top(s_status_panel, 30, 0);
    lv_obj_set_style_pad_bottom(s_status_panel, 0, 0);
    lv_obj_set_style_clip_corner(s_status_panel, true, 0);

    /* This hierarchy mirrors the reference QuickSettings component. */
    lv_obj_t *status = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(status);
    lv_obj_set_width(status, lv_pct(100));
    lv_obj_set_height(status, LV_SIZE_CONTENT);
    lv_obj_set_x(status, 1);
    lv_obj_set_y(status, -1);
    lv_obj_set_align(status, LV_ALIGN_TOP_MID);
    lv_obj_set_style_pad_left(status, 0, 0);
    lv_obj_set_style_pad_right(status, 0, 0);
    lv_obj_set_style_pad_top(status, 0, 0);
    lv_obj_set_style_pad_bottom(status, 20, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *status_internal = lv_obj_create(status);
    lv_obj_remove_style_all(status_internal);
    lv_obj_set_width(status_internal, lv_pct(47));
    lv_obj_set_height(status_internal, LV_SIZE_CONTENT);
    lv_obj_set_align(status_internal, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_flex_flow(status_internal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_internal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_internal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *status_top = lv_obj_create(status_internal);
    lv_obj_remove_style_all(status_top);
    lv_obj_set_width(status_top, lv_pct(97));
    lv_obj_set_height(status_top, LV_SIZE_CONTENT);
    lv_obj_set_x(status_top, -1);
    lv_obj_set_align(status_top, LV_ALIGN_CENTER);
    lv_obj_clear_flag(status_top, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_status_time_label = _create_text(status_top, "12:00 AM",
                                       &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    lv_obj_set_align(s_status_time_label, LV_ALIGN_LEFT_MID);
    lv_obj_set_x(s_status_time_label, -1);

    lv_obj_t *status_right = lv_obj_create(status_top);
    lv_obj_remove_style_all(status_right);
    lv_obj_set_width(status_right, LV_SIZE_CONTENT);
    lv_obj_set_height(status_right, LV_SIZE_CONTENT);
    lv_obj_set_align(status_right, LV_ALIGN_RIGHT_MID);
    lv_obj_set_flex_flow(status_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(status_right, 0, 0);
    lv_obj_set_style_pad_column(status_right, 5, 0);
    lv_obj_clear_flag(status_right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_status_wifi_label = _create_image(status_right,
                                        &speaker_image_middle_quick_settings_wifi_close_20_20,
                                        20, C_WHITE, false);
    s_status_battery_icon = _create_image(status_right,
                                          &speaker_image_middle_quick_settings_battery_charge_20_20,
                                          20, C_WHITE, false);
    s_status_battery_label = _create_text(status_right, "100%",
                                          &esp_brookesia_font_maison_neue_book_12, C_WHITE);
    lv_obj_set_style_pad_left(s_status_battery_label, -1, 0);
    lv_obj_set_style_pad_right(s_status_battery_label, 0, 0);
    lv_obj_set_style_pad_top(s_status_battery_label, 0, 0);
    lv_obj_set_style_pad_bottom(s_status_battery_label, -1, 0);

    lv_obj_t *status_bottom = lv_obj_create(status_internal);
    lv_obj_remove_style_all(status_bottom);
    lv_obj_set_width(status_bottom, lv_pct(100));
    lv_obj_set_height(status_bottom, 22);
    lv_obj_set_align(status_bottom, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(status_bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(status_bottom, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status_bottom, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    _create_text(status_bottom, "5/27", &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    _create_text(status_bottom, "Wednesday", &esp_brookesia_font_maison_neue_book_16, C_WHITE);

    lv_obj_t *buttons = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_width(buttons, lv_pct(79));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_align(buttons, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(buttons, 20, 0);
    lv_obj_set_style_pad_column(buttons, 10, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_status_wifi_btn = _create_quick_button(
        buttons, &speaker_image_middle_quick_settings_wifi_48_48, "Wi-Fi", true);
    s_status_volume_btn = _create_quick_button(
        buttons, &speaker_image_middle_quick_settings_volume_high_48_48, "Volume", false);
    s_status_brightness_btn = _create_quick_button(
        buttons, &speaker_image_middle_quick_settings_brightness_high_48_48, "Brightness", false);
    lv_obj_add_event_cb(s_status_wifi_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_status_volume_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_status_brightness_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *memory = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(memory);
    lv_obj_set_width(memory, lv_pct(100));
    lv_obj_set_height(memory, LV_SIZE_CONTENT);
    lv_obj_set_align(memory, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(memory, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(memory, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(memory, 0, 0);
    lv_obj_set_style_pad_right(memory, 0, 0);
    lv_obj_set_style_pad_top(memory, 80, 0);
    lv_obj_set_style_pad_bottom(memory, 40, 0);
    lv_obj_set_style_pad_row(memory, 5, 0);
    lv_obj_clear_flag(memory, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *memory_internal = lv_obj_create(memory);
    lv_obj_remove_style_all(memory_internal);
    lv_obj_set_width(memory_internal, 216);
    lv_obj_set_height(memory_internal, LV_SIZE_CONTENT);
    lv_obj_set_align(memory_internal, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(memory_internal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(memory_internal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(memory_internal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sram = lv_obj_create(memory_internal);
    lv_obj_remove_style_all(sram);
    lv_obj_set_width(sram, lv_pct(100));
    lv_obj_set_height(sram, LV_SIZE_CONTENT);
    lv_obj_set_align(sram, LV_ALIGN_RIGHT_MID);
    lv_obj_clear_flag(sram, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    _create_text(sram, "  SRAM:", &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    s_status_sram_bar = lv_bar_create(sram);
    lv_bar_set_value(s_status_sram_bar, 50, LV_ANIM_OFF);
    lv_bar_set_start_value(s_status_sram_bar, 0, LV_ANIM_OFF);
    lv_obj_set_height(s_status_sram_bar, 10);
    lv_obj_set_width(s_status_sram_bar, lv_pct(70));
    lv_obj_set_align(s_status_sram_bar, LV_ALIGN_RIGHT_MID);

    lv_obj_t *psram = lv_obj_create(memory_internal);
    lv_obj_remove_style_all(psram);
    lv_obj_set_width(psram, lv_pct(100));
    lv_obj_set_height(psram, LV_SIZE_CONTENT);
    lv_obj_set_align(psram, LV_ALIGN_CENTER);
    lv_obj_clear_flag(psram, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    _create_text(psram, "PSRAM:", &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    s_status_psram_bar = lv_bar_create(psram);
    lv_bar_set_value(s_status_psram_bar, 50, LV_ANIM_OFF);
    lv_bar_set_start_value(s_status_psram_bar, 0, LV_ANIM_OFF);
    lv_obj_set_height(s_status_psram_bar, 10);
    lv_obj_set_width(s_status_psram_bar, lv_pct(70));
    lv_obj_set_align(s_status_psram_bar, LV_ALIGN_RIGHT_MID);

    lv_obj_t *memory_bars[] = {s_status_sram_bar, s_status_psram_bar};
    for (size_t i = 0; i < sizeof(memory_bars) / sizeof(memory_bars[0]); ++i) {
        lv_obj_set_style_bg_color(memory_bars[i], lv_color_hex(0x38393A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(memory_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(memory_bars[i], C_GREEN, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(memory_bars[i], lv_color_hex(0xFF0000), LV_PART_INDICATOR);
        lv_obj_set_style_bg_main_stop(memory_bars[i], 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_stop(memory_bars[i], 255, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(memory_bars[i], LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
        lv_obj_set_style_radius(memory_bars[i], 5, LV_PART_MAIN);
        lv_obj_set_style_radius(memory_bars[i], 5, LV_PART_INDICATOR);
    }

    s_return_bar = lv_bar_create(layer);
    lv_obj_set_size(s_return_bar, RETURN_BAR_W, RETURN_BAR_H);
    lv_obj_set_pos(s_return_bar, (TFT_W - RETURN_BAR_W) / 2, RETURN_BAR_Y);
    lv_bar_set_range(s_return_bar, 0, 100);
    lv_bar_set_value(s_return_bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_return_bar, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_return_bar, C_WHITE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_return_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_return_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_return_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_return_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_return_bar, 5, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_return_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_status_visible = false;
    s_return_visible = false;
    s_gesture.mode = GESTURE_MODE_NONE;
    lv_obj_add_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_return_bar, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *_create_settings_content(lv_obj_t *screen)
{
    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, TFT_W, TFT_H - 58);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_top(content, 16, 0);
    lv_obj_set_style_pad_bottom(content, 76, 0);
    lv_obj_set_style_pad_left(content, 0, 0);
    lv_obj_set_style_pad_right(content, 0, 0);
    lv_obj_set_style_pad_row(content, 16, 0);
    lv_obj_set_style_clip_corner(content, true, 0);
    return content;
}

static void _create_settings_screen(void)
{
    s_settings_scr = lv_obj_create(NULL);
    _style_screen(s_settings_scr);

    lv_obj_t *content = _create_settings_content(s_settings_scr);
    lv_obj_t *wireless = _create_settings_group(content, "Wireless", NULL);
    s_settings_wlan_cell = _create_settings_cell(
        wireless, &esp_brookesia_app_icon_wireless_wlan_48_48, "WLAN", NULL, "Off", true,
        false, false, NULL, &s_settings_wlan_status_label);
    lv_obj_add_event_cb(s_settings_wlan_cell, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *media = _create_settings_group(content, "Media", NULL);
    lv_obj_t *sound = _create_settings_cell(
        media, &esp_brookesia_app_icon_media_sound_48_48, "Sound", NULL, NULL, true, false, true,
        NULL, NULL);
    lv_obj_t *display = _create_settings_cell(
        media, &esp_brookesia_app_icon_media_display_48_48, "Display", NULL, NULL, true, false, false,
        NULL, NULL);
    lv_obj_add_event_cb(sound, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(display, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *input = _create_settings_group(content, "Input", NULL);
    lv_obj_t *touch = _create_settings_cell(
        input, &esp_brookesia_app_icon_input_touch_48_48, "Touch", NULL, NULL, false, false, false,
        NULL, NULL);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *more = _create_settings_group(content, "More", NULL);
    lv_obj_t *about = _create_settings_cell(
        more, &esp_brookesia_app_icon_more_about_48_48, "About", NULL, NULL, true, false, true,
        NULL, NULL);
    lv_obj_t *developer = _create_settings_cell(
        more, &esp_brookesia_app_icon_more_developer_mode_48_48, "Developer Mode", NULL, NULL, false,
        false, true, NULL, NULL);
    lv_obj_t *restore = _create_settings_cell(
        more, &esp_brookesia_app_icon_more_restart_48_48, "Restore Factory", NULL, NULL, false, false,
        false, NULL, NULL);
    lv_obj_add_event_cb(about, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(developer, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(restore, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
}

static void _create_wlan_screen(void)
{
    s_wlan_scr = lv_obj_create(NULL);
    _style_screen(s_wlan_scr);
    s_wlan_back_btn = _create_header_button(s_wlan_scr, "Settings");
    lv_obj_add_event_cb(s_wlan_back_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *content = _create_settings_content(s_wlan_scr);
    lv_obj_t *control = _create_settings_group(content, "", NULL);
    lv_obj_t *control_cell = _create_settings_cell(
        control, NULL, "WLAN", NULL, NULL, false, true, false, &s_wlan_switch, NULL);
    lv_obj_clear_flag(control_cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wlan_switch, _system_button_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *provisioning = _create_settings_group(content, "Provisioning", NULL);
    s_wlan_softap_cell = _create_settings_cell(
        provisioning, NULL, "SoftAP Mode", NULL, NULL, true, false, false, NULL, NULL);
    lv_obj_add_event_cb(s_wlan_softap_cell, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *connected = _create_settings_group(content, "Connected network", &s_wlan_connected_group);
    _create_wlan_network_cell(connected, "", "", -100, false, false, false,
                              &s_wlan_connected_main_label,
                              &s_wlan_connected_minor_label,
                              &s_wlan_connected_signal_icon,
                              &s_wlan_connected_lock_icon);
    s_wlan_connected_label = s_wlan_connected_main_label;

    s_wlan_available_container = _create_settings_group(
        content, "Available networks", &s_wlan_available_group);
    s_wlan_available_label = NULL;
    lv_obj_add_flag(s_wlan_connected_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wlan_available_group, LV_OBJ_FLAG_HIDDEN);
}

static void _create_softap_screen(void)
{
    s_softap_scr = lv_obj_create(NULL);
    _style_screen(s_softap_scr);
    s_softap_back_btn = _create_header_button(s_softap_scr, "WLAN");
    lv_obj_add_event_cb(s_softap_back_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *content = _create_settings_content(s_softap_scr);
    lv_obj_t *group = _create_settings_group(content, "", NULL);
    lv_obj_t *qr_cell = lv_obj_create(group);
    lv_obj_remove_style_all(qr_cell);
    lv_obj_set_width(qr_cell, 256);
    lv_obj_set_height(qr_cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(qr_cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qr_cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(qr_cell, 10, 0);
    lv_obj_set_style_pad_row(qr_cell, 10, 0);
    lv_obj_clear_flag(qr_cell, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_softap_qrcode = lv_qrcode_create(qr_cell);
    lv_qrcode_set_size(s_softap_qrcode, 100);
    lv_qrcode_set_dark_color(s_softap_qrcode, lv_color_black());
    lv_qrcode_set_light_color(s_softap_qrcode, lv_color_white());
    lv_obj_set_style_border_color(s_softap_qrcode, C_WHITE, 0);
    lv_obj_set_style_border_width(s_softap_qrcode, 10, 0);
    lv_qrcode_update(s_softap_qrcode, "WIFI:T:nopass;S:ESP-Brookesia;P:;;",
                     strlen("WIFI:T:nopass;S:ESP-Brookesia;P:;;"));
    s_softap_info_label = _create_text(qr_cell, "", &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    lv_obj_set_width(s_softap_info_label, 280);
    lv_obj_set_style_text_align(s_softap_info_label, LV_TEXT_ALIGN_CENTER, 0);
    s_softap_content_back_btn = lv_btn_create(qr_cell);
    lv_obj_set_size(s_softap_content_back_btn, 120, 44);
    lv_obj_set_style_radius(s_softap_content_back_btn, 8, 0);
    lv_obj_set_style_pad_top(s_softap_content_back_btn, 6, 0);
    lv_obj_set_style_pad_bottom(s_softap_content_back_btn, 6, 0);
    lv_obj_set_style_pad_left(s_softap_content_back_btn, 12, 0);
    lv_obj_set_style_pad_right(s_softap_content_back_btn, 12, 0);
    lv_obj_t *back_label = _create_text(s_softap_content_back_btn, "back",
                                        &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(s_softap_content_back_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
}

static void _wlan_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t config = {
        .show_hidden = false,
    };
    wifi_ap_record_t records[WLAN_SCAN_MAX_ITEMS] = {0};
    uint16_t record_count = WLAN_SCAN_MAX_ITEMS;
    esp_err_t ret = esp_wifi_scan_start(&config, true);
    if (ret == ESP_OK) ret = esp_wifi_scan_get_ap_records(&record_count, records);

    wlan_scan_item_t results[WLAN_SCAN_MAX_ITEMS] = {0};
    uint16_t result_count = 0;
    if (ret == ESP_OK) {
        for (uint16_t i = 0; i < record_count && result_count < WLAN_SCAN_MAX_ITEMS; i++) {
            const char *ssid = (const char *)records[i].ssid;
            if (!ssid[0]) continue;
            bool duplicate = false;
            for (uint16_t j = 0; j < result_count; j++) {
                if (strcmp(results[j].ssid, ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            snprintf(results[result_count].ssid, sizeof(results[result_count].ssid), "%s", ssid);
            results[result_count].rssi = records[i].rssi;
            results[result_count].authmode = records[i].authmode;
            result_count++;
        }
    } else {
        ESP_LOGW(TAG, "WLAN scan failed: %s", esp_err_to_name(ret));
    }

    portENTER_CRITICAL(&s_wlan_scan_lock);
    memcpy(s_wlan_scan_results, results, sizeof(results));
    s_wlan_scan_count = result_count;
    s_wlan_scan_pending = true;
    s_wlan_scan_running = false;
    portEXIT_CRITICAL(&s_wlan_scan_lock);
    lvgl_ui_delete_current_task();
}

static void _request_wlan_scan(void)
{
    bool start_task = false;
    portENTER_CRITICAL(&s_wlan_scan_lock);
    if (!s_wlan_scan_running) {
        s_wlan_scan_running = true;
        start_task = true;
    }
    portEXIT_CRITICAL(&s_wlan_scan_lock);
    if (!start_task) return;

    if (lvgl_ui_create_task(_wlan_scan_task, "wlan_scan",
                            LVGL_WLAN_SCAN_STACK_BYTES, NULL, 4, 1) != pdPASS) {
        portENTER_CRITICAL(&s_wlan_scan_lock);
        s_wlan_scan_running = false;
        portEXIT_CRITICAL(&s_wlan_scan_lock);
        ESP_LOGE(TAG, "WLAN scan task creation failed");
    }
}

static void _apply_wlan_scan_results(void)
{
    wlan_scan_item_t results[WLAN_SCAN_MAX_ITEMS];
    uint16_t count = 0;
    bool pending = false;
    portENTER_CRITICAL(&s_wlan_scan_lock);
    if (s_wlan_scan_pending) {
        memcpy(results, s_wlan_scan_results, sizeof(results));
        count = s_wlan_scan_count;
        s_wlan_scan_pending = false;
        pending = true;
    }
    portEXIT_CRITICAL(&s_wlan_scan_lock);
    if (!pending || !s_wlan_available_container || !s_wlan_available_group) return;

    lv_obj_clean(s_wlan_available_container);
    for (uint16_t i = 0; i < count; i++) {
        _create_wlan_network_cell(s_wlan_available_container, results[i].ssid, NULL,
                                  results[i].rssi,
                                  results[i].authmode != WIFI_AUTH_OPEN,
                                  i + 1 < count, true, NULL, NULL, NULL, NULL);
    }
    if (count) lv_obj_clear_flag(s_wlan_available_group, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_wlan_available_group, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "WLAN scan UI updated: %u networks", (unsigned)count);
}

static void _update_system_status(void)
{
    if (!s_status_time_label) return;

    time_t now = time(NULL);
    struct tm local_time = {0};
    if (localtime_r(&now, &local_time) == NULL) memset(&local_time, 0, sizeof(local_time));
    int hour = local_time.tm_hour % 12;
    if (hour == 0) hour = 12;
    lv_label_set_text_fmt(s_status_time_label, "%02d:%02d %s", hour, local_time.tm_min,
                          local_time.tm_hour >= 12 ? "PM" : "AM");

    wifi_ap_record_t ap = {0};
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    wifi_mode_t wifi_mode = WIFI_MODE_NULL;
    bool wlan_on = esp_wifi_get_mode(&wifi_mode) == ESP_OK && wifi_mode != WIFI_MODE_NULL;
    const lv_img_dsc_t *wifi_icon = &speaker_image_middle_quick_settings_wifi_close_20_20;
    if (connected) {
        if (ap.rssi >= -50) {
            wifi_icon = &speaker_image_middle_quick_settings_wifi_level3_20_20;
        } else if (ap.rssi >= -70) {
            wifi_icon = &speaker_image_middle_quick_settings_wifi_level2_20_20;
        } else {
            wifi_icon = &speaker_image_middle_quick_settings_wifi_level1_20_20;
        }
    }
    lv_img_set_src(s_status_wifi_label, wifi_icon);
    if (connected) {
        lv_obj_add_state(s_status_wifi_btn, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_status_wifi_btn, LV_STATE_CHECKED);
    }
    lv_img_set_src(s_status_battery_icon,
                   &speaker_image_middle_quick_settings_battery_charge_20_20);
    lv_label_set_text(s_status_battery_label, "100%");
    if (s_settings_wlan_status_label) {
        lv_label_set_text(s_settings_wlan_status_label, wlan_on ? "On" : "Off");
    }
    if (s_wlan_switch) {
        if (wlan_on) lv_obj_add_state(s_wlan_switch, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_wlan_switch, LV_STATE_CHECKED);
    }

    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int internal_used = internal_total ? (int)((internal_total - internal_free) * 100 / internal_total) : 0;
    int psram_used = psram_total ? (int)((psram_total - psram_free) * 100 / psram_total) : 0;
    lv_bar_set_value(s_status_sram_bar, internal_used, LV_ANIM_OFF);
    lv_bar_set_value(s_status_psram_bar, psram_used, LV_ANIM_OFF);

    if (s_wlan_connected_main_label) {
        if (connected) {
            lv_label_set_text_fmt(s_wlan_connected_main_label, "%s", (const char *)ap.ssid);
            if (s_wlan_connected_minor_label) {
                lv_label_set_text(s_wlan_connected_minor_label, "Connected");
            }
            if (s_wlan_connected_signal_icon) {
                lv_img_set_src(s_wlan_connected_signal_icon, _wlan_signal_icon(ap.rssi));
            }
            if (s_wlan_connected_lock_icon) {
                if (ap.authmode == WIFI_AUTH_OPEN) {
                    lv_obj_add_flag(s_wlan_connected_lock_icon, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(s_wlan_connected_lock_icon, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_wlan_connected_group) {
                lv_obj_clear_flag(s_wlan_connected_group, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_label_set_text(s_wlan_connected_main_label, "Not connected");
            if (s_wlan_connected_minor_label) {
                lv_label_set_text(s_wlan_connected_minor_label, "");
            }
            if (s_wlan_connected_group) {
                lv_obj_add_flag(s_wlan_connected_group, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void _show_settings(void)
{
    if (!s_settings_scr) return;
    _show_status_panel(false);
    s_parent_screen = s_main_scr;
    s_return_target = s_main_scr;
    s_lyrics_visible = false;
    lv_scr_load(s_settings_scr);
    _show_return_bar(false);
    _update_system_status();
}

static void _show_wlan(void)
{
    if (!s_wlan_scr) return;
    _show_status_panel(false);
    s_parent_screen = s_settings_scr;
    s_return_target = s_settings_scr;
    s_lyrics_visible = false;
    lv_scr_load(s_wlan_scr);
    _show_return_bar(false);
    _update_system_status();
    _request_wlan_scan();
}

static void _show_softap(void)
{
    if (!s_softap_scr) return;
    if (!wifi_provision_is_running()) {
        esp_err_t ret = wifi_provision_start();
        if (ret != ESP_OK) ESP_LOGE(TAG, "Wi-Fi provisioning start failed: %s", esp_err_to_name(ret));
    }
    const char *ap_ssid = wifi_provision_get_ap_ssid();
    if (s_softap_qrcode && ap_ssid && ap_ssid[0] != '\0') {
        char qr_string[128];
        snprintf(qr_string, sizeof(qr_string), "WIFI:T:nopass;S:%s;P:;;", ap_ssid);
        lv_qrcode_update(s_softap_qrcode, qr_string, strlen(qr_string));
    }
    _show_status_panel(false);
    s_parent_screen = s_wlan_scr;
    s_return_target = s_wlan_scr;
    s_lyrics_visible = false;
    lv_scr_load(s_softap_scr);
    _show_return_bar(false);
    _update_system_status();
}

static void _show_main_screen(void)
{
    if (!s_main_scr) return;
    _show_status_panel(false);
    s_parent_screen = NULL;
    s_return_target = NULL;
    s_lyrics_visible = false;
    lv_scr_load(s_main_scr);
    _show_return_bar(false);
}

static void _show_parent_screen(void)
{
    lv_obj_t *current = lv_scr_act();
    if (current == s_softap_scr) _show_wlan();
    else if (current == s_wlan_scr) _show_settings();
    else if (current == s_settings_scr || current == s_lyrics_scr) _show_main_screen();
    else if (s_parent_screen) lv_scr_load(s_parent_screen);
}

static void _system_button_event_cb(lv_event_t *event)
{
    if (!event) return;
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_current_target(event);
    if (code == LV_EVENT_VALUE_CHANGED && target == s_wlan_switch) {
        ESP_LOGI(TAG, "WLAN switch changed: %d",
                 lv_obj_has_state(target, LV_STATE_CHECKED));
        _update_system_status();
        return;
    }
    if (code != LV_EVENT_CLICKED) return;
    if (target == s_status_wifi_btn || target == s_settings_wlan_cell) {
        _show_wlan();
    } else if (target == s_wlan_softap_cell) {
        _show_softap();
    } else if (target == s_wlan_back_btn || target == s_softap_back_btn ||
               target == s_softap_content_back_btn) {
        _show_parent_screen();
    } else if (target == s_status_volume_btn) {
        ESP_LOGI(TAG, "QuickSettings volume clicked");
    } else if (target == s_status_brightness_btn) {
        ESP_LOGI(TAG, "QuickSettings brightness clicked");
    } else {
        ESP_LOGI(TAG, "Settings cell clicked");
    }
}

static void _gesture_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    bool pressed = false;
    int x = 0;
    int y = 0;
    if (!lvgl_port_touch_get_snapshot(&pressed, &x, &y)) return;

    if (pressed && !s_gesture.active) {
        s_gesture.active = true;
        s_gesture.start_x = x;
        s_gesture.start_y = y;
        s_gesture.last_x = x;
        s_gesture.last_y = y;
        s_gesture.start_tick = lv_tick_get();
        s_gesture.mode = GESTURE_MODE_NONE;
        if (!s_status_visible && y < GESTURE_EDGE_PX) {
            s_gesture.mode = GESTURE_MODE_STATUS_OPEN;
            lv_obj_clear_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_status_panel);
            lv_obj_set_y(s_status_panel, y - (STATUS_PANEL_H - 20));
        } else if (s_status_visible && y >= TFT_H - GESTURE_EDGE_PX) {
            s_gesture.mode = GESTURE_MODE_STATUS_CLOSE;
        } else if (!s_status_visible && lv_scr_act() != s_main_scr &&
                   y >= TFT_H - GESTURE_EDGE_PX) {
            s_gesture.mode = GESTURE_MODE_RETURN;
            _show_return_bar(true);
        }
        ESP_LOGI(TAG, "gesture start (%d,%d) mode=%d", x, y, s_gesture.mode);
        return;
    }

    if (pressed) {
        s_gesture.last_x = x;
        s_gesture.last_y = y;
        if (s_gesture.mode == GESTURE_MODE_STATUS_OPEN ||
            s_gesture.mode == GESTURE_MODE_STATUS_CLOSE) {
            int panel_y = s_gesture.mode == GESTURE_MODE_STATUS_OPEN
                              ? y - (STATUS_PANEL_H - 20)
                              : y - s_gesture.start_y;
            if (panel_y > 0) panel_y = 0;
            if (panel_y < STATUS_PANEL_HIDDEN_Y) panel_y = STATUS_PANEL_HIDDEN_Y;
            lv_obj_set_y(s_status_panel, panel_y);
        } else if (s_gesture.mode == GESTURE_MODE_RETURN) {
            int offset = s_gesture.start_y - y;
            if (offset < 0) offset = 0;
            if (offset > GESTURE_DISTANCE_PX) offset = GESTURE_DISTANCE_PX;
            lv_coord_t width = RETURN_BAR_W - RETURN_BAR_W * offset / GESTURE_DISTANCE_PX;
            lv_obj_set_width(s_return_bar, width);
            lv_obj_set_x(s_return_bar, (TFT_W - width) / 2);
        }
        return;
    }

    if (!s_gesture.active) return;
    s_gesture.active = false;
    int dx = s_gesture.last_x - s_gesture.start_x;
    int dy = s_gesture.last_y - s_gesture.start_y;
    int abs_dx = abs(dx);
    int abs_dy = abs(dy);
    uint32_t duration_ms = lv_tick_elaps(s_gesture.start_tick);
    bool vertical = abs_dy >= GESTURE_DISTANCE_PX &&
                    (abs_dx == 0 || ((float)abs_dy / (float)abs_dx) >= 1.732f);

    if (s_gesture.mode == GESTURE_MODE_STATUS_OPEN) {
        ESP_LOGI(TAG, "gesture down dx=%d dy=%d duration=%lu stop_y=%d", dx, dy,
                 (unsigned long)duration_ms, s_gesture.last_y);
        _show_status_panel(vertical && dy >= GESTURE_DISTANCE_PX &&
                           s_gesture.last_y >= TFT_H * 20 / 100);
    } else if (s_gesture.mode == GESTURE_MODE_STATUS_CLOSE) {
        ESP_LOGI(TAG, "gesture status close dx=%d dy=%d duration=%lu stop_y=%d", dx, dy,
                 (unsigned long)duration_ms, s_gesture.last_y);
        _show_status_panel(!(vertical && dy <= -GESTURE_DISTANCE_PX &&
                             s_gesture.last_y <= TFT_H * 80 / 100));
    } else if (s_gesture.mode == GESTURE_MODE_RETURN) {
        ESP_LOGI(TAG, "gesture up dx=%d dy=%d duration=%lu short=%d", dx, dy,
                 (unsigned long)duration_ms, duration_ms < GESTURE_SHORT_MS);
        _show_return_bar(false);
        if (vertical && dy <= -GESTURE_DISTANCE_PX) _show_parent_screen();
    } else if (!vertical || duration_ms == 0) {
        ESP_LOGI(TAG, "gesture ignored dx=%d dy=%d duration=%lu", dx, dy,
                 (unsigned long)duration_ms);
    } else {
        ESP_LOGI(TAG, "gesture edge ignored start=(%d,%d) dx=%d dy=%d", s_gesture.start_x,
                 s_gesture.start_y, dx, dy);
    }
    s_gesture.mode = GESTURE_MODE_NONE;
}

static void _wifi_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    _apply_wlan_scan_results();
    _update_system_status();
}

void lvgl_port_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    s_wlan_ssid_font = esp_brookesia_font_maison_neue_book_22;
    s_wlan_ssid_font.fallback = &lv_font_simsun_16_cjk;

    /* 关闭屏幕滚动条 */
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ====== 动态背景渐变（预抖动图片，消除 RGB565 色阶） ====== */
    s_bg_img = lv_img_create(scr);
    lv_obj_set_pos(s_bg_img, 0, 0);
    lv_obj_move_background(s_bg_img);
    _generate_dithered_bg(28, 82, 120, 6, 16, 24);   /* 默认深蓝渐变：上亮下暗 */

    /* ====== 唱片转盘（1:1 高清矢量黑胶，256x256，中心 180,128） ====== */
    s_disc_img = lv_img_create(scr);
    lv_img_set_src(s_disc_img, &ui_img_jiaopian_png);
    lv_obj_set_pos(s_disc_img, UI_CENTER_X - 128, 128 - 128); /* (52, 0) */
    lv_img_set_zoom(s_disc_img, LV_SCALE_NONE);
    lv_img_set_pivot(s_disc_img, 128, 128);

    /* ====== 封面（容器圆形裁剪 + 图片在容器内旋转） ====== */
    s_cover_container = lv_obj_create(scr);
    lv_obj_set_size(s_cover_container, UI_COVER_SIZE, UI_COVER_SIZE);
    lv_obj_set_pos(s_cover_container, UI_CENTER_X - UI_COVER_SIZE / 2, 54);
    lv_obj_set_style_radius(s_cover_container, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_cover_container, true, 0);
    lv_obj_set_style_border_width(s_cover_container, 0, 0);
    lv_obj_set_style_bg_opa(s_cover_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_cover_container, 0, 0);
    lv_obj_clear_flag(s_cover_container, LV_OBJ_FLAG_SCROLLABLE);

    s_cover_img = lv_img_create(s_cover_container);
    lv_img_set_src(s_cover_img, &ui_img_haibao_png);
    lv_img_set_zoom(s_cover_img, UI_COVER_ART_ZOOM);
    lv_img_set_pivot(s_cover_img, 46, 46);  /* 中心 pivot */
    lv_obj_set_pos(s_cover_img, 27, 27);

    /* ====== 唱片唱针（1:1 高清细腻金属唱针，48x100，无需运行期缩放） ====== */
    s_img_citou = lv_img_create(scr);
    lv_img_set_src(s_img_citou, &ui_img_citou_png);
    lv_img_set_zoom(s_img_citou, LV_SCALE_NONE);
    lv_img_set_pivot(s_img_citou, 42, 11);  /* 旋转轴心：唱针固定端基座中心 (42, 11) */
    lv_obj_set_pos(s_img_citou, 266, 44);  /* pivot 落在 (308, 55) */
    lv_img_set_angle(s_img_citou, -200);    /* 初始抬起（停止位） */

    /* ====== 标题 ====== */
    s_label_title = lv_label_create(scr);
    lv_obj_set_pos(s_label_title, UI_SAFE_X, 220);
    lv_obj_set_width(s_label_title, UI_SAFE_W);
    lv_label_set_text(s_label_title, "DLNA Player");
    lv_obj_set_style_text_color(s_label_title, C_WHITE, 0);
    lv_obj_set_style_text_font(s_label_title, &lv_font_simsun_16_cjk, 0);
    lv_label_set_long_mode(s_label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_label_title, LV_TEXT_ALIGN_CENTER, 0);

    /* ====== 歌手 ====== */
    s_label_artist = lv_label_create(scr);
    lv_obj_set_pos(s_label_artist, UI_SAFE_X, 242);
    lv_obj_set_width(s_label_artist, UI_SAFE_W);
    lv_label_set_text(s_label_artist, "Waiting...");
    lv_obj_set_style_text_color(s_label_artist, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_label_artist, &lv_font_simsun_16_cjk, 0);
    lv_label_set_long_mode(s_label_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_label_artist, LV_TEXT_ALIGN_CENTER, 0);

    /* ====== 进度条 + 时间 ====== */
    s_bar_progress = lv_bar_create(scr);
    lv_obj_set_pos(s_bar_progress, 60, 271);
    lv_obj_set_size(s_bar_progress, 240, 4);
    lv_bar_set_range(s_bar_progress, 0, 1000);
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_progress, C_BTN_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_progress, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    s_label_time1 = lv_label_create(scr);
    lv_obj_set_pos(s_label_time1, 72, 277);
    lv_obj_set_size(s_label_time1, 80, 20);
    lv_label_set_text(s_label_time1, "0:00");
    lv_obj_set_style_text_color(s_label_time1, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_label_time1, &lv_font_montserrat_12, 0);

    s_label_time2 = lv_label_create(scr);
    lv_obj_set_pos(s_label_time2, 208, 277);
    lv_obj_set_size(s_label_time2, 80, 20);
    lv_label_set_text(s_label_time2, "0:00");
    lv_obj_set_style_text_color(s_label_time2, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_label_time2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_label_time2, LV_TEXT_ALIGN_RIGHT, 0);

    /* ====== 控制按钮 ====== */
    s_btn_prev = lv_imgbtn_create(scr);
    lv_imgbtn_set_src(s_btn_prev, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_shangyi1_png, NULL);
    lv_obj_set_pos(s_btn_prev,
                   UI_CENTER_X - UI_BTN_PLAY_SIZE / 2 - UI_BUTTON_GAP - UI_BTN_SIDE_SIZE,
                   UI_BUTTON_CENTER_Y - UI_BTN_SIDE_SIZE / 2);
    lv_obj_set_size(s_btn_prev, UI_BTN_SIDE_SIZE, UI_BTN_SIDE_SIZE);
    lv_obj_set_ext_click_area(s_btn_prev, UI_BUTTON_CLICK_PAD_SIDE);

    s_btn_play = lv_imgbtn_create(scr);
    lv_imgbtn_set_src(s_btn_play, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_bofang1_png, NULL);
    lv_obj_set_pos(s_btn_play,
                   UI_CENTER_X - UI_BTN_PLAY_SIZE / 2,
                   UI_BUTTON_CENTER_Y - UI_BTN_PLAY_SIZE / 2);
    lv_obj_set_size(s_btn_play, UI_BTN_PLAY_SIZE, UI_BTN_PLAY_SIZE);
    lv_obj_set_ext_click_area(s_btn_play, UI_BUTTON_CLICK_PAD);

    s_btn_next = lv_imgbtn_create(scr);
    lv_imgbtn_set_src(s_btn_next, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_xiyi1_png, NULL);
    lv_obj_set_pos(s_btn_next,
                   UI_CENTER_X + UI_BTN_PLAY_SIZE / 2 + UI_BUTTON_GAP,
                   UI_BUTTON_CENTER_Y - UI_BTN_SIDE_SIZE / 2);
    lv_obj_set_size(s_btn_next, UI_BTN_SIDE_SIZE, UI_BTN_SIDE_SIZE);
    lv_obj_set_ext_click_area(s_btn_next, UI_BUTTON_CLICK_PAD_SIDE);

    /* 按压反馈：按下时变暗，松开恢复（替代 scale，性能更优） */
    static lv_style_transition_dsc_t press_tr;
    static lv_style_prop_t press_props[] = { LV_STYLE_OPA, (lv_style_prop_t)0 };
    lv_style_transition_dsc_init(&press_tr, press_props, lv_anim_path_ease_out, 120, 0, NULL);
    lv_obj_set_style_transition(s_btn_prev, &press_tr, LV_STATE_PRESSED);
    lv_obj_set_style_transition(s_btn_prev, &press_tr, LV_STATE_DEFAULT);
    lv_obj_set_style_transition(s_btn_play, &press_tr, LV_STATE_PRESSED);
    lv_obj_set_style_transition(s_btn_play, &press_tr, LV_STATE_DEFAULT);
    lv_obj_set_style_transition(s_btn_next, &press_tr, LV_STATE_PRESSED);
    lv_obj_set_style_transition(s_btn_next, &press_tr, LV_STATE_DEFAULT);
    lv_obj_set_style_opa(s_btn_prev, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(s_btn_play, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(s_btn_next, LV_OPA_70, LV_STATE_PRESSED);

    /* 点击回调 */
    lv_obj_add_event_cb(s_btn_prev, _btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn_play, _btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_btn_next, _btn_click_cb, LV_EVENT_CLICKED, NULL);

    /* 点击封面图 → 切到歌词界面 */
    lv_obj_add_event_cb(s_cover_container, _cover_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_opa(s_cover_container, LV_OPA_70, LV_STATE_PRESSED);
    static lv_style_transition_dsc_t cover_press_tr;
    static lv_style_prop_t cover_press_props[] = { LV_STYLE_OPA, (lv_style_prop_t)0 };
    lv_style_transition_dsc_init(&cover_press_tr, cover_press_props, lv_anim_path_ease_out, 120, 0, NULL);
    lv_obj_set_style_transition(s_cover_container, &cover_press_tr, LV_STATE_DEFAULT);

    lvgl_port_ui_lyrics_create();
    s_main_scr = scr;  /* 保存主屏幕引用 */
    _create_settings_screen();
    _create_wlan_screen();
    _create_softap_screen();
    _create_system_ui();
    s_gesture_timer = lv_timer_create(_gesture_timer_cb, 20, NULL);
    s_wifi_status_timer = lv_timer_create(_wifi_status_timer_cb, 1000, NULL);
    _update_system_status();
    ESP_LOGI(TAG, "UI created");
}

/* ====== 歌词界面（3行：上一句/当前句/下一句） ====== */
void lvgl_port_ui_lyrics_create(void)
{
    s_lyrics_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(s_lyrics_scr, LV_OPA_0, 0);   /* 背景透明，由背景图显示 */
    lv_obj_clear_flag(s_lyrics_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 点击歌词界面 → 返回封面 */
    lv_obj_add_event_cb(s_lyrics_scr, _cover_click_cb, LV_EVENT_CLICKED, NULL);

    /* 背景图（与主界面共享 s_bg_dsc 预抖动渐变） */
    s_lyrics_bg_img = lv_img_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_bg_img, 0, 0);
    lv_obj_move_background(s_lyrics_bg_img);
    if (s_bg_dsc) lv_img_set_src(s_lyrics_bg_img, s_bg_dsc);

    /* 暂无歌词占位（居中） */
    s_lyrics_placeholder = lv_label_create(s_lyrics_scr);
    lv_obj_set_width(s_lyrics_placeholder, UI_SAFE_W);
    lv_obj_set_pos(s_lyrics_placeholder, UI_SAFE_X, 166);
    lv_label_set_text(s_lyrics_placeholder, "暂无歌词");
    lv_obj_set_style_text_font(s_lyrics_placeholder, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_placeholder, C_DIM, 0);
    lv_obj_set_style_text_align(s_lyrics_placeholder, LV_TEXT_ALIGN_CENTER, 0);

    /* 上一句（圆屏上半部，暗淡） */
    s_lyrics_prev = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_prev, UI_SAFE_X, 128);
    lv_obj_set_width(s_lyrics_prev, UI_SAFE_W);
    lv_label_set_text(s_lyrics_prev, "");
    lv_obj_set_style_text_font(s_lyrics_prev, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_prev, C_DIM, 0);
    lv_obj_set_style_text_align(s_lyrics_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyrics_prev, LV_LABEL_LONG_CLIP);

    /* 当前句（灰色+选中高亮，左对齐，缓慢左移） */
    s_lyrics_curr = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_curr, UI_SAFE_X, 168);
    lv_obj_set_width(s_lyrics_curr, UI_SAFE_W);
    lv_label_set_text(s_lyrics_curr, "");
    lv_obj_set_style_text_font(s_lyrics_curr, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_curr, C_DIM, 0);
    lv_obj_set_style_text_color(s_lyrics_curr, C_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_lyrics_curr, C_BG_TOP, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_lyrics_curr, LV_OPA_0, LV_PART_SELECTED);   /* 透明背景，让背景图透出 */
    lv_obj_set_style_text_align(s_lyrics_curr, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_lyrics_curr, LV_LABEL_LONG_CLIP);

    /* 下一句（圆屏下半部，暗淡） */
    s_lyrics_next = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_next, UI_SAFE_X, 208);
    lv_obj_set_width(s_lyrics_next, UI_SAFE_W);
    lv_label_set_text(s_lyrics_next, "");
    lv_obj_set_style_text_font(s_lyrics_next, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(s_lyrics_next, C_DIM, 0);
    lv_obj_set_style_text_align(s_lyrics_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyrics_next, LV_LABEL_LONG_CLIP);

    ESP_LOGI(TAG, "Lyrics screen created (3-line)");
}

void lvgl_port_ui_toggle_lyrics(void)
{
    s_lyrics_visible = !s_lyrics_visible;
    if (s_lyrics_visible) {
        _show_status_panel(false);
        _show_return_bar(false);
        lv_scr_load(s_lyrics_scr);
    } else {
        if (s_main_scr) lv_scr_load(s_main_scr);
    }
}

bool lvgl_port_ui_lyrics_is_visible(void)
{
    return s_lyrics_visible;
}

/* ====== UI 更新 ====== */
void lvgl_port_ui_set_title(const char *title) {
    lv_label_set_text(s_label_title, title ? title : "DLNA Player");
}
void lvgl_port_ui_set_artist(const char *artist) {
    lv_label_set_text(s_label_artist, artist && artist[0] ? artist : "Unknown");
}
void lvgl_port_ui_set_progress(int position_sec, int duration_sec) {
    int pmin = position_sec / 60, psec = position_sec % 60;
    if (duration_sec > 0) {
        int dmin = duration_sec / 60, dsec = duration_sec % 60;
        int permille = position_sec * 1000 / duration_sec;
        if (permille > 1000) permille = 1000;
        lv_bar_set_value(s_bar_progress, permille, LV_ANIM_ON);
        lv_label_set_text_fmt(s_label_time1, "%d:%02d", pmin, psec);
        lv_label_set_text_fmt(s_label_time2, "%d:%02d", dmin, dsec);
    } else {
        lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_label_time1, "%d:%02d", pmin, psec);
        lv_label_set_text(s_label_time2, "0:00");
    }
}
/* ── 唱片封面旋转动画回调（仅旋转裁剪容器内的封面，黑胶底盘固定真实反射，极大提升帧率至满帧丝滑） ── */
static void _cover_anim_cb(void *obj, int32_t v)
{
    (void)obj;
    if (s_cover_img) lv_img_set_angle(s_cover_img, v);
}

/* ── 唱针平滑移动动画 ── */
static void _citou_anim_cb(void *obj, int32_t v)
{
    lv_img_set_angle((lv_obj_t *)obj, v);
}

static void _animate_citou(int target_angle)
{
    if (!s_img_citou) return;
    lv_anim_del(s_img_citou, _citou_anim_cb);  /* 取消正在进行的动画 */
    int32_t cur = lv_img_get_angle(s_img_citou);
    /* 归一化目标到 0-3600 */
    while (target_angle < 0) target_angle += 3600;
    while (target_angle >= 3600) target_angle -= 3600;
    /* 计算最短路径方向（顺时针/逆时针） */
    int32_t diff = target_angle - cur;
    if (diff > 1800) diff -= 3600;
    if (diff < -1800) diff += 3600;
    int32_t end = cur + diff;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_img_citou);
    lv_anim_set_exec_cb(&a, _citou_anim_cb);
    lv_anim_set_values(&a, cur, end);
    lv_anim_set_time(&a, 100);                 /* 100ms 干脆利落 */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);    /* 快起慢停，模拟物理惯性 */
    lv_anim_start(&a);
}

static int s_last_ui_state = -1;  /* 上次 set_state 的值，用于检测变化 */

void lvgl_port_ui_set_state(int state) {
    if (state == s_last_ui_state) return;  /* 状态没变，跳过 */
    s_last_ui_state = state;

    if (state == 1) {
        lv_imgbtn_set_src(s_btn_play, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_zanting1_png, NULL);
        /* 播放：封面就绪后才放下唱针 + 旋转 */
        if (s_cover_ready) {
            _animate_citou(70);    /* 唱针平滑放下（角度减小，不要抬太高） */
            if (s_cover_img && !lv_anim_get(s_cover_img, _cover_anim_cb)) {
                int32_t cur = lv_img_get_angle(s_cover_img);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_cover_img);
                lv_anim_set_exec_cb(&a, _cover_anim_cb);
                lv_anim_set_values(&a, cur, cur + 3600);
                lv_anim_set_time(&a, 8000);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_start(&a);
            }
        }
    } else {
        lv_imgbtn_set_src(s_btn_play, LV_IMGBTN_STATE_RELEASED, NULL, &ui_img_bofang1_png, NULL);
        /* 暂停/停止：停止动画（封面保持当前角度）+ 唱针平滑抬起 */
        lv_anim_del(s_cover_img, _cover_anim_cb);
        _animate_citou(-200);
    }
}
void lvgl_port_ui_set_volume(int vol) { (void)vol; }

/* Interleaved gradient noise → [0, mod). Better than Bayer for slow ramps. */
static unsigned _ign(int x, int y, unsigned mod)
{
    unsigned s = ((unsigned)x * 4398u + (unsigned)y * 383u) & 0xFFFFu;
    unsigned long long p = (unsigned long long)s * 3472295ull;
    return (unsigned)(((p & 0xFFFFFFFFull) * (unsigned long long)mod) >> 32);
}

/* ====== 生成预抖动渐变背景（8.8 插值 + IGN，消除 RGB565 色带） ====== */
static void _generate_dithered_bg(uint8_t r_top, uint8_t g_top, uint8_t b_top,
                                   uint8_t r_bot, uint8_t g_bot, uint8_t b_bot)
{
    const int W = TFT_W, H = TFT_H;

    /* 首次调用分配 PSRAM 缓冲 + LVGL 图片描述符 */
    if (!s_bg_dsc) {
        s_bg_dsc = (lv_img_dsc_t *)lv_malloc(sizeof(lv_img_dsc_t));
        if (!s_bg_dsc) return;
        memset(s_bg_dsc, 0, sizeof(lv_img_dsc_t));
        s_bg_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        s_bg_dsc->header.flags = 0;
        s_bg_dsc->header.w = W;
        s_bg_dsc->header.h = H;
        s_bg_dsc->header.stride = W * sizeof(uint16_t);
        s_bg_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
        s_bg_dsc->data_size = W * H * sizeof(uint16_t);
        s_bg_dsc->data = (const uint8_t *)heap_caps_malloc(s_bg_dsc->data_size, MALLOC_CAP_SPIRAM);
        if (!s_bg_dsc->data) {
            ESP_LOGW(TAG, "bg dither PSRAM alloc failed");
            lv_free(s_bg_dsc);
            s_bg_dsc = NULL;
            return;
        }
    }

    uint16_t *buf = (uint16_t *)s_bg_dsc->data;

    const int y_div = H > 1 ? (H - 1) : 1;
    const int r_delta = (int)r_bot - (int)r_top;
    const int g_delta = (int)g_bot - (int)g_top;
    const int b_delta = (int)b_bot - (int)b_top;

    for (int y = 0; y < H; y++) {
        int r_fp = ((int)r_top << 8) + r_delta * y * 256 / y_div;
        int g_fp = ((int)g_top << 8) + g_delta * y * 256 / y_div;
        int b_fp = ((int)b_top << 8) + b_delta * y * 256 / y_div;

        for (int x = 0; x < W; x++) {
            /* ±1 RGB565 LSB（2 LSB 峰峰值），三通道空间错开，避免亮灰绿/品红横带 */
            int r5 = (r_fp + (int)_ign(x, y, 4096u) - 2048) >> 11;
            int g6 = (g_fp + (int)_ign(x + 19, y + 7, 2048u) - 1024) >> 10;
            int b5 = (b_fp + (int)_ign(x + 47, y + 23, 4096u) - 2048) >> 11;

            if (r5 < 0) r5 = 0; else if (r5 > 31) r5 = 31;
            if (g6 < 0) g6 = 0; else if (g6 > 63) g6 = 63;
            if (b5 < 0) b5 = 0; else if (b5 > 31) b5 = 31;

            /* LVGL 9 native RGB565; display byte swap is handled in flush_cb. */
            buf[y * W + x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }

    lv_img_set_src(s_bg_img, s_bg_dsc);
    lv_obj_invalidate(s_bg_img);
    if (s_lyrics_bg_img) {
        lv_img_set_src(s_lyrics_bg_img, s_bg_dsc);
        lv_obj_invalidate(s_lyrics_bg_img);
    }
}

/* ====== 从封面 RGB565 像素提取主色并更新背景 ====== */
static void _update_bg_from_cover(const uint16_t *pixels, int w, int h)
{
    if (!pixels || w <= 0 || h <= 0) return;

    /* Standard LVGL RGB565: R=bits 15..11, G=bits 10..5, B=bits 4..0. */
    unsigned long r_sum = 0, g_sum = 0, b_sum = 0;
    int count = w * h;

    for (int i = 0; i < count; i++) {
        uint16_t p = pixels[i];
        int r5 = (p >> 11) & 0x1F;
        int g6 = (p >> 5) & 0x3F;
        int b5 = p & 0x1F;
        r_sum += (r5 << 3) | (r5 >> 2);   /* 5→8 位扩展 */
        g_sum += (g6 << 2) | (g6 >> 4);   /* 6→8 位扩展 */
        b_sum += (b5 << 3) | (b5 >> 2);   /* 5→8 位扩展 */
    }

    if (count == 0) return;

    int avg_r = (int)(r_sum / count);
    int avg_g = (int)(g_sum / count);
    int avg_b = (int)(b_sum / count);

    /* 顶部约 55% 封面亮度，底部约 12%，保留色相，形成能看见的纵向渐变 */
    int r_top = avg_r * 140 / 256;
    int g_top = avg_g * 140 / 256;
    int b_top = avg_b * 140 / 256;
    int r_bot = avg_r * 30 / 256;
    int g_bot = avg_g * 30 / 256;
    int b_bot = avg_b * 30 / 256;

    if (r_top + g_top + b_top < 60) {
        r_top += 18;
        g_top += 22;
        b_top += 32;
    }
    if (r_top > 255) r_top = 255;
    if (g_top > 255) g_top = 255;
    if (b_top > 255) b_top = 255;
    if (r_bot > r_top) r_bot = r_top;
    if (g_bot > g_top) g_bot = g_top;
    if (b_bot > b_top) b_bot = b_top;

    _generate_dithered_bg((uint8_t)r_top, (uint8_t)g_top, (uint8_t)b_top,
                          (uint8_t)r_bot, (uint8_t)g_bot, (uint8_t)b_bot);
}

static void _opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void lvgl_port_ui_lyrics_update(int current_idx, const char *prev, const char *curr, const char *next)
{
    /* 检测行号切换，触发向上滚动动画 */
    if (current_idx != s_lyrics_prev_line && s_lyrics_prev_line >= 0) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_lyrics_prev);
        lv_anim_set_exec_cb(&a, _opa_anim_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&a, 150);               /* 退出更快 */
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_lyrics_curr);
        lv_anim_set_exec_cb(&a, _opa_anim_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 250);               /* 进入稍慢 */
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    s_lyrics_current = current_idx;
    s_lyrics_prev_line = current_idx;

    lv_label_set_text(s_lyrics_prev, prev ? prev : "");
    lv_label_set_text(s_lyrics_curr, curr ? curr : "");
    lv_label_set_text(s_lyrics_next, next ? next : "");

    /* 上下行：超宽则左对齐，否则居中 */
    int16_t w_prev = lv_txt_get_width(prev ? prev : "", prev ? strlen(prev) : 0,
        lv_obj_get_style_text_font(s_lyrics_prev, 0), 0);
    lv_obj_set_style_text_align(s_lyrics_prev, w_prev > UI_SAFE_W ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);

    int16_t w_next = lv_txt_get_width(next ? next : "", next ? strlen(next) : 0,
        lv_obj_get_style_text_font(s_lyrics_next, 0), 0);
    lv_obj_set_style_text_align(s_lyrics_next, w_next > UI_SAFE_W ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);

    /* 当前行动态对齐：居中（放得下）或左对齐（超宽） */
    lv_font_t *font_curr = lv_obj_get_style_text_font(s_lyrics_curr, 0);
    int16_t w_curr = lv_txt_get_width(curr ? curr : "", curr ? strlen(curr) : 0,
        font_curr, 0);
    bool curr_overflow = w_curr > UI_SAFE_W;
    lv_obj_set_style_text_align(s_lyrics_curr,
        curr_overflow ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);

    /* 重置高亮及位置 */
    lv_label_set_text_selection_start(s_lyrics_curr, 0);
    lv_label_set_text_selection_end(s_lyrics_curr, 0);
    ((lv_label_t *)s_lyrics_curr)->offset.x = 0;

    /* 有歌词→隐藏占位 */
    if (curr && curr[0]) {
        if (s_lyrics_placeholder) lv_obj_add_flag(s_lyrics_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── 逐字高亮（karaoke）+ 逐字左移 ── */
static int s_scroll_total = 0;   /* 总滚动距离（正数） */
static int s_text_bytes = 0;     /* 总字节数 */

void lvgl_port_ui_lyrics_karaoke(int byte_idx)
{
    const char *text = lv_label_get_text(s_lyrics_curr);
    if (!text || !text[0]) return;

    if (byte_idx <= 0) {
        lv_label_set_text_selection_start(s_lyrics_curr, 0);
        lv_label_set_text_selection_end(s_lyrics_curr, 0);
        ((lv_label_t *)s_lyrics_curr)->offset.x = 0;
        return;
    }

    lv_label_set_text_selection_start(s_lyrics_curr, 0);
    lv_label_set_text_selection_end(s_lyrics_curr, byte_idx);

    /* 仅当文本超长时才左移，否则只高亮不移动 */
    if (s_scroll_total > 0) {
        /* 当前字保持在屏幕 1/4 位置，但不超出右边界 */
        int16_t w = lv_txt_get_width(text, byte_idx,
            lv_obj_get_style_text_font(s_lyrics_curr, 0), 0);
        int label_w = lv_obj_get_width(s_lyrics_curr);
        int anchor = label_w / 4;
        int offset = 0;
        if (w > anchor) {
            offset = anchor - w;
        }
        /* 限制左移不超出右边界（最后一个字能在右边显示即可） */
        if (offset < -s_scroll_total) offset = -s_scroll_total;
        ((lv_label_t *)s_lyrics_curr)->offset.x = offset;
    }
}

void lvgl_port_ui_lyrics_set_scroll_dist(int dist)
{
    s_scroll_total = dist;
}

/* ── 当前句滚动状态 ── */
static int s_scroll_dist = 0;  /* 需滚动的像素（正数），0=不滚动 */

void lvgl_port_ui_lyrics_scroll_to_end(int line_duration_ms)
{
    (void)line_duration_ms;
    const char *text = lv_label_get_text(s_lyrics_curr);
    if (!text || !text[0]) { s_scroll_total = 0; s_text_bytes = 0; return; }

    /* 用 lv_txt_get_width 取实际像素宽度（不受 max_width 约束换行） */
    lv_font_t *font = lv_obj_get_style_text_font(s_lyrics_curr, 0);
    int16_t text_w = lv_txt_get_width(text, strlen(text), font, 0);
    int label_w = lv_obj_get_width(s_lyrics_curr);
    if (text_w <= label_w) { s_scroll_total = 0; s_text_bytes = 0; return; }

    s_scroll_total = (int)text_w - label_w;
    s_text_bytes = strlen(text);
    ((lv_label_t *)s_lyrics_curr)->offset.x = 0;
    ESP_LOGI(TAG, "scroll_dist: total=%d bytes=%d (text_w=%d label_w=%d)", s_scroll_total, s_text_bytes, text_w, label_w);
}

void lvgl_port_ui_lyrics_set_scroll_progress(int pct)
{
    if (s_scroll_dist <= 0) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int x = -s_scroll_dist * pct / 100;
    ((lv_label_t *)s_lyrics_curr)->offset.x = x;
    lv_obj_invalidate(s_lyrics_curr);
    if (pct % 25 == 0) ESP_LOGI(TAG, "scroll_x=%d (dist=%d pct=%d)", x, s_scroll_dist, pct);
}

void lvgl_port_ui_lyrics_tick_scroll(void)
{
    /* LV_LABEL_LONG_SCROLL 内部处理 */
}

void lvgl_port_ui_lyrics_clear(void)
{
    s_lyrics_current = -1;
    s_lyrics_prev_line = -1;
    lv_label_set_text(s_lyrics_prev, "");
    lv_label_set_text(s_lyrics_curr, "");
    lv_label_set_text(s_lyrics_next, "");
    lv_label_set_text_selection_start(s_lyrics_curr, 0);
    lv_label_set_text_selection_end(s_lyrics_curr, 0);
    s_scroll_total = 0;
    s_text_bytes = 0;
    if (s_lyrics_placeholder) lv_obj_clear_flag(s_lyrics_placeholder, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Lyrics UI cleared");
}
void lvgl_port_ui_set_cover(const uint16_t *pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return;
    size_t px_size = w * h * sizeof(uint16_t);
    if (px_size > sizeof(s_cover_buf)) {
        ESP_LOGW(TAG, "Cover too large: %dx%d", w, h);
        return;
    }

    ESP_LOGI(TAG, "set_cover: %dx%d", w, h);
    memcpy(s_cover_buf, pixels, px_size);
    _update_bg_from_cover(s_cover_buf, w, h);   /* 依据封面主色更新背景 */

    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_malloc(sizeof(lv_img_dsc_t));
    if (!dsc) return;
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.flags = 0;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * sizeof(uint16_t);
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->data_size = px_size;
    dsc->data = (const uint8_t *)s_cover_buf;
    lv_img_set_src(s_cover_img, dsc);
    lv_obj_invalidate(s_cover_img);
    lv_img_set_zoom(s_cover_img, LV_SCALE_NONE);
    lv_img_set_pivot(s_cover_img, w / 2, h / 2);
    lv_obj_set_pos(s_cover_img, (UI_COVER_SIZE - w) / 2,
                   (UI_COVER_SIZE - h) / 2);
    lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cover_container, LV_OBJ_FLAG_HIDDEN);
    s_cover_ready = true;   /* 封面就绪 */
    s_last_ui_state = -1; /* 触发下次 set_state 放下唱针 + 旋转 */
    if (s_cover_prev) lv_free(s_cover_prev);
    s_cover_prev = dsc;
}

void lvgl_port_ui_clear_cover(void) {
    if (!s_cover_img) return;
    s_cover_ready = false;
    lv_anim_del(s_cover_img, _cover_anim_cb);
    lv_img_set_src(s_cover_img, &ui_img_haibao_png);
    lv_img_set_zoom(s_cover_img, UI_COVER_ART_ZOOM);
    lv_img_set_pivot(s_cover_img, 46, 46);
    lv_obj_set_pos(s_cover_img, 27, 27);
    lv_img_set_angle(s_cover_img, 0);
    if (s_disc_img) lv_img_set_angle(s_disc_img, 0);
    if (s_cover_prev) { lv_free(s_cover_prev); s_cover_prev = NULL; }
}

/* ── 播放控制按钮回调注册 ── */
void lvgl_port_ui_register_btn_prev_cb(lvgl_btn_cb_t cb) { s_cb_btn_prev = cb; }
void lvgl_port_ui_register_btn_play_cb(lvgl_btn_cb_t cb) { s_cb_btn_play = cb; }
void lvgl_port_ui_register_btn_next_cb(lvgl_btn_cb_t cb) { s_cb_btn_next = cb; }

/* ====== 小米音箱模式：接管画面 ====== */
void lvgl_port_ui_set_speaker_mode(bool active)
{
    if (active) {
        /* 显示接管提示 */
        lv_label_set_text(s_label_title, "音频已由手机接管");
        lv_label_set_text(s_label_artist, "小米音箱模式");
        /* 隐藏封面、黑胶底盘和唱针 */
        if (s_disc_img) lv_obj_add_flag(s_disc_img, LV_OBJ_FLAG_HIDDEN);
        if (s_cover_container) lv_obj_add_flag(s_cover_container, LV_OBJ_FLAG_HIDDEN);
        if (s_img_citou) lv_img_set_angle(s_img_citou, -200);  /* 唱针抬起 */
        lv_anim_del(s_cover_img, _cover_anim_cb);  /* 停止旋转 */
        /* 进度条归零 */
        lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
        lv_label_set_text(s_label_time1, "0:00");
        lv_label_set_text(s_label_time2, "0:00");
        ESP_LOGI(TAG, "Speaker mode UI: takeover screen shown");
    } else {
        /* 恢复默认 */
        lv_label_set_text(s_label_title, "DLNA Player");
        lv_label_set_text(s_label_artist, "Waiting...");
        /* 恢复黑胶底盘与封面容器（如果有封面数据会自动更新） */
        if (s_disc_img) lv_obj_clear_flag(s_disc_img, LV_OBJ_FLAG_HIDDEN);
        if (s_cover_container) lv_obj_clear_flag(s_cover_container, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Speaker mode UI: normal restored");
    }
}

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
#define LV_FONT_MONTSERRAT_48 1
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
#include "bat_monitor.h"
#include "nvs_flash.h"
#include "nvs.h"

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
LV_FONT_DECLARE(lv_font_simsun_16_supplement);
LV_FONT_DECLARE(lv_font_simsun_16_ipa);
LV_FONT_DECLARE(lv_font_montserrat_48);

/* RAM 可写副本（原字体是 const 在 flash，不能直接写 fallback 字段）*/
static lv_font_t s_cjk_font;
static lv_font_t s_supplement_font;
static lv_font_t s_ipa_font;

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
LV_IMG_DECLARE(capsule_vol_32_32);
LV_IMG_DECLARE(capsule_bri_32_32);

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

/* 按钮动作：按下即入队，常驻任务执行（避免每次点击动态建/删任务）。
 *
 * 关键：拆成两条独立队列/worker。切歌（prev/next）在无 next_uri 时会在
 * cb_next() 内部等待手机下发 URI 最多 5 秒；若与播放/暂停共用同一个
 * worker，切歌期间暂停会被排队阻塞，手感像死机。两条队列各自串行，
 * 互不影响。 */
typedef enum {
    BTN_ACT_PREV = 1,
    BTN_ACT_PLAY,
    BTN_ACT_NEXT,
} btn_action_t;

#define BTN_QUEUE_LEN 4

static QueueHandle_t s_btn_queue_ctrl;  /* play/pause —— 快路径 */
static QueueHandle_t s_btn_queue_track; /* prev/next  —— 慢路径（含等待） */

static StaticTask_t  s_btn_ctrl_tcb;
static StaticTask_t  s_btn_track_tcb;
static StackType_t  *s_btn_ctrl_stack = NULL;
static StackType_t  *s_btn_track_stack = NULL;

/* 切歌按下序号：每次 prev/next 入队时自增。
 * dlna.c 的切歌等待循环会检查它——用户又按了就立刻放弃旧等待，
 * 让新命令马上执行，避免"切歌后 5 秒内按什么都没反应"。 */
static volatile uint32_t s_btn_track_seq = 0;
static int s_last_ui_state = -1;          /* 上次 set_state 的值（前向引用，定义在 set_state 处） */

uint32_t lvgl_port_ui_track_seq(void)
{
    return s_btn_track_seq;
}

static void btn_dispatch(btn_action_t act)
{
    lvgl_btn_cb_t cb = NULL;
    if (act == BTN_ACT_PREV)      cb = s_cb_btn_prev;
    else if (act == BTN_ACT_PLAY) cb = s_cb_btn_play;
    else if (act == BTN_ACT_NEXT) cb = s_cb_btn_next;
    if (cb) cb();
}

/* 常驻按钮任务：绑核0 + 高优先级（核1被 MiPlay media task 占满） */
static void _btn_worker_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    btn_action_t act;
    while (1) {
        if (xQueueReceive(q, &act, portMAX_DELAY) == pdTRUE) {
            btn_dispatch(act);
        }
    }
}

static void _btn_start_worker(QueueHandle_t q, const char *name,
                              StaticTask_t *tcb, StackType_t **stack)
{
    if (!q) return;
    if (!*stack) {
        *stack = heap_caps_malloc(LVGL_BTN_ACTION_STACK_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!*stack) {
        ESP_LOGE(TAG, "%s stack alloc failed", name);
        return;
    }
    /* TCB 必须 memset（内部 RAM，不能放 PSRAM） */
    memset(tcb, 0, sizeof(*tcb));
    if (!xTaskCreateStaticPinnedToCore(
            _btn_worker_task, name,
            LVGL_BTN_ACTION_STACK_BYTES / sizeof(StackType_t), (void *)q,
            6, *stack, tcb, 0)) {
        ESP_LOGE(TAG, "%s create failed", name);
    }
}

static void _btn_worker_start(void)
{
    if (s_btn_queue_ctrl) return;
    s_btn_queue_ctrl  = xQueueCreate(BTN_QUEUE_LEN, sizeof(btn_action_t));
    s_btn_queue_track = xQueueCreate(BTN_QUEUE_LEN, sizeof(btn_action_t));
    if (!s_btn_queue_ctrl || !s_btn_queue_track) {
        ESP_LOGE(TAG, "btn queue create failed");
        return;
    }
    _btn_start_worker(s_btn_queue_ctrl,  "btn_ctrl",
                      &s_btn_ctrl_tcb,  &s_btn_ctrl_stack);
    _btn_start_worker(s_btn_queue_track, "btn_track",
                      &s_btn_track_tcb, &s_btn_track_stack);
}

/* 按下即响应（PRESSED 而非 CLICKED）：手指微动不会被判为拖拽而丢失点击 */
static void _btn_press_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    btn_action_t act = 0;
    QueueHandle_t q = NULL;
    if (btn == s_btn_prev)      { act = BTN_ACT_PREV; q = s_btn_queue_track; }
    else if (btn == s_btn_play) { act = BTN_ACT_PLAY; q = s_btn_queue_ctrl;  }
    else if (btn == s_btn_next) { act = BTN_ACT_NEXT; q = s_btn_queue_track; }
    else return;

    /* 只投递，绝不阻塞 LVGL 任务；队列满时丢弃（用户会再按） */
    if (q) {
        if (q == s_btn_queue_track) s_btn_track_seq++;
        xQueueSend(q, &act, 0);
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
static lv_obj_t *s_lyrics_label_title = NULL;   /* 歌词界面顶部歌曲名 */
static lv_obj_t *s_lyrics_label_artist = NULL;  /* 歌词界面顶部歌手名 */
static lv_obj_t *s_lyrics_bar_progress = NULL;  /* 歌词界面底部进度条 */
static lv_obj_t *s_lyrics_label_time1 = NULL;   /* 歌词界面当前播放时间 */
static lv_obj_t *s_lyrics_label_time2 = NULL;   /* 歌词界面总时长 */
static lv_obj_t *s_lyrics_hint_back = NULL;     /* 歌词界面返回提示 */
static int s_lyrics_current = -1;
static int s_lyrics_prev_line = -1;        /* 上一次的行号，用于检测切换动画 */
static bool s_lyrics_visible = false;
static bool s_cover_ready = false;         /* 封面图片已加载就绪 */
/* ── 待机时钟：非播放态无触摸超时进入 ── */
#define STANDBY_IDLE_MS 300000U            /* 5 分钟无操作进入休眠屏保 */
static uint32_t s_last_touch_tick = 0;     /* 上次触摸时刻（lv_tick） */
static bool s_playing_state = false;       /* 由 lvgl_port_ui_set_state 维护 */
static lv_obj_t *s_lyrics_bg_img = NULL; /* 歌词界面背景图 */
/* 背景渐变（预抖动图片，消除 RGB565 色阶） */
static lv_obj_t *s_bg_img = NULL;        /* 背景图片控件 */
static lv_img_dsc_t *s_bg_dsc = NULL;    /* 背景图片描述符（PSRAM 像素数据） */
static void _generate_dithered_bg(uint8_t r_top, uint8_t g_top, uint8_t b_top,
                                   uint8_t r_bot, uint8_t g_bot, uint8_t b_bot);

/* Brookesia 360x360 system UI */
#define GESTURE_EDGE_PX       25
#define GESTURE_DISTANCE_PX   35
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
static lv_obj_t *s_status_wifi_caption;   /* Wi-Fi 按钮下方文字（显示当前 SSID） */
static lv_font_t s_status_wifi_caption_font;
static lv_obj_t *s_status_volume_btn;
static lv_obj_t *s_status_brightness_btn;

/* ── 胶囊弹窗（音量/亮度） ── */
typedef enum {
    CAPSULE_POPUP_VOLUME = 0,
    CAPSULE_POPUP_BRIGHTNESS,
} capsule_popup_type_t;

static lv_obj_t *s_capsule_card;          /* 弹窗卡片容器 */
static lv_obj_t *s_capsule_icon;          /* 顶部图标 */
static lv_obj_t *s_capsule_label;         /* 百分比文字 */
static lv_obj_t *s_capsule_slider;        /* 滑块 */
static lv_obj_t *s_capsule_backdrop;      /* 半透明遮罩 */
static lv_timer_t *s_capsule_hide_timer;  /* 自动隐藏定时器 */
static lv_timer_t *s_brightness_save_timer; /* NVS 防抖 */
static lv_timer_t *s_volume_save_timer;     /* NVS 防抖（音量） */
static capsule_popup_type_t s_capsule_type = CAPSULE_POPUP_VOLUME;
static int s_capsule_current_value;       /* 当前值 0-100 */
static bool s_capsule_visible;
static uint8_t s_brightness_current = 100;
static int s_volume_current = 50;          /* 本地音量镜像 */

/* 音量/亮度外部回调 */
typedef void (*capsule_value_cb_t)(int value);
static capsule_value_cb_t s_capsule_volume_cb = NULL;
static capsule_value_cb_t s_capsule_brightness_cb = NULL;

/* NVS 句柄 */
static nvs_handle_t s_nvs_brightness = 0;
static nvs_handle_t s_nvs_volume = 0;
static lv_obj_t *s_status_sram_bar;
static lv_obj_t *s_status_psram_bar;
static lv_obj_t *s_wlan_scr;
static lv_obj_t *s_softap_scr;
static lv_obj_t *s_standby_scr;
static lv_obj_t *s_standby_time_label;
static lv_obj_t *s_wlan_connected_label;
static lv_obj_t *s_wlan_available_label;
static lv_obj_t *s_softap_qrcode;
static lv_obj_t *s_softap_info_label;
static lv_obj_t *s_wlan_back_btn;
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
static lv_obj_t *s_wlan_password_scr;
static lv_obj_t *s_wlan_password_back_btn;
static lv_obj_t *s_wlan_password_ssid_label;
static lv_obj_t *s_wlan_password_ta;
static lv_obj_t *s_wlan_password_kb;
static char s_wlan_password_ssid[33];
static bool s_provision_return_done;
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

static void _show_wlan(void);
static void _show_softap(void);
static void _show_wlan_password(const char *ssid);
static void _create_wlan_password_screen(void);
static void _wlan_connect_selected(const char *ssid, const char *password);
static void _wlan_network_cell_event_cb(lv_event_t *event);
static void _wlan_password_event_cb(lv_event_t *event);
static void _maybe_return_after_provision(void);
static void _pause_playback_if_playing(void);
static void _show_main_screen(void);
static void _show_parent_screen(void);
static void _show_standby_screen(void);
static void _create_standby_screen(void);
static void _update_standby_time(void);
static void _update_system_status(void);
static void _gesture_timer_cb(lv_timer_t *timer);
static void _wifi_status_timer_cb(lv_timer_t *timer);
static void _request_wlan_scan(void);
static void _apply_wlan_scan_results(void);
static void _show_status_panel(bool visible);
static void _show_return_bar(bool visible);
static void _system_button_event_cb(lv_event_t *event);
static void _show_capsule_popup(capsule_popup_type_t type, int value);
static void _hide_capsule_popup(void);

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
#define UI_BUTTON_CLICK_PAD 8
/* 侧边按钮点击区 10px（14px 会与 Play 按钮重叠 2px，导致误触） */
#define UI_BUTTON_CLICK_PAD_SIDE 10
#define UI_BUTTON_CENTER_Y  316
/* 侧边按钮向屏幕外侧额外扩展的宽度（手指容易偏到按钮外侧） */
#define UI_BUTTON_EXTRA_OUTWARD 26

/* 非对称外扩热区：direction=-1 向左扩（prev），+1 向右扩（next）。
 * ext_click_area 只能四周对称扩展，无法只扩外侧，故用透明容器补一段。 */
static void _hotzone_press_cb(lv_event_t *e)
{
    btn_action_t act = (btn_action_t)(intptr_t)lv_event_get_user_data(e);
    QueueHandle_t q = (act == BTN_ACT_PLAY) ? s_btn_queue_ctrl : s_btn_queue_track;
    if (!q) return;
    if (q == s_btn_queue_track) s_btn_track_seq++;
    xQueueSend(q, &act, 0);
}

static void _create_outward_hotzone(lv_obj_t *parent, lv_obj_t *btn,
                                    btn_action_t act, int direction)
{
    lv_obj_t *zone = lv_obj_create(parent);
    lv_obj_remove_style_all(zone);
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(zone, UI_BUTTON_EXTRA_OUTWARD,
                    UI_BTN_SIDE_SIZE + 2 * UI_BUTTON_CLICK_PAD_SIDE);
    lv_obj_align_to(zone, btn, direction < 0 ? LV_ALIGN_OUT_LEFT_MID
                                             : LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(zone, _hotzone_press_cb, LV_EVENT_PRESSED,
                        (void *)(intptr_t)act);
    /* 热区放最底层，不遮挡按钮自身的按压反馈 */
    lv_obj_move_background(zone);
}

static void _style_screen(lv_obj_t *screen)
{
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, TFT_W, TFT_H);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    /* 不设 radius/clip_corner，让背景铺满整个方形，圆屏硬件自然裁切 */
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
    lv_obj_set_size(cell, 320, left_minor_text ? 72 : 48);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(cell, C_WHITE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

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
    const lv_coord_t cell_h = minor_text ? 72 : 48;
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, 320, cell_h);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell, C_WHITE, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cell, LV_OPA_10, LV_STATE_PRESSED);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    if (clickable) lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *left_area = lv_obj_create(cell);
    lv_obj_remove_style_all(left_area);
    lv_obj_set_size(left_area, 200, cell_h);
    lv_obj_align(left_area, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_flex_flow(left_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left_area, 8, 0);
    lv_obj_clear_flag(left_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_label = _create_text(left_area, ssid ? ssid : "",
                                        &s_wlan_ssid_font, C_WHITE);
    /* 高度必须用字体实际行高：固定 24px 会把 22 号字裁到不可见 */
    lv_obj_set_width(main_label, 200);
    lv_obj_set_height(main_label, lv_font_get_line_height(&s_wlan_ssid_font));
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
    lv_obj_set_width(main, 320);
    lv_obj_set_height(main, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(main, 8, 0);
    lv_obj_clear_flag(main, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = _create_text(main, title ? title : "",
                                          &esp_brookesia_font_maison_neue_book_16,
                                          lv_color_hex(0x888888));
    if (!title || title[0] == '\0') lv_obj_add_flag(title_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *container = lv_obj_create(main);
    lv_obj_remove_style_all(container);
    lv_obj_set_width(container, 320);
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
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);

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
                                      const char *caption, bool checkable,
                                      lv_obj_t **caption_out)
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
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    /* 热区外扩：圆屏弧形边缘触摸落点偏移时仍可命中 */
    lv_obj_set_ext_click_area(icon, 12);
    lv_obj_set_style_radius(icon, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_img_src(icon, icon_source, LV_PART_MAIN);
    lv_obj_set_style_bg_color(icon, lv_color_hex(0x00AADD), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    if (checkable) {
        lv_obj_set_style_bg_color(icon, C_RED, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
    }

    lv_obj_t *caption_label = _create_text(button, caption, &esp_brookesia_font_maison_neue_book_16, C_WHITE);
    lv_obj_set_style_pad_bottom(caption_label, 2, 0);
    if (caption_out) *caption_out = caption_label;
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

/* ════════════════════════════════════════════════
 *  胶囊弹窗（音量 / 亮度）
 * ════════════════════════════════════════════════ */

#define CAPSULE_CARD_W     62
#define CAPSULE_CARD_H     220
#define CAPSULE_HIDE_MS    3000
#define BRIGHTNESS_SAVE_MS 1500

static void _load_brightness_from_nvs(void)
{
    nvs_handle_t h = 0;
    if (nvs_open("display", NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, "brightness", &val) == ESP_OK) {
            s_brightness_current = val;
        }
        nvs_close(h);
    }
}

static void _save_brightness_to_nvs(uint8_t val)
{
    if (!s_nvs_brightness) {
        if (nvs_open("display", NVS_READWRITE, &s_nvs_brightness) != ESP_OK) {
            ESP_LOGW(TAG, "NVS open brightness failed");
            return;
        }
    }
    nvs_set_u8(s_nvs_brightness, "brightness", val);
    nvs_commit(s_nvs_brightness);
    ESP_LOGI(TAG, "Brightness %d saved to NVS", val);
}

static void _brightness_save_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    _save_brightness_to_nvs(s_brightness_current);
    if (s_brightness_save_timer) {
        lv_timer_del(s_brightness_save_timer);
        s_brightness_save_timer = NULL;
    }
}

static void _brightness_debounced_save(uint8_t val)
{
    s_brightness_current = val;
    if (s_brightness_save_timer) {
        lv_timer_reset(s_brightness_save_timer);
    } else {
        s_brightness_save_timer = lv_timer_create(
            _brightness_save_timer_cb, BRIGHTNESS_SAVE_MS, NULL);
        lv_timer_set_repeat_count(s_brightness_save_timer, 1);
    }
}

/* ── 音量 NVS 持久化 ──
 * 与亮度同款：命名空间 "display"，防抖 500ms 落盘。
 * 直接在 LVGL 定时器里写 flash 是安全的——timer 跑在 lvgl 任务（内部 RAM 栈），
 * 不存在从 PSRAM 栈调用 nvs_commit 的断言问题。 */
static void _save_volume_to_nvs(uint8_t val)
{
    if (!s_nvs_volume) {
        if (nvs_open("display", NVS_READWRITE, &s_nvs_volume) != ESP_OK) {
            ESP_LOGW(TAG, "NVS open volume failed");
            return;
        }
    }
    nvs_set_u8(s_nvs_volume, "volume", val);
    nvs_commit(s_nvs_volume);
    ESP_LOGI(TAG, "Volume %d saved to NVS", val);
}

static void _volume_save_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    _save_volume_to_nvs((uint8_t)s_volume_current);
    if (s_volume_save_timer) {
        lv_timer_del(s_volume_save_timer);
        s_volume_save_timer = NULL;
    }
}

static void _volume_debounced_save(int val)
{
    if (s_volume_save_timer) {
        lv_timer_reset(s_volume_save_timer);
    } else {
        s_volume_save_timer = lv_timer_create(
            _volume_save_timer_cb, 500, NULL);
        lv_timer_set_repeat_count(s_volume_save_timer, 1);
    }
}

uint8_t lvgl_port_ui_load_volume_from_nvs(void)
{
    nvs_handle_t h = 0;
    uint8_t val = 35;   /* 无记录时默认 35%，比 50% 更接近正常听音音量 */
    if (nvs_open("display", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "volume", &val) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded volume %u from NVS", (unsigned)val);
        }
        nvs_close(h);
    }
    if (val > 100) val = 100;
    return val;
}

static void _capsule_opa_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void _capsule_hide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    _hide_capsule_popup();
}

static void _capsule_backdrop_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        _hide_capsule_popup();
    }
}

static void _capsule_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    s_capsule_current_value = val;

    /* 更新百分比文字 */
    if (s_capsule_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(s_capsule_label, buf);
    }

    if (s_capsule_type == CAPSULE_POPUP_VOLUME) {
        if (s_capsule_volume_cb) s_capsule_volume_cb(val);
    } else {
        uint8_t bri = (uint8_t)val;
        tft_set_backlight(bri);
        _brightness_debounced_save(bri);
        if (s_capsule_brightness_cb) s_capsule_brightness_cb(val);
    }

    /* 重置自动隐藏定时器 */
    if (s_capsule_hide_timer) {
        lv_timer_reset(s_capsule_hide_timer);
    }
}

static void _show_capsule_popup(capsule_popup_type_t type, int value)
{
    if (!s_capsule_card) return;

    s_capsule_type = type;
    s_capsule_current_value = value;

    /* 更新图标（32x32 预缩放素材） */
    if (s_capsule_icon) {
        if (type == CAPSULE_POPUP_VOLUME) {
            lv_obj_set_style_bg_img_src(s_capsule_icon, &capsule_vol_32_32, 0);
        } else {
            lv_obj_set_style_bg_img_src(s_capsule_icon, &capsule_bri_32_32, 0);
        }
    }

    /* 更新文字 */
    if (s_capsule_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(s_capsule_label, buf);
    }

    /* 更新滑块范围 */
    if (s_capsule_slider) {
        if (type == CAPSULE_POPUP_VOLUME) {
            lv_slider_set_range(s_capsule_slider, 0, 100);
        } else {
            lv_slider_set_range(s_capsule_slider, 10, 100);
        }
        lv_slider_set_value(s_capsule_slider, value, LV_ANIM_OFF);
    }

    /* 显示 + 淡入动画（图标已回归卡内 flex，随卡片一起显示） */
    lv_obj_set_style_opa(s_capsule_card, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_capsule_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_capsule_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_capsule_backdrop);
    lv_obj_move_foreground(s_capsule_card);
    s_capsule_visible = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_capsule_card);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_exec_cb(&a, _capsule_opa_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* 自动隐藏定时器 */
    if (s_capsule_hide_timer) {
        lv_timer_reset(s_capsule_hide_timer);
    } else {
        s_capsule_hide_timer = lv_timer_create(
            _capsule_hide_timer_cb, CAPSULE_HIDE_MS, NULL);
        lv_timer_set_repeat_count(s_capsule_hide_timer, 1);
    }

    ESP_LOGI(TAG, "Capsule popup: type=%d value=%d", type, value);
}

static void _hide_capsule_popup(void)
{
    if (!s_capsule_visible) return;
    if (s_capsule_backdrop) lv_obj_add_flag(s_capsule_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (s_capsule_card) lv_obj_add_flag(s_capsule_card, LV_OBJ_FLAG_HIDDEN);
    s_capsule_visible = false;

    if (s_capsule_hide_timer) {
        lv_timer_del(s_capsule_hide_timer);
        s_capsule_hide_timer = NULL;
    }
    ESP_LOGI(TAG, "Capsule popup hidden");
}

static void _create_capsule_popup(lv_obj_t *layer)
{
    /* 半透明遮罩 — 加深到 70% 增强注意力聚焦 */
    s_capsule_backdrop = lv_obj_create(layer);
    lv_obj_remove_style_all(s_capsule_backdrop);
    lv_obj_set_size(s_capsule_backdrop, TFT_W, TFT_H);
    lv_obj_set_style_bg_color(s_capsule_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_capsule_backdrop, LV_OPA_70, 0);
    lv_obj_add_flag(s_capsule_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_capsule_backdrop, _capsule_backdrop_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_capsule_backdrop, LV_OBJ_FLAG_HIDDEN);

    /* 胶囊卡片 — 完全胶囊形态，与快捷按钮同色 */
    s_capsule_card = lv_obj_create(layer);
    lv_obj_remove_style_all(s_capsule_card);
    lv_obj_set_size(s_capsule_card, CAPSULE_CARD_W, CAPSULE_CARD_H);
    lv_obj_align(s_capsule_card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(s_capsule_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_capsule_card, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(s_capsule_card, 255, 0);          /* 完全胶囊 */
    lv_obj_set_style_bg_color(s_capsule_card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(s_capsule_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(s_capsule_card, 18, 0);
    lv_obj_set_style_pad_bottom(s_capsule_card, 18, 0);
    lv_obj_set_style_pad_left(s_capsule_card, 0, 0);
    lv_obj_set_style_pad_right(s_capsule_card, 0, 0);
    lv_obj_clear_flag(s_capsule_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_capsule_card, LV_OBJ_FLAG_HIDDEN);

    /* 图标：预缩放 32x32 素材，bg_img_src 原生尺寸渲染（零缩放零裁切） */
    s_capsule_icon = lv_obj_create(s_capsule_card);
    lv_obj_remove_style_all(s_capsule_icon);
    lv_obj_set_size(s_capsule_icon, 32, 32);
    lv_obj_set_style_bg_img_src(s_capsule_icon, &capsule_vol_32_32, 0);
    lv_obj_clear_flag(s_capsule_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 百分比文字 */
    s_capsule_label = lv_label_create(s_capsule_card);
    lv_label_set_text(s_capsule_label, "0%");
    lv_obj_set_style_text_color(s_capsule_label, C_WHITE, 0);
    lv_obj_set_style_text_font(s_capsule_label, &esp_brookesia_font_maison_neue_book_16, 0);

    /* 滑块（竖向）— 细轨无 knob，iOS/手环式 */
    s_capsule_slider = lv_slider_create(s_capsule_card);
    lv_obj_set_size(s_capsule_slider, 6, 110);
    lv_slider_set_range(s_capsule_slider, 0, 100);
    lv_slider_set_value(s_capsule_slider, 50, LV_ANIM_OFF);
    lv_slider_set_mode(s_capsule_slider, LV_SLIDER_MODE_NORMAL);
    lv_obj_set_style_bg_color(s_capsule_slider, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_capsule_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_capsule_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_capsule_slider, C_WHITE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_capsule_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_capsule_slider, 3, LV_PART_INDICATOR);
    /* 隐藏 knob：透明+零尺寸，位置由 indicator 填充表达 */
    lv_obj_set_style_bg_opa(s_capsule_slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_capsule_slider, 0, LV_PART_KNOB);
    /* 触摸热区扩大：左右各+24px，上下各+16px */
    lv_obj_set_ext_click_area(s_capsule_slider, 24);
    lv_obj_add_event_cb(s_capsule_slider, _capsule_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

static void _create_system_ui(void)
{
    lv_obj_t *layer = lv_layer_top();
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

    s_status_panel = lv_obj_create(layer);
    lv_obj_remove_style_all(s_status_panel);
    lv_obj_set_size(s_status_panel, TFT_W, STATUS_PANEL_H);
    lv_obj_set_pos(s_status_panel, 0, STATUS_PANEL_HIDDEN_Y);
    lv_obj_set_flex_flow(s_status_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_status_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_status_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_status_panel, 0, 0);
    lv_obj_set_style_pad_right(s_status_panel, 0, 0);
    lv_obj_set_style_pad_top(s_status_panel, 50, 0);
    lv_obj_set_style_pad_bottom(s_status_panel, 0, 0);
    lv_obj_set_style_clip_corner(s_status_panel, true, 0);

    /* This hierarchy mirrors the reference QuickSettings component. */
    lv_obj_t *status = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(status);
    lv_obj_set_width(status, lv_pct(100));
    lv_obj_set_height(status, LV_SIZE_CONTENT);
    lv_obj_set_x(status, 0);
    lv_obj_set_y(status, 0);
    lv_obj_set_align(status, LV_ALIGN_TOP_MID);
    lv_obj_set_style_pad_left(status, 20, 0);
    lv_obj_set_style_pad_right(status, 20, 0);
    lv_obj_set_style_pad_top(status, 0, 0);
    lv_obj_set_style_pad_bottom(status, 20, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *status_internal = lv_obj_create(status);
    lv_obj_remove_style_all(status_internal);
    lv_obj_set_width(status_internal, lv_pct(85));
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
    lv_obj_set_x(status_top, 0);
    lv_obj_set_align(status_top, LV_ALIGN_CENTER);
    lv_obj_clear_flag(status_top, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 时间 + 电量：居中一行，小米手环风格 */
    s_status_time_label = _create_text(status_top, "12:00",
                                       &esp_brookesia_font_maison_neue_book_20, C_WHITE);
    lv_obj_set_align(s_status_time_label, LV_ALIGN_CENTER);
    lv_obj_set_x(s_status_time_label, -40);  /* 整体偏左，与电量组视觉居中 */

    lv_obj_t *bat_group = lv_obj_create(status_top);
    lv_obj_remove_style_all(bat_group);
    lv_obj_set_size(bat_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(bat_group, LV_ALIGN_CENTER, 40, 0);
    lv_obj_set_flex_flow(bat_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bat_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bat_group, 4, 0);
    lv_obj_clear_flag(bat_group, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_status_battery_icon = _create_image(bat_group,
                                          &speaker_image_middle_quick_settings_battery_charge_20_20,
                                          20, C_WHITE, false);
    s_status_battery_label = _create_text(bat_group, "100%",
                                          &esp_brookesia_font_maison_neue_book_16, C_WHITE);

    /* WiFi 图标保留变量但隐藏（_update_system_status 引用） */
    s_status_wifi_label = _create_image(status_top,
                                        &speaker_image_middle_quick_settings_wifi_close_20_20,
                                        20, C_WHITE, false);
    lv_obj_add_flag(s_status_wifi_label, LV_OBJ_FLAG_HIDDEN);

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
        buttons, &speaker_image_middle_quick_settings_wifi_48_48, "Wi-Fi", true,
        &s_status_wifi_caption);
    s_status_volume_btn = _create_quick_button(
        buttons, &speaker_image_middle_quick_settings_volume_high_48_48, "Volume", false, NULL);
    s_status_brightness_btn = _create_quick_button(
        buttons, &speaker_image_middle_quick_settings_brightness_high_48_48, "Brightness", false, NULL);
    /* Wi-Fi 按钮下方显示当前 SSID：宽度受限，用 DOT 截断 + CJK fallback */
    if (s_status_wifi_caption) {
        lv_obj_set_style_text_font(s_status_wifi_caption, &s_status_wifi_caption_font, 0);
        lv_label_set_long_mode(s_status_wifi_caption, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_status_wifi_caption, 88);
        lv_obj_set_style_text_align(s_status_wifi_caption, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_add_event_cb(s_status_wifi_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_status_volume_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_status_brightness_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    /* memory bars — 保留 SRAM/PSRAM 监控 */
    lv_obj_t *memory = lv_obj_create(s_status_panel);
    lv_obj_remove_style_all(memory);
    lv_obj_set_width(memory, lv_pct(100));
    lv_obj_set_height(memory, LV_SIZE_CONTENT);
    lv_obj_set_align(memory, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(memory, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(memory, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(memory, 40, 0);
    lv_obj_set_style_pad_right(memory, 40, 0);
    lv_obj_set_style_pad_top(memory, 30, 0);
    lv_obj_set_style_pad_bottom(memory, 40, 0);
    lv_obj_set_style_pad_row(memory, 8, 0);
    lv_obj_clear_flag(memory, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *memory_internal = lv_obj_create(memory);
    lv_obj_remove_style_all(memory_internal);
    lv_obj_set_width(memory_internal, lv_pct(100));
    lv_obj_set_height(memory_internal, LV_SIZE_CONTENT);
    lv_obj_set_align(memory_internal, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(memory_internal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(memory_internal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(memory_internal, 6, 0);
    lv_obj_clear_flag(memory_internal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sram = lv_obj_create(memory_internal);
    lv_obj_remove_style_all(sram);
    lv_obj_set_width(sram, lv_pct(100));
    lv_obj_set_height(sram, LV_SIZE_CONTENT);
    lv_obj_set_align(sram, LV_ALIGN_RIGHT_MID);
    lv_obj_clear_flag(sram, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    _create_text(sram, "SRAM:", &esp_brookesia_font_maison_neue_book_12, C_WHITE80);
    s_status_sram_bar = lv_bar_create(sram);
    lv_bar_set_value(s_status_sram_bar, 50, LV_ANIM_OFF);
    lv_bar_set_start_value(s_status_sram_bar, 0, LV_ANIM_OFF);
    lv_obj_set_height(s_status_sram_bar, 8);
    lv_obj_set_width(s_status_sram_bar, lv_pct(65));
    lv_obj_set_align(s_status_sram_bar, LV_ALIGN_RIGHT_MID);

    lv_obj_t *psram = lv_obj_create(memory_internal);
    lv_obj_remove_style_all(psram);
    lv_obj_set_width(psram, lv_pct(100));
    lv_obj_set_height(psram, LV_SIZE_CONTENT);
    lv_obj_set_align(psram, LV_ALIGN_CENTER);
    lv_obj_clear_flag(psram, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    _create_text(psram, "PSRAM:", &esp_brookesia_font_maison_neue_book_12, C_WHITE80);
    s_status_psram_bar = lv_bar_create(psram);
    lv_bar_set_value(s_status_psram_bar, 50, LV_ANIM_OFF);
    lv_bar_set_start_value(s_status_psram_bar, 0, LV_ANIM_OFF);
    lv_obj_set_height(s_status_psram_bar, 8);
    lv_obj_set_width(s_status_psram_bar, lv_pct(65));
    lv_obj_set_align(s_status_psram_bar, LV_ALIGN_RIGHT_MID);

    lv_obj_t *memory_bars[] = {s_status_sram_bar, s_status_psram_bar};
    for (size_t i = 0; i < sizeof(memory_bars) / sizeof(memory_bars[0]); ++i) {
        lv_obj_set_style_bg_color(memory_bars[i], lv_color_hex(0x2A2A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(memory_bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(memory_bars[i], lv_color_hex(0x34C759), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(memory_bars[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(memory_bars[i], 4, LV_PART_MAIN);
        lv_obj_set_style_radius(memory_bars[i], 4, LV_PART_INDICATOR);
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

    /* 胶囊弹窗（音量/亮度） */
    _create_capsule_popup(layer);
    _load_brightness_from_nvs();
    tft_set_backlight(s_brightness_current);
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
    return content;
}

static void _create_wlan_screen(void)
{
    s_wlan_scr = lv_obj_create(NULL);
    _style_screen(s_wlan_scr);
    s_wlan_back_btn = _create_header_button(s_wlan_scr, "Player");
    lv_obj_add_event_cb(s_wlan_back_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *content = _create_settings_content(s_wlan_scr);

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
    lv_obj_set_width(qr_cell, 320);
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

/* ══ 配网：点击网络→连接 / 密码输入屏（圆屏键盘适配）══ */
static void _wlan_connect_selected(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return;
    if (wifi_provision_get_status() == WIFI_PROVISION_STATUS_CONNECTING) return;
    esp_err_t ret = wifi_provision_connect(ssid, password ? password : "");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WLAN connect start failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Connecting to '%s'", ssid);
    _update_system_status();
}

static void _wlan_network_cell_event_cb(lv_event_t *event)
{
    if (!event) return;
    wlan_scan_item_t *item = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        free(item);
        return;
    }
    if (code != LV_EVENT_CLICKED || !item || item->ssid[0] == '\0') return;
    if (item->authmode == WIFI_AUTH_OPEN) {
        _wlan_connect_selected(item->ssid, "");
    } else {
        _show_wlan_password(item->ssid);
    }
}

static void _wlan_password_event_cb(lv_event_t *event)
{
    if (!event) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        _show_wlan();
        return;
    }
    if (code != LV_EVENT_READY || !s_wlan_password_ta) return;

    const char *password = lv_textarea_get_text(s_wlan_password_ta);
    if (!password || strlen(password) < 8) {
        ESP_LOGW(TAG, "WLAN password too short");
        return;
    }
    _wlan_connect_selected(s_wlan_password_ssid, password);
    _show_wlan();
}

#define WLAN_KB_BTN(width)  (LV_BUTTONMATRIX_CTRL_POPOVER | (lv_buttonmatrix_ctrl_t)(width))
#define WLAN_KB_CTRL(width) (LV_KEYBOARD_CTRL_BUTTON_FLAGS | (lv_buttonmatrix_ctrl_t)(width))
#define WLAN_KB_PHR(width)  (LV_BUTTONMATRIX_CTRL_HIDDEN | (lv_buttonmatrix_ctrl_t)(width))
#define WLAN_KB_PHR_STR     "  "

static const char *const s_wlan_kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    WLAN_KB_PHR_STR, "a", "s", "d", "f", "g", "h", "j", "k", "l", WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, "ABC", "z", "x", "c", "v", "b", "n", "m", WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, "1#", " ", LV_SYMBOL_BACKSPACE, WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, LV_SYMBOL_LEFT, LV_SYMBOL_OK, LV_SYMBOL_RIGHT, WLAN_KB_PHR_STR, ""
};

static const char *const s_wlan_kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    WLAN_KB_PHR_STR, "A", "S", "D", "F", "G", "H", "J", "K", "L", WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, "abc", "Z", "X", "C", "V", "B", "N", "M", WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, "1#", " ", LV_SYMBOL_BACKSPACE, WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, LV_SYMBOL_LEFT, LV_SYMBOL_OK, LV_SYMBOL_RIGHT, WLAN_KB_PHR_STR, ""
};

static const char *const s_wlan_kb_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    WLAN_KB_PHR_STR, "@", "#", "!", "*", "&", "%", "-", "+", "=", WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, "abc", "_", "/", ":", ";", "(", ")", "?", WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, ".", " ", LV_SYMBOL_BACKSPACE, WLAN_KB_PHR_STR, "\n",
    WLAN_KB_PHR_STR, LV_SYMBOL_LEFT, LV_SYMBOL_OK, LV_SYMBOL_RIGHT, WLAN_KB_PHR_STR, ""
};

static const lv_buttonmatrix_ctrl_t s_wlan_kb_ctrl_map[] = {
    WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2),
    WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2),
    WLAN_KB_PHR(1), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2),
    WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_PHR(1),
    WLAN_KB_PHR(1), WLAN_KB_CTRL(3), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2),
    WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_BTN(2), WLAN_KB_PHR(2),
    WLAN_KB_PHR(2), WLAN_KB_CTRL(3), WLAN_KB_BTN(8), WLAN_KB_BTN(4), WLAN_KB_PHR(3),
    WLAN_KB_PHR(5), WLAN_KB_BTN(4), WLAN_KB_CTRL(6), WLAN_KB_BTN(4), WLAN_KB_PHR(5)
};

static void _wlan_keyboard_set_round_maps(lv_obj_t *keyboard)
{
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, s_wlan_kb_map_lc, s_wlan_kb_ctrl_map);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, s_wlan_kb_map_uc, s_wlan_kb_ctrl_map);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, s_wlan_kb_map_spec, s_wlan_kb_ctrl_map);
}

static void _create_wlan_password_screen(void)
{
    s_wlan_password_scr = lv_obj_create(NULL);
    _style_screen(s_wlan_password_scr);
    s_wlan_password_back_btn = _create_header_button(s_wlan_password_scr, "");
    lv_obj_add_event_cb(s_wlan_password_back_btn, _system_button_event_cb, LV_EVENT_CLICKED, NULL);

    s_wlan_password_ssid_label = _create_text(s_wlan_password_scr, "",
                                              &esp_brookesia_font_maison_neue_book_22, C_WHITE);
    lv_obj_set_width(s_wlan_password_ssid_label, 280);
    lv_obj_set_style_text_align(s_wlan_password_ssid_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_wlan_password_ssid_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_wlan_password_ssid_label, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *pwd_box = lv_obj_create(s_wlan_password_scr);
    lv_obj_remove_style_all(pwd_box);
    lv_obj_set_size(pwd_box, 320, 56);
    lv_obj_align(pwd_box, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_radius(pwd_box, 16, 0);
    lv_obj_set_style_bg_color(pwd_box, lv_color_hex(0x38393A), 0);
    lv_obj_set_style_bg_opa(pwd_box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(pwd_box, 16, 0);
    lv_obj_set_style_pad_right(pwd_box, 16, 0);
    lv_obj_clear_flag(pwd_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_wlan_password_ta = lv_textarea_create(pwd_box);
    lv_obj_set_width(s_wlan_password_ta, 320);
    lv_obj_set_height(s_wlan_password_ta, 44);
    lv_obj_align(s_wlan_password_ta, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_one_line(s_wlan_password_ta, true);
    lv_textarea_set_password_mode(s_wlan_password_ta, true);
    lv_textarea_set_password_bullet(s_wlan_password_ta, "*");
    lv_textarea_set_placeholder_text(s_wlan_password_ta, "Password");
    lv_textarea_set_max_length(s_wlan_password_ta, 64);
    lv_obj_set_style_text_font(s_wlan_password_ta, &esp_brookesia_font_maison_neue_book_22, 0);
    lv_obj_set_style_text_color(s_wlan_password_ta, C_WHITE, 0);
    lv_obj_set_style_text_color(s_wlan_password_ta, lv_color_hex(0x888888), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_opa(s_wlan_password_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wlan_password_ta, 0, 0);
    lv_obj_set_style_pad_all(s_wlan_password_ta, 8, 0);
    lv_obj_add_event_cb(s_wlan_password_ta, _wlan_password_event_cb, LV_EVENT_READY, NULL);

    s_wlan_password_kb = lv_keyboard_create(s_wlan_password_scr);
    lv_obj_set_size(s_wlan_password_kb, (TFT_W * 94) / 100, 176);
    lv_obj_align(s_wlan_password_kb, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(s_wlan_password_kb, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wlan_password_kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wlan_password_kb, lv_color_hex(0x2A2A2A), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_wlan_password_kb, C_WHITE, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_wlan_password_kb, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_wlan_password_kb, 4, 0);
    lv_obj_set_style_pad_bottom(s_wlan_password_kb, 8, 0);
    lv_obj_set_style_pad_left(s_wlan_password_kb, 6, 0);
    lv_obj_set_style_pad_right(s_wlan_password_kb, 6, 0);
    lv_obj_set_style_pad_row(s_wlan_password_kb, 6, 0);
    lv_obj_set_style_pad_column(s_wlan_password_kb, 4, 0);
    lv_obj_set_style_text_font(s_wlan_password_kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_wlan_password_kb, s_wlan_password_ta);
    _wlan_keyboard_set_round_maps(s_wlan_password_kb);
    lv_keyboard_set_mode(s_wlan_password_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_wlan_password_kb, _wlan_password_event_cb, LV_EVENT_CANCEL, NULL);
}

static void _wlan_scan_task(void *arg)
{
    (void)arg;
    /* MiPlay 推流期间 WiFi 驱动繁忙，默认每信道 120ms 驻留常常空手而归。
     * 加长每信道主动扫描时间到 300ms，并把省电模式临时关掉，扫描更完整。 */
    wifi_scan_config_t config = {
        .show_hidden = false,
        .scan_time.active = {
            .min = 100,
            .max = 300,
        },
    };
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_ap_record_t records[WLAN_SCAN_MAX_ITEMS] = {0};
    uint16_t record_count = WLAN_SCAN_MAX_ITEMS;
    esp_err_t ret = esp_wifi_scan_start(&config, true);
    if (ret == ESP_OK) ret = esp_wifi_scan_get_ap_records(&record_count, records);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

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

    wifi_ap_record_t ap = {0};
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const char *skip_ssid = NULL;
    if (wifi_provision_get_status() == WIFI_PROVISION_STATUS_CONNECTING) {
        skip_ssid = wifi_provision_get_target_ssid();
    }

    lv_obj_clean(s_wlan_available_container);
    uint16_t shown = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (connected && strcmp(results[i].ssid, (const char *)ap.ssid) == 0) continue;
        if (skip_ssid && skip_ssid[0] && strcmp(results[i].ssid, skip_ssid) == 0) continue;

        lv_obj_t *cell = _create_wlan_network_cell(s_wlan_available_container, results[i].ssid, NULL,
                                                   results[i].rssi,
                                                   results[i].authmode != WIFI_AUTH_OPEN,
                                                   i + 1 < count, true, NULL, NULL, NULL, NULL);
        wlan_scan_item_t *item = malloc(sizeof(*item));
        if (item) {
            *item = results[i];
            lv_obj_add_event_cb(cell, _wlan_network_cell_event_cb, LV_EVENT_CLICKED, item);
            lv_obj_add_event_cb(cell, _wlan_network_cell_event_cb, LV_EVENT_DELETE, item);
        }
        shown++;
    }
    if (!count) {
        _create_wlan_network_cell(s_wlan_available_container, "No networks found", NULL,
                                  -100, false, false, false, NULL, NULL, NULL, NULL);
    }
    lv_obj_clear_flag(s_wlan_available_group, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "WLAN scan UI updated: %u networks", (unsigned)shown);
}

static void _update_system_status(void)
{
    if (!s_status_time_label) return;

    time_t now = time(NULL);
    struct tm local_time = {0};
    if (localtime_r(&now, &local_time) == NULL) memset(&local_time, 0, sizeof(local_time));
    lv_label_set_text_fmt(s_status_time_label, "%02d:%02d", local_time.tm_hour, local_time.tm_min);
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
    /* Wi-Fi 按钮下方显示当前网络名：优先已连接 AP，其次配网目标，最后 NVS 里保存的 SSID */
    if (s_status_wifi_caption) {
        char ssid_text[33] = "Wi-Fi";
        if (connected && ap.ssid[0]) {
            memcpy(ssid_text, ap.ssid, sizeof(ap.ssid));
            ssid_text[sizeof(ssid_text) - 1] = '\0';
        } else {
            const char *target_ssid = wifi_provision_get_target_ssid();
            if (target_ssid && target_ssid[0]) {
                snprintf(ssid_text, sizeof(ssid_text), "%s", target_ssid);
            } else {
                wifi_config_t sta_cfg = {0};
                if (esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK &&
                    sta_cfg.sta.ssid[0]) {
                    memcpy(ssid_text, sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid));
                    ssid_text[sizeof(ssid_text) - 1] = '\0';
                }
            }
        }
        if (strcmp(lv_label_get_text(s_status_wifi_caption), ssid_text) != 0) {
            lv_label_set_text(s_status_wifi_caption, ssid_text);
        }
    }
    /* 电量：真实 ADC 采样。状态栏 1s 刷新，但电池电压变化以分钟计，
     * 这里 10s 才真正采一次，其余刷新复用缓存值，省电且避免 ADC 抖动。
     * 充电状态单独 2s 检查一次（比电量更频繁），确保拔线后快速切图标。 */
    static int s_bat_pct_cached = 0;
    static bool s_bat_charging_cached = false;
    static uint8_t s_bat_tick = 0;
    static uint8_t s_chg_tick = 0;

    /* 充电状态：每 2s 检查一次 */
    if (++s_chg_tick >= 2) {
        s_chg_tick = 0;
        s_bat_charging_cached = bat_monitor_is_charging();
    }

    /* 电量百分比：每 10s 采一次 */
    if (s_bat_tick == 0) {
        int raw_pct = bat_monitor_get_percent();

        /* 诊断日志：用于校准分压比与满电电压（观察 10s 一次） */
        ESP_LOGI(TAG, "BAT volts=%.3fV raw_pct=%d charging=%d",
                 bat_monitor_get_volts(), raw_pct, (int)s_bat_charging_cached);

        /* 变化率限速：真实电池不可能在 10s 内跳几十个百分点。
         * 插拔充电器时端电压会阶跃，直接采信会导致百分比瞬跳，
         * 这里限制单次更新幅度，让显示平滑爬升/下降。 */
        if (s_bat_pct_cached == 0) {
            s_bat_pct_cached = raw_pct;              /* 首帧直接采信 */
        } else if (raw_pct > s_bat_pct_cached) {
            s_bat_pct_cached += (raw_pct - s_bat_pct_cached > 5) ? 5 : (raw_pct - s_bat_pct_cached);
        } else if (raw_pct < s_bat_pct_cached) {
            s_bat_pct_cached -= (s_bat_pct_cached - raw_pct > 5) ? 5 : (s_bat_pct_cached - raw_pct);
        }
    }
    if (++s_bat_tick >= 10) s_bat_tick = 0;

    /* 100% 时不显示充电图标（电压阈值与百分比更新频率不同步时的兜底） */
    bool show_charging = s_bat_charging_cached && s_bat_pct_cached < 100;

    const lv_img_dsc_t *bat_icon;
    if (show_charging) {
        bat_icon = &speaker_image_middle_quick_settings_battery_charge_20_20;
    } else if (s_bat_pct_cached >= 75) {
        bat_icon = &speaker_image_middle_quick_settings_battery_level4_20_20;
    } else if (s_bat_pct_cached >= 50) {
        bat_icon = &speaker_image_middle_quick_settings_battery_level3_20_20;
    } else if (s_bat_pct_cached >= 25) {
        bat_icon = &speaker_image_middle_quick_settings_battery_level2_20_20;
    } else {
        bat_icon = &speaker_image_middle_quick_settings_battery_level1_20_20;
    }
    lv_img_set_src(s_status_battery_icon, bat_icon);
    lv_label_set_text_fmt(s_status_battery_label, "%d%%", s_bat_pct_cached);

    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int internal_used = internal_total ? (int)((internal_total - internal_free) * 100 / internal_total) : 0;
    int psram_used = psram_total ? (int)((psram_total - psram_free) * 100 / psram_total) : 0;
    lv_bar_set_value(s_status_sram_bar, internal_used, LV_ANIM_OFF);
    lv_bar_set_value(s_status_psram_bar, psram_used, LV_ANIM_OFF);

    if (s_wlan_connected_main_label) {
        wifi_provision_status_t provision_status = wifi_provision_get_status();
        const char *target_ssid = wifi_provision_get_target_ssid();
        if (provision_status == WIFI_PROVISION_STATUS_CONNECTING && target_ssid && target_ssid[0]) {
            lv_label_set_text_fmt(s_wlan_connected_main_label, "%s", target_ssid);
            if (s_wlan_connected_minor_label) {
                lv_label_set_text(s_wlan_connected_minor_label, "Connecting...");
            }
            if (s_wlan_connected_group) {
                lv_obj_clear_flag(s_wlan_connected_group, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (connected) {
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
        } else if (provision_status == WIFI_PROVISION_STATUS_FAILED && target_ssid && target_ssid[0]) {
            lv_label_set_text_fmt(s_wlan_connected_main_label, "%s", target_ssid);
            if (s_wlan_connected_minor_label) {
                lv_label_set_text(s_wlan_connected_minor_label, "Failed");
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

/* ── 进 WLAN/配网页前：若正在播放，自动触发一次暂停（对齐参考项目）。
 * 不照搬参考版"动态建 task"（有 PSRAM 碎片风险），直接投 BTN_ACT_PLAY
 * 到常驻快队列，语义一致。 ── */
static void _pause_playback_if_playing(void)
{
    if (s_last_ui_state != 1 || !s_btn_queue_ctrl) return;
    btn_action_t act = BTN_ACT_PLAY;
    xQueueSend(s_btn_queue_ctrl, &act, 0);
}

static void _show_wlan(void)
{
    if (!s_wlan_scr) return;
    _pause_playback_if_playing();
    _show_status_panel(false);
    s_parent_screen = s_main_scr;
    s_return_target = s_main_scr;
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
    if (s_softap_info_label) {
        if (ap_ssid && ap_ssid[0] != '\0') {
            lv_label_set_text_fmt(s_softap_info_label, "%s", ap_ssid);
        } else {
            lv_label_set_text(s_softap_info_label, "Starting SoftAP...");
        }
    }
}

/* 点击加密网络 → 密码输入屏（圆屏键盘） */
static void _show_wlan_password(const char *ssid)
{
    if (!s_wlan_password_scr) _create_wlan_password_screen();
    if (!s_wlan_password_scr) return;

    snprintf(s_wlan_password_ssid, sizeof(s_wlan_password_ssid), "%s", ssid ? ssid : "");
    if (s_wlan_password_ssid_label) {
        lv_label_set_text_fmt(s_wlan_password_ssid_label, "%s", s_wlan_password_ssid);
    }
    if (s_wlan_password_ta) {
        lv_textarea_set_text(s_wlan_password_ta, "");
    }
    _show_status_panel(false);
    s_parent_screen = s_wlan_scr;
    s_return_target = s_wlan_scr;
    s_lyrics_visible = false;
    lv_scr_load(s_wlan_password_scr);
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

static void _update_standby_time(void)
{
    if (!s_standby_time_label) return;
    time_t now = time(NULL);
    struct tm local_time = {0};
    if (localtime_r(&now, &local_time) == NULL) {
        memset(&local_time, 0, sizeof(local_time));
    }
    lv_label_set_text_fmt(s_standby_time_label, "%02d:%02d", local_time.tm_hour, local_time.tm_min);
}

static void _standby_scr_click_cb(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Standby screen touched, waking up to previous screen");
    _show_parent_screen();
}

static void _create_standby_screen(void)
{
    if (s_standby_scr) return;

    s_standby_scr = lv_obj_create(NULL);
    lv_obj_clear_flag(s_standby_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_standby_scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_standby_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_standby_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_standby_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_standby_scr, _standby_scr_click_cb, LV_EVENT_CLICKED, NULL);

    s_standby_time_label = lv_label_create(s_standby_scr);
    lv_obj_clear_flag(s_standby_time_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(s_standby_time_label, TFT_W);
    lv_obj_set_style_text_font(s_standby_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_standby_time_label, C_WHITE, 0);
    lv_obj_set_style_text_align(s_standby_time_label, LV_TEXT_ALIGN_CENTER, 0);
    /* 放大 2 倍 (scale = 512, 100% = 256) */
    lv_obj_set_style_transform_scale(s_standby_time_label, 512, 0);
    lv_obj_set_style_transform_pivot_x(s_standby_time_label, TFT_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_standby_time_label, lv_pct(50), 0);
    lv_obj_align(s_standby_time_label, LV_ALIGN_CENTER, 0, 0);

    _update_standby_time();
}

static void _show_standby_screen(void)
{
    if (!s_standby_scr) {
        _create_standby_screen();
    }
    if (lv_scr_act() == s_standby_scr) {
        return;
    }

    _show_status_panel(false);
    _hide_capsule_popup();
    _show_return_bar(false);

    lv_obj_t *current = lv_scr_act();
    if (current && current != s_standby_scr) {
        s_parent_screen = current;
    } else {
        s_parent_screen = s_main_scr;
    }

    _update_standby_time();
    lv_scr_load(s_standby_scr);
    ESP_LOGI(TAG, "Entered standby clock screen");
}

static void _show_parent_screen(void)
{
    lv_obj_t *current = lv_scr_act();
    if (current == s_standby_scr) {
        lv_obj_t *target = s_parent_screen ? s_parent_screen : s_main_scr;
        s_parent_screen = NULL;
        lv_scr_load(target);
        return;
    }
    if (current == s_softap_scr) _show_wlan();
    else if (current == s_wlan_password_scr) _show_wlan();
    else if (current == s_wlan_scr || current == s_lyrics_scr) _show_main_screen();
    else if (s_parent_screen) lv_scr_load(s_parent_screen);
}

static void _system_button_event_cb(lv_event_t *event)
{
    if (!event) return;
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_current_target(event);
    if (code != LV_EVENT_CLICKED) return;
    if (target == s_status_wifi_btn) {
        _show_wlan();
    } else if (target == s_wlan_softap_cell) {
        _show_softap();
    } else if (target == s_wlan_back_btn || target == s_softap_back_btn ||
               target == s_softap_content_back_btn || target == s_wlan_password_back_btn) {
        _show_parent_screen();
    } else if (target == s_status_volume_btn) {
        _show_capsule_popup(CAPSULE_POPUP_VOLUME, s_volume_current);
    } else if (target == s_status_brightness_btn) {
        _show_capsule_popup(CAPSULE_POPUP_BRIGHTNESS, s_brightness_current);
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

    /* ── 待机时钟：非播放态连续无触摸 STANDBY_IDLE_MS 后进入 ── */
    if (pressed) {
        s_last_touch_tick = lv_tick_get();
    } else if (lv_scr_act() != s_standby_scr && !s_playing_state &&
               s_last_touch_tick != 0 &&
               (lv_tick_get() - s_last_touch_tick) > STANDBY_IDLE_MS) {
        _show_standby_screen();
    }
    if (pressed && lv_scr_act() == s_standby_scr) {
        /* 待机屏任意触摸唤醒（LVGL CLICKED 可能因手势滑动不触发） */
        _show_parent_screen();
    }

    if (pressed && !s_gesture.active) {
        s_gesture.start_x = x;
        s_gesture.start_y = y;
        s_gesture.last_x = x;
        s_gesture.last_y = y;
        s_gesture.start_tick = lv_tick_get();
        s_gesture.mode = GESTURE_MODE_NONE;
        /* 配网/密码界面本身要上下滚动选网络，顶部下拉手势会误触发状态栏 */
        if (!s_status_visible && y < GESTURE_EDGE_PX &&
            lv_scr_act() != s_wlan_scr &&
            lv_scr_act() != s_softap_scr &&
            lv_scr_act() != s_wlan_password_scr &&
            lv_scr_act() != s_standby_scr) {
            s_gesture.mode = GESTURE_MODE_STATUS_OPEN;
            s_gesture.active = true;
            lv_obj_clear_flag(s_status_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_status_panel);
            lv_obj_set_y(s_status_panel, y - (STATUS_PANEL_H - 20));
        } else if (s_status_visible && y >= TFT_H - GESTURE_EDGE_PX) {
            s_gesture.mode = GESTURE_MODE_STATUS_CLOSE;
            s_gesture.active = true;
        } else if (!s_status_visible && lv_scr_act() != s_main_scr &&
                   lv_scr_act() != s_standby_scr &&
                   y >= TFT_H - GESTURE_EDGE_PX) {
            s_gesture.mode = GESTURE_MODE_RETURN;
            s_gesture.active = true;
            _show_return_bar(true);
        } else {
            /* 触摸在非手势区域（如按钮），不拦截，让 LVGL 处理点击 */
            return;
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
    _maybe_return_after_provision();

    /* 息屏时钟页面显示时，保持每秒更新时间 */
    if (lv_scr_act() == s_standby_scr) {
        _update_standby_time();
    } else if (lv_scr_act() != s_standby_scr && !s_playing_state &&
               s_last_touch_tick != 0 &&
               (lv_tick_get() - s_last_touch_tick) > STANDBY_IDLE_MS) {
        /* 非播放态且连续无触摸/遥控操作 STANDBY_IDLE_MS 后进入待机。
         * 统一用 s_last_touch_tick（触摸 + set_state + 遥控都会刷新它），
         * 不用 lv_display_get_inactive_time（只靠 LVGL 输入重置，遥控器操作
         * 不产生触摸事件会导致误判进入待机）。 */
        ESP_LOGI(TAG, "Inactive time reached %u ms, entering standby clock",
                 (unsigned)STANDBY_IDLE_MS);
        _show_standby_screen();
    }
}

/* 配网成功后自动返回 WLAN 页（从软AP页/密码页），只触发一次 */
static void _maybe_return_after_provision(void)
{
    if (wifi_provision_get_status() != WIFI_PROVISION_STATUS_SUCCESS) {
        s_provision_return_done = false;
        return;
    }
    if (s_provision_return_done) {
        return;
    }

    lv_obj_t *current = lv_scr_act();
    if (current != s_softap_scr && current != s_wlan_scr &&
        current != s_wlan_password_scr) {
        return;
    }

    s_provision_return_done = true;
    if (current == s_wlan_scr) {
        return;
    }
    ESP_LOGI(TAG, "Provisioning succeeded, returning to WLAN screen");
    _show_wlan();
}

void lvgl_port_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    s_wlan_ssid_font = esp_brookesia_font_maison_neue_book_22;
    s_wlan_ssid_font.fallback = &s_cjk_font;
    s_status_wifi_caption_font = esp_brookesia_font_maison_neue_book_16;
    s_status_wifi_caption_font.fallback = &s_cjk_font;

    /* CJK 字体 fallback 链：CJK(思源) → 补丁字体(思源) → IPA(DejaVu) → Montserrat 12
     * IPA 层用 DejaVuSans 借字形，兜底思源黑体本尊不含的重音/IPA/翻转E 等
     *（如泽野曲名 R∃/MEMBER 的 Ǝ）。这些字思源 cmap 里根本没有，无法用思源补。 */
    s_cjk_font = lv_font_simsun_16_cjk;
    s_supplement_font = lv_font_simsun_16_supplement;
    s_ipa_font = lv_font_simsun_16_ipa;
    s_ipa_font.fallback = &lv_font_montserrat_12;
    s_supplement_font.fallback = &s_ipa_font;
    s_cjk_font.fallback = &s_supplement_font;

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
    lv_obj_set_style_text_font(s_label_title, &s_cjk_font, 0);
    lv_label_set_long_mode(s_label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_label_title, LV_TEXT_ALIGN_CENTER, 0);

    /* ====== 歌手 ====== */
    s_label_artist = lv_label_create(scr);
    lv_obj_set_pos(s_label_artist, UI_SAFE_X, 242);
    lv_obj_set_width(s_label_artist, UI_SAFE_W);
    lv_label_set_text(s_label_artist, "Waiting...");
    lv_obj_set_style_text_color(s_label_artist, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_label_artist, &s_cjk_font, 0);
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

    /* ====== 控制按钮 ======
     * 侧边按钮用透明热区容器实现非对称扩展：外侧多扩（手指容易偏到屏幕外），
     * 内侧少扩（避免侵入 Play 区域导致误触切歌）。
     * lv_obj_set_ext_click_area 只能四周对称扩展，做不到这一点。 */
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

    /* 非对称外扩热区：prev 向左、next 向右各加 UI_BUTTON_EXTRA_OUTWARD px。
     * 事件转发到对应按钮，自身完全透明不可见。 */
    _create_outward_hotzone(scr, s_btn_prev, BTN_ACT_PREV, -1);
    _create_outward_hotzone(scr, s_btn_next, BTN_ACT_NEXT, +1);

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

    /* 按下即触发（PRESSED）：不受手指微动影响，响应最快 */
    lv_obj_add_event_cb(s_btn_prev, _btn_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn_play, _btn_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_btn_next, _btn_press_cb, LV_EVENT_PRESSED, NULL);
    _btn_worker_start();

    /* 点击封面图 → 切到歌词界面 */
    lv_obj_add_event_cb(s_cover_container, _cover_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_opa(s_cover_container, LV_OPA_70, LV_STATE_PRESSED);
    static lv_style_transition_dsc_t cover_press_tr;
    static lv_style_prop_t cover_press_props[] = { LV_STYLE_OPA, (lv_style_prop_t)0 };
    lv_style_transition_dsc_init(&cover_press_tr, cover_press_props, lv_anim_path_ease_out, 120, 0, NULL);
    lv_obj_set_style_transition(s_cover_container, &cover_press_tr, LV_STATE_DEFAULT);

    lvgl_port_ui_lyrics_create();
    s_main_scr = scr;  /* 保存主屏幕引用 */
    _create_wlan_screen();
    _create_softap_screen();
    _create_wlan_password_screen();
    _create_standby_screen();
    _create_system_ui();
    s_last_touch_tick = lv_tick_get();   /* 待机空闲计时起点 */
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

    /* ====== 顶部歌曲名与歌手信息 ====== */
    s_lyrics_label_title = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_label_title, UI_CENTER_X - 110, 26);
    lv_obj_set_width(s_lyrics_label_title, 220);
    lv_obj_set_style_pad_all(s_lyrics_label_title, 0, 0);
    const char *init_title = s_label_title ? lv_label_get_text(s_label_title) : "DLNA Player";
    lv_label_set_text(s_lyrics_label_title, init_title ? init_title : "DLNA Player");
    lv_obj_set_style_text_color(s_lyrics_label_title, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_lyrics_label_title, &s_cjk_font, 0);
    lv_label_set_long_mode(s_lyrics_label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_lyrics_label_title, LV_TEXT_ALIGN_CENTER, 0);

    s_lyrics_label_artist = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_label_artist, UI_CENTER_X - 110, 48);
    lv_obj_set_width(s_lyrics_label_artist, 220);
    lv_obj_set_style_pad_all(s_lyrics_label_artist, 0, 0);
    const char *init_artist = s_label_artist ? lv_label_get_text(s_label_artist) : "";
    lv_label_set_text(s_lyrics_label_artist, init_artist ? init_artist : "");
    lv_obj_set_style_text_color(s_lyrics_label_artist, lv_color_hex(0x758B9C), 0);
    lv_obj_set_style_text_font(s_lyrics_label_artist, &s_cjk_font, 0);
    lv_label_set_long_mode(s_lyrics_label_artist, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_lyrics_label_artist, LV_TEXT_ALIGN_CENTER, 0);

    /* ====== 暂无歌词占位（居中） ====== */
    s_lyrics_placeholder = lv_label_create(s_lyrics_scr);
    lv_obj_set_width(s_lyrics_placeholder, UI_SAFE_W);
    lv_obj_set_pos(s_lyrics_placeholder, UI_SAFE_X, 152);
    lv_label_set_text(s_lyrics_placeholder, "暂无歌词");
    lv_obj_set_style_text_font(s_lyrics_placeholder, &s_cjk_font, 0);
    lv_obj_set_style_text_color(s_lyrics_placeholder, C_DIM, 0);
    lv_obj_set_style_text_align(s_lyrics_placeholder, LV_TEXT_ALIGN_CENTER, 0);

    /* ====== 歌词 3 行显示 ====== */
    /* 上一句（圆屏上半部，淡雅半透） */
    s_lyrics_prev = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_prev, UI_SAFE_X, 106);
    lv_obj_set_width(s_lyrics_prev, UI_SAFE_W);
    lv_label_set_text(s_lyrics_prev, "");
    lv_obj_set_style_text_font(s_lyrics_prev, &s_cjk_font, 0);
    lv_obj_set_style_text_color(s_lyrics_prev, lv_color_hex(0x657888), 0);
    lv_obj_set_style_opa(s_lyrics_prev, LV_OPA_70, 0);
    lv_obj_set_style_text_align(s_lyrics_prev, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyrics_prev, LV_LABEL_LONG_CLIP);

    /* 当前句（明亮焦点，支持卡拉OK逐字高亮与平滑推移） */
    s_lyrics_curr = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_curr, UI_SAFE_X, 152);
    lv_obj_set_width(s_lyrics_curr, UI_SAFE_W);
    lv_label_set_text(s_lyrics_curr, "");
    lv_obj_set_style_text_font(s_lyrics_curr, &s_cjk_font, 0);
    lv_obj_set_style_text_color(s_lyrics_curr, lv_color_hex(0xC6D6E5), 0);
    lv_obj_set_style_text_color(s_lyrics_curr, C_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_lyrics_curr, LV_OPA_0, LV_PART_SELECTED);   /* 透明背景，杜绝原生反色块 */
    lv_obj_set_style_text_align(s_lyrics_curr, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyrics_curr, LV_LABEL_LONG_CLIP);

    /* 下一句（圆屏下半部，淡雅半透） */
    s_lyrics_next = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_next, UI_SAFE_X, 198);
    lv_obj_set_width(s_lyrics_next, UI_SAFE_W);
    lv_label_set_text(s_lyrics_next, "");
    lv_obj_set_style_text_font(s_lyrics_next, &s_cjk_font, 0);
    lv_obj_set_style_text_color(s_lyrics_next, lv_color_hex(0x657888), 0);
    lv_obj_set_style_opa(s_lyrics_next, LV_OPA_60, 0);
    lv_obj_set_style_text_align(s_lyrics_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyrics_next, LV_LABEL_LONG_CLIP);

    /* ====== 底部进度条 + 播放时间 ====== */
    s_lyrics_bar_progress = lv_bar_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_bar_progress, 60, 268);
    lv_obj_set_size(s_lyrics_bar_progress, 240, 4);
    lv_bar_set_range(s_lyrics_bar_progress, 0, 1000);
    lv_bar_set_value(s_lyrics_bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_lyrics_bar_progress, C_BTN_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_lyrics_bar_progress, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_lyrics_bar_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_lyrics_bar_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    s_lyrics_label_time1 = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_label_time1, 72, 274);
    lv_obj_set_size(s_lyrics_label_time1, 80, 20);
    lv_label_set_text(s_lyrics_label_time1, "0:00");
    lv_obj_set_style_text_color(s_lyrics_label_time1, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_lyrics_label_time1, &lv_font_montserrat_12, 0);

    s_lyrics_label_time2 = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_label_time2, 208, 274);
    lv_obj_set_size(s_lyrics_label_time2, 80, 20);
    lv_label_set_text(s_lyrics_label_time2, "0:00");
    lv_obj_set_style_text_color(s_lyrics_label_time2, C_WHITE80, 0);
    lv_obj_set_style_text_font(s_lyrics_label_time2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(s_lyrics_label_time2, LV_TEXT_ALIGN_RIGHT, 0);

    /* ====== 底部返回提示 ====== */
    s_lyrics_hint_back = lv_label_create(s_lyrics_scr);
    lv_obj_set_pos(s_lyrics_hint_back, UI_CENTER_X - 60, 304);
    lv_obj_set_size(s_lyrics_hint_back, 120, 18);
    lv_label_set_text(s_lyrics_hint_back, "‹ 点击返回");
    lv_obj_set_style_text_color(s_lyrics_hint_back, lv_color_hex(0x4A6070), 0);
    lv_obj_set_style_text_font(s_lyrics_hint_back, &s_cjk_font, 0);
    lv_obj_set_style_text_align(s_lyrics_hint_back, LV_TEXT_ALIGN_CENTER, 0);

    ESP_LOGI(TAG, "Lyrics screen created (optimized layout)");
}

void lvgl_port_ui_toggle_lyrics(void)
{
    s_lyrics_visible = !s_lyrics_visible;
    if (s_lyrics_visible) {
        _show_status_panel(false);
        _show_return_bar(false);
        s_parent_screen = s_main_scr;
        s_return_target = s_main_scr;
        lv_scr_load(s_lyrics_scr);
    } else {
        _show_main_screen();
    }
}

bool lvgl_port_ui_lyrics_is_visible(void)
{
    return s_lyrics_visible;
}

/* ====== UI 更新 ====== */
void lvgl_port_ui_set_title(const char *title) {
    const char *text = title ? title : "DLNA Player";
    lv_label_set_text(s_label_title, text);
    if (s_lyrics_label_title) {
        lv_label_set_text(s_lyrics_label_title, text);
    }
}
void lvgl_port_ui_set_artist(const char *artist) {
    const char *text = artist && artist[0] ? artist : "Unknown";
    lv_label_set_text(s_label_artist, text);
    if (s_lyrics_label_artist) {
        lv_label_set_text(s_lyrics_label_artist, text);
    }
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
        if (s_lyrics_bar_progress) {
            lv_bar_set_value(s_lyrics_bar_progress, permille, LV_ANIM_ON);
            lv_label_set_text_fmt(s_lyrics_label_time1, "%d:%02d", pmin, psec);
            lv_label_set_text_fmt(s_lyrics_label_time2, "%d:%02d", dmin, dsec);
        }
    } else {
        lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_label_time1, "%d:%02d", pmin, psec);
        lv_label_set_text(s_label_time2, "0:00");
        if (s_lyrics_bar_progress) {
            lv_bar_set_value(s_lyrics_bar_progress, 0, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_lyrics_label_time1, "%d:%02d", pmin, psec);
            lv_label_set_text(s_lyrics_label_time2, "0:00");
        }
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

/* s_last_ui_state 声明已上移到 s_btn_track_seq 附近 */

void lvgl_port_ui_set_state(int state) {
    if (state == s_last_ui_state) return;  /* 状态没变，跳过 */
    s_last_ui_state = state;
    /* 播放/暂停时不计入待机空闲（暂停算"在用"，仅停止才息屏） */
    s_playing_state = (state != 0);
    s_last_touch_tick = lv_tick_get();

    /* 如果开始播放，退出息屏时钟页面 */
    if (state == 1) {
        if (lv_scr_act() == s_standby_scr) {
            ESP_LOGI(TAG, "Music started playing, waking up from standby screen");
            _show_parent_screen();
        }
    }

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
void lvgl_port_ui_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    bool changed = (vol != s_volume_current);
    s_volume_current = vol;
    /* 音量变化防抖落 NVS（500ms），避免拖动滑块时反复擦写 flash */
    if (changed) {
        _volume_debounced_save(vol);
    }
    /* 胶囊弹窗正在显示音量时同步滑块 */
    if (s_capsule_visible && s_capsule_type == CAPSULE_POPUP_VOLUME && s_capsule_slider) {
        lv_slider_set_value(s_capsule_slider, vol, LV_ANIM_OFF);
        if (s_capsule_label) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", vol);
            lv_label_set_text(s_capsule_label, buf);
        }
    }
}

void lvgl_port_ui_show_volume_popup(int vol)
{
    lvgl_port_lock();
    /* 屏保界面不弹音量胶囊，避免打扰待机时钟 */
    if (lv_scr_act() != s_standby_scr) {
        _show_capsule_popup(CAPSULE_POPUP_VOLUME, vol);
    }
    lvgl_port_unlock();
}

/* 进入休眠屏保（供 airkan 遥控关机键等调用）。持锁线程安全。 */
void lvgl_port_ui_enter_standby(void)
{
    lvgl_port_lock();
    _show_standby_screen();
    lvgl_port_unlock();
}

/* 退出休眠屏保，回到之前的界面（供关机键再按唤醒）。若不在屏保则无操作。 */
void lvgl_port_ui_exit_standby(void)
{
    lvgl_port_lock();
    if (lv_scr_act() == s_standby_scr) {
        _show_parent_screen();
    }
    lvgl_port_unlock();
}

/* 当前是否处于休眠屏保界面 */
bool lvgl_port_ui_is_standby(void)
{
    return lv_scr_act() == s_standby_scr;
}

/* 重置待机空闲计时（供 airkan 遥控按键调用）。
 * 遥控操作不产生触摸事件，但应算作"用户在用"，避免误入待机屏。 */
void lvgl_port_ui_reset_idle(void)
{
    lvgl_port_lock();
    s_last_touch_tick = lv_tick_get();
    lvgl_port_unlock();
}

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

static void _y_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, (int32_t)v);
}

void lvgl_port_ui_lyrics_update(int current_idx, const char *prev, const char *curr, const char *next)
{
    /* 检测行号切换，触发向上微浮与淡入平滑动画 */
    if (current_idx != s_lyrics_prev_line && s_lyrics_prev_line >= 0) {
        /* 上一句：从正常可见柔和过渡到半透（始终保持可见） */
        lv_anim_del(s_lyrics_prev, _opa_anim_cb);
        lv_anim_t ap;
        lv_anim_init(&ap);
        lv_anim_set_var(&ap, s_lyrics_prev);
        lv_anim_set_exec_cb(&ap, _opa_anim_cb);
        lv_anim_set_values(&ap, LV_OPA_COVER, LV_OPA_70);
        lv_anim_set_time(&ap, 200);
        lv_anim_set_path_cb(&ap, lv_anim_path_ease_out);
        lv_anim_start(&ap);

        /* 当前句：从微下方(Y=158)平滑上浮至焦点位(Y=152)，并由 30% 淡入至 100% */
        lv_anim_del(s_lyrics_curr, _opa_anim_cb);
        lv_anim_del(s_lyrics_curr, _y_anim_cb);

        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, s_lyrics_curr);
        lv_anim_set_exec_cb(&ay, _y_anim_cb);
        lv_anim_set_values(&ay, 158, 152);
        lv_anim_set_time(&ay, 220);
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
        lv_anim_start(&ay);

        lv_anim_t ao;
        lv_anim_init(&ao);
        lv_anim_set_var(&ao, s_lyrics_curr);
        lv_anim_set_exec_cb(&ao, _opa_anim_cb);
        lv_anim_set_values(&ao, LV_OPA_30, LV_OPA_COVER);
        lv_anim_set_time(&ao, 220);
        lv_anim_set_path_cb(&ao, lv_anim_path_ease_out);
        lv_anim_start(&ao);

        /* 下一句：微淡入至 60% 透明度 */
        lv_anim_del(s_lyrics_next, _opa_anim_cb);
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, s_lyrics_next);
        lv_anim_set_exec_cb(&an, _opa_anim_cb);
        lv_anim_set_values(&an, LV_OPA_20, LV_OPA_60);
        lv_anim_set_time(&an, 200);
        lv_anim_set_path_cb(&an, lv_anim_path_ease_out);
        lv_anim_start(&an);
    } else {
        lv_obj_set_style_opa(s_lyrics_prev, LV_OPA_70, 0);
        lv_obj_set_style_opa(s_lyrics_curr, LV_OPA_COVER, 0);
        lv_obj_set_y(s_lyrics_curr, 152);
        lv_obj_set_style_opa(s_lyrics_next, LV_OPA_60, 0);
    }

    s_lyrics_current = current_idx;
    s_lyrics_prev_line = current_idx;

    lv_label_set_text(s_lyrics_prev, prev ? prev : "");
    lv_label_set_text(s_lyrics_curr, curr ? curr : "");
    lv_label_set_text(s_lyrics_next, next ? next : "");

    /* 重置高亮及位置 */
    lv_label_set_text_selection_start(s_lyrics_curr, 0);
    lv_label_set_text_selection_end(s_lyrics_curr, 0);
    ((lv_label_t *)s_lyrics_curr)->offset.x = 0;

    /* 有歌词→隐藏占位 */
    if (curr && curr[0]) {
        if (s_lyrics_placeholder) lv_obj_add_flag(s_lyrics_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── 逐字高亮（karaoke）+ 平滑视口推移 ── */
static int s_scroll_total = 0;   /* 总超宽像素（正数） */
static int s_text_bytes = 0;     /* 总字节数 */

static int _utf8_bytes_to_char_count(const char *s, int byte_limit)
{
    if (!s || byte_limit <= 0) return 0;
    int chars = 0;
    for (int i = 0; s[i] && i < byte_limit; i++) {
        if ((s[i] & 0xC0) != 0x80) chars++;
    }
    return chars;
}

void lvgl_port_ui_lyrics_karaoke(int byte_idx)
{
    const char *text = lv_label_get_text(s_lyrics_curr);
    if (!text || !text[0]) return;

    if (byte_idx <= 0) {
        lv_label_set_text_selection_start(s_lyrics_curr, 0);
        lv_label_set_text_selection_end(s_lyrics_curr, 0);
        if (s_scroll_total > 0) {
            ((lv_label_t *)s_lyrics_curr)->offset.x = s_scroll_total / 2;
        } else {
            ((lv_label_t *)s_lyrics_curr)->offset.x = 0;
        }
        return;
    }

    /* 字符索引（UTF-8 Safe）：LVGL 文本选中要求字符索引而非字节索引 */
    int char_idx = _utf8_bytes_to_char_count(text, byte_idx);
    lv_label_set_text_selection_start(s_lyrics_curr, 0);
    lv_label_set_text_selection_end(s_lyrics_curr, char_idx);

    /* 超长歌词沿水平方向随演唱进度平滑平移 */
    if (s_scroll_total > 0) {
        int total_bytes = strlen(text);
        if (total_bytes > 0) {
            int progress_permille = byte_idx * 1000 / total_bytes;
            if (progress_permille > 1000) progress_permille = 1000;
            /* 居中对齐下：从 +s_scroll_total/2 (左对齐开头) 平滑移至 -s_scroll_total/2 (右对齐结尾) */
            int offset = (s_scroll_total / 2) - (s_scroll_total * progress_permille / 1000);
            ((lv_label_t *)s_lyrics_curr)->offset.x = offset;
        }
    }
}

void lvgl_port_ui_lyrics_set_scroll_dist(int dist)
{
    s_scroll_total = dist;
}

void lvgl_port_ui_lyrics_scroll_to_end(int line_duration_ms)
{
    (void)line_duration_ms;
    const char *text = lv_label_get_text(s_lyrics_curr);
    if (!text || !text[0]) {
        s_scroll_total = 0;
        s_text_bytes = 0;
        return;
    }

    /* 用 lv_txt_get_width 取实际像素宽度 */
    const lv_font_t *font = lv_obj_get_style_text_font(s_lyrics_curr, 0);
    int16_t text_w = lv_txt_get_width(text, strlen(text), font, 0);
    int label_w = lv_obj_get_width(s_lyrics_curr);
    if (text_w <= label_w) {
        s_scroll_total = 0;
        s_text_bytes = 0;
        ((lv_label_t *)s_lyrics_curr)->offset.x = 0;
        return;
    }

    s_scroll_total = (int)text_w - label_w;
    s_text_bytes = strlen(text);
    /* 初始将文字开头对齐在屏幕安全区左侧 */
    ((lv_label_t *)s_lyrics_curr)->offset.x = s_scroll_total / 2;
    ESP_LOGI(TAG, "scroll_dist: total=%d bytes=%d (text_w=%d label_w=%d)", s_scroll_total, s_text_bytes, text_w, label_w);
}

void lvgl_port_ui_lyrics_set_scroll_progress(int pct)
{
    if (s_scroll_total <= 0) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int x = (s_scroll_total / 2) - (s_scroll_total * pct / 100);
    ((lv_label_t *)s_lyrics_curr)->offset.x = x;
    lv_obj_invalidate(s_lyrics_curr);
}

void lvgl_port_ui_lyrics_tick_scroll(void)
{
    /* 由 lvgl_port_ui_lyrics_karaoke 随演唱进度平滑推进 */
}

void lvgl_port_ui_lyrics_clear(void)
{
    s_lyrics_current = -1;
    s_lyrics_prev_line = -1;
    if (s_lyrics_prev) {
        lv_label_set_text(s_lyrics_prev, "");
        lv_obj_set_style_opa(s_lyrics_prev, LV_OPA_70, 0);
    }
    if (s_lyrics_curr) {
        lv_anim_del(s_lyrics_curr, _opa_anim_cb);
        lv_anim_del(s_lyrics_curr, _y_anim_cb);
        lv_label_set_text(s_lyrics_curr, "");
        lv_obj_set_style_opa(s_lyrics_curr, LV_OPA_COVER, 0);
        lv_obj_set_y(s_lyrics_curr, 152);
        lv_label_set_text_selection_start(s_lyrics_curr, 0);
        lv_label_set_text_selection_end(s_lyrics_curr, 0);
        ((lv_label_t *)s_lyrics_curr)->offset.x = 0;
    }
    if (s_lyrics_next) {
        lv_label_set_text(s_lyrics_next, "");
        lv_obj_set_style_opa(s_lyrics_next, LV_OPA_60, 0);
    }
    s_scroll_total = 0;
    s_text_bytes = 0;
    if (s_lyrics_placeholder) lv_obj_clear_flag(s_lyrics_placeholder, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Lyrics UI cleared");
}
/* ── 双线性预缩放到 UI_COVER_SIZE（严格重采样，保留备用） ── */
static void _scale_rgb565_to_cover_buf(const uint16_t *src, int src_w, int src_h,
                                       uint16_t *dst, int dst_w, int dst_h)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    if (src_w == dst_w && src_h == dst_h) {
        memcpy(dst, src, (size_t)dst_w * dst_h * sizeof(uint16_t));
        return;
    }
    for (int y = 0; y < dst_h; y++) {
        float fy = (float)y / (float)dst_h * (float)src_h;
        int iy = (int)fy;
        if (iy >= src_h - 1) iy = src_h - 2;
        if (iy < 0) iy = 0;
        float dy = fy - (float)iy;

        for (int x = 0; x < dst_w; x++) {
            float fx = (float)x / (float)dst_w * (float)src_w;
            int ix = (int)fx;
            if (ix >= src_w - 1) ix = src_w - 2;
            if (ix < 0) ix = 0;
            float dx = fx - (float)ix;

            uint16_t p00 = src[iy * src_w + ix];
            uint16_t p01 = src[iy * src_w + ix + 1];
            uint16_t p10 = src[(iy + 1) * src_w + ix];
            uint16_t p11 = src[(iy + 1) * src_w + ix + 1];

            int r = (int)((((p00 >> 11) & 0x1F) * (1.0f - dx) + ((p01 >> 11) & 0x1F) * dx) * (1.0f - dy) +
                          (((p10 >> 11) & 0x1F) * (1.0f - dx) + ((p11 >> 11) & 0x1F) * dx) * dy);
            int g = (int)((((p00 >> 5) & 0x3F) * (1.0f - dx) + ((p01 >> 5) & 0x3F) * dx) * (1.0f - dy) +
                          (((p10 >> 5) & 0x3F) * (1.0f - dx) + ((p11 >> 5) & 0x3F) * dx) * dy);
            int b = (int)(((p00 & 0x1F) * (1.0f - dx) + (p01 & 0x1F) * dx) * (1.0f - dy) +
                          ((p10 & 0x1F) * (1.0f - dx) + (p11 & 0x1F) * dx) * dy);

            dst[y * dst_w + x] = (uint16_t)(((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F));
        }
    }
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

/* ════════════════════════════════════════════════
 *  胶囊弹窗公开 API
 * ════════════════════════════════════════════════ */

int lvgl_port_ui_get_volume(void)
{
    return s_volume_current;
}

int lvgl_port_ui_get_brightness(void)
{
    return (int)s_brightness_current;
}

void lvgl_port_ui_set_brightness(int brightness)
{
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    s_brightness_current = (uint8_t)brightness;
    tft_set_backlight(s_brightness_current);
    if (s_capsule_visible && s_capsule_type == CAPSULE_POPUP_BRIGHTNESS && s_capsule_slider) {
        lv_slider_set_value(s_capsule_slider, brightness, LV_ANIM_OFF);
        if (s_capsule_label) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", brightness);
            lv_label_set_text(s_capsule_label, buf);
        }
    }
}

void lvgl_port_ui_register_volume_cb(void (*cb)(int))
{
    s_capsule_volume_cb = cb;
}

void lvgl_port_ui_register_brightness_cb(void (*cb)(int))
{
    s_capsule_brightness_cb = cb;
}

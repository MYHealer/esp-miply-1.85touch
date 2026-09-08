/*
 * LVGL 端口桥接 — ESP32-S3 + ST7735S + 触控
 *
 * 提供：
 *   lvgl_port_init()         — 初始化 LVGL + 显示 + 触控
 *   lvgl_port_lock()/unlock() — 线程安全访问
 *   lvgl_port_ui_*()          — UI 控件更新
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

struct lv_display_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL + 显示驱动 + 触控输入
 * @param task_priority  LVGL 定时器处理任务优先级
 * @return ESP_OK 或错误码
 */
esp_err_t lvgl_port_init(int task_priority);

/**
 * @brief Register the CST816S touchscreen as an LVGL pointer device.
 *
 * The board I2C bus is initialized by tft_init(). A missing touch controller
 * is reported to the log but does not prevent the display from starting.
 */
esp_err_t lvgl_port_touch_init(struct lv_display_t *disp);

/**
 * @brief Read the latest polled touch state for the gesture detector.
 */
bool lvgl_port_touch_get_snapshot(bool *pressed, int *x, int *y);

/**
 * @brief LVGL 互斥锁（display_task 中调用 lvgl 前必须 lock）
 */
void lvgl_port_lock(void);
void lvgl_port_unlock(void);

/* ══════════════════════════════════════════════
 *  UI 创建 + 更新接口
 * ══════════════════════════════════════════════ */

/* Decode covers to the 148px disc size and display them 1:1. */
#define LVGL_PORT_COVER_SRC_SIZE 148

void lvgl_port_ui_create(void);
void lvgl_port_ui_set_title(const char *title);
void lvgl_port_ui_set_artist(const char *artist);
void lvgl_port_ui_set_progress(int position_sec, int duration_sec);
void lvgl_port_ui_set_state(int state);  /* 0=停止 1=播放 2=暂停 */
void lvgl_port_ui_set_volume(int vol);
void lvgl_port_ui_lyrics_update(int current_idx, const char *prev, const char *curr, const char *next);
void lvgl_port_ui_lyrics_karaoke(int byte_idx);
void lvgl_port_ui_lyrics_scroll_to_end(int line_duration_ms);
void lvgl_port_ui_lyrics_tick_scroll(void);
void lvgl_port_ui_set_cover(const uint16_t *pixels, int w, int h);
void lvgl_port_ui_clear_cover(void);
void lvgl_port_ui_lyrics_create(void);
void lvgl_port_ui_toggle_lyrics(void);
bool lvgl_port_ui_lyrics_is_visible(void);
void lvgl_port_ui_lyrics_clear(void);

/**
 * @brief 小米音箱模式：显示/隐藏接管画面
 * @param active true=显示"音频已由手机接管", false=恢复正常UI
 */
void lvgl_port_ui_set_speaker_mode(bool active);

/* ══════════════════════════════════════════════
 *  播放控制按钮回调注册
 * ══════════════════════════════════════════════ */
typedef void (*lvgl_btn_cb_t)(void);
void lvgl_port_ui_register_btn_prev_cb(lvgl_btn_cb_t cb);
void lvgl_port_ui_register_btn_play_cb(lvgl_btn_cb_t cb);
void lvgl_port_ui_register_btn_next_cb(lvgl_btn_cb_t cb);

/**
 * @brief 切歌键按下序号（每次 prev/next 入队自增）
 *
 * 切歌等待循环用它在"用户又按了"时立刻放弃旧等待，让新命令马上执行，
 * 避免切歌后数秒内按键无响应。
 */
uint32_t lvgl_port_ui_track_seq(void);

/* ══════════════════════════════════════════════
 *  胶囊弹窗（音量/亮度）
 * ══════════════════════════════════════════════ */
int  lvgl_port_ui_get_volume(void);
int  lvgl_port_ui_get_brightness(void);
void lvgl_port_ui_set_brightness(int brightness);
void lvgl_port_ui_register_volume_cb(void (*cb)(int));
void lvgl_port_ui_register_brightness_cb(void (*cb)(int));

#ifdef __cplusplus
}
#endif

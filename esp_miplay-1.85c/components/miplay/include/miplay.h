#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 MiPlay mDNS 广播 + TCP 8899 监听
 *
 * 注册 _lyra-mdns._udp 和 _mi-connect._udp mDNS 服务，
 * 让小米妙播手机端发现 ESP32 设备。
 */
esp_err_t miplay_init(void);

/**
 * @brief 停止 MiPlay 服务
 */
void miplay_stop(void);

/**
 * @brief MiPlay 连接状态回调
 * connected=true: 手机已连上 MiPlay，DLNA 应暂停
 * connected=false: MiPlay 断开，DLNA 可恢复
 */
typedef void (*miplay_connected_cb_t)(bool connected);
void miplay_set_connected_cb(miplay_connected_cb_t cb);

/* CMD_NOTIFY / SetMediaInfo 中的媒体字段。changed 只表示本次事件实际携带的字段。 */
#define MIPLAY_MEDIA_CHANGED_ID             (1U << 0)
#define MIPLAY_MEDIA_CHANGED_AUDIO_ID      (1U << 1)
#define MIPLAY_MEDIA_CHANGED_TITLE         (1U << 2)
#define MIPLAY_MEDIA_CHANGED_ARTIST        (1U << 3)
#define MIPLAY_MEDIA_CHANGED_ALBUM         (1U << 4)
#define MIPLAY_MEDIA_CHANGED_COVER_URL     (1U << 5)
#define MIPLAY_MEDIA_CHANGED_DURATION      (1U << 6)
#define MIPLAY_MEDIA_CHANGED_POSITION      (1U << 7)
#define MIPLAY_MEDIA_CHANGED_STATUS        (1U << 8)
#define MIPLAY_MEDIA_CHANGED_DEVICE_STATE  (1U << 9)
#define MIPLAY_MEDIA_CHANGED_PLAYER_STATE  (1U << 10)
#define MIPLAY_COVER_SOURCE_MAX             49152

#define MIPLAY_PLAYER_STATE_UNKNOWN  (-1)
#define MIPLAY_PLAYER_STATE_STOPPED   0
#define MIPLAY_PLAYER_STATE_PLAYING   1
#define MIPLAY_PLAYER_STATE_PAUSED    2

typedef struct {
    uint32_t changed;
    char id[64];
    char audio_id[64];
    char title[128];
    char artist[128];
    char album[128];
    char cover_url[MIPLAY_COVER_SOURCE_MAX];
    int64_t duration_ms;
    int64_t position_ms;
    int status;
    int device_state;
    int player_state;
} miplay_media_event_t;

typedef void (*miplay_media_cb_t)(const miplay_media_event_t *event);
void miplay_set_media_cb(miplay_media_cb_t cb);
bool miplay_is_connected(void);

/**
 * @brief 获取当前 MiPlay 音量百分比 (0-100)
 */
uint32_t miplay_get_volume(void);

/**
 * @brief 向手机发送反向控制通知
 * @param action "pause", "play", "next", "prev", "seek"
 * @param value  seek 时为 positionMs，其他为 0
 */
void miplay_send_receiver_control(const char *action, int64_t value);

#ifdef __cplusplus
}
#endif

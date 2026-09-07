/*
 * chime — 合成提示音（无 flash 资源占用）
 *
 * 小米充电提示音特征：两个上行音，约 E5(659Hz) → B5(988Hz)，
 * 各 ~110ms，短促清亮。用正弦合成 + attack/decay 包络，
 * 避免电平突变产生"啪"的爆音。
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "chime.h"
#include "board.h"

static const char *TAG = "CHIME";

/* 音符频率 (Hz) */
#define NOTE_E5   659.25f
#define NOTE_B5   987.77f
#define NOTE_A5   880.00f

/* 单音时长 (ms) */
#define CHARGE_NOTE_MS   110
#define TICK_NOTE_MS      60

/* 峰值幅度 (16bit 有符号)。提示音要清亮但不刺耳，取 1/3 满量程 */
#define CHIME_AMPLITUDE  11000

/* 包络：attack 防起始爆音，decay 做自然收尾 */
#define ATTACK_RATIO     0.10f
#define DECAY_RATIO      0.65f

/* 分块合成，避免一次性分配大缓冲 */
#define BLOCK_FRAMES     256

typedef struct {
    float  freq;
    int    duration_ms;
    float  amplitude;
} tone_t;

static bool s_ready = false;
static QueueHandle_t s_queue = NULL;
static bool (*s_is_busy)(void) = NULL;

/* 常驻任务配置：PSRAM 静态栈，避免内部 SRAM 碎片（见记忆：动态建 task 会 OOM） */
#define CHIME_TASK_STACK_BYTES (6 * 1024)
#define CHIME_QUEUE_LEN 4
static StaticTask_t   s_task_tcb;
static StaticQueue_t  s_queue_buf;
static StackType_t   *s_task_stack = NULL;
static uint8_t       *s_queue_storage = NULL;

/**
 * 合成并播放一段带包络的正弦音
 * 返回 false 表示 I2S 不可用（未初始化或被占用）
 */
static bool play_tone(const tone_t *tone)
{
    int rate = audio_out_get_rate();
    int ch = audio_out_get_ch();
    if (rate <= 0 || ch <= 0) {
        ESP_LOGW(TAG, "I2S not ready (rate=%d ch=%d)", rate, ch);
        return false;
    }
    if (audio_out_is_paused()) {
        ESP_LOGW(TAG, "I2S paused, skip chime");
        return false;
    }

    const int total_frames = (rate * tone->duration_ms) / 1000;
    const int block_samples = BLOCK_FRAMES * ch;
    int16_t *buf = heap_caps_malloc(block_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "alloc failed");
        return false;
    }

    int frames_done = 0;
    while (frames_done < total_frames) {
        int n = total_frames - frames_done;
        if (n > BLOCK_FRAMES) n = BLOCK_FRAMES;

        for (int i = 0; i < n; i++) {
            const int frame = frames_done + i;
            const float t = (float)frame / (float)rate;
            float env = 1.0f;

            /* attack */
            const int attack_frames = (int)(total_frames * ATTACK_RATIO);
            if (frame < attack_frames && attack_frames > 0) {
                env = (float)frame / (float)attack_frames;
            }
            /* decay：指数衰减到 decay 比例 */
            const int decay_start = (int)(total_frames * (1.0f - DECAY_RATIO));
            if (frame > decay_start) {
                const int decay_frames = total_frames - decay_start;
                const float k = (float)(frame - decay_start) / (float)decay_frames;
                env *= (1.0f - k) * (1.0f - k);   /* 二次曲线更自然 */
            }

            float s = sinf(2.0f * M_PI * tone->freq * t) * env * tone->amplitude;
            int16_t v = (int16_t)(s);
            for (int c = 0; c < ch; c++) {
                buf[i * ch + c] = v;
            }
        }

        size_t written = 0;
        esp_err_t ret = audio_out_write(buf, n * ch * sizeof(int16_t), &written, 500);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "write failed: %d", ret);
            heap_caps_free(buf);
            return false;
        }
        frames_done += n;
    }

    heap_caps_free(buf);
    return true;
}

static void chime_task(void *arg)
{
    uint32_t kind = 0;
    while (1) {
        if (xQueueReceive(s_queue, &kind, portMAX_DELAY) != pdTRUE) continue;

        /* 音乐播放中不抢 I2S —— GMF 管线正在写，混写会爆音 */
        if (s_is_busy && s_is_busy()) {
            ESP_LOGD(TAG, "audio busy, skip chime");
            continue;
        }

        if (kind == 0) {
            /* 小米充电提示音：E5 → B5 上行 */
            tone_t t1 = { .freq = NOTE_E5, .duration_ms = CHARGE_NOTE_MS,
                          .amplitude = CHIME_AMPLITUDE };
            if (!play_tone(&t1)) continue;
            tone_t t2 = { .freq = NOTE_B5, .duration_ms = CHARGE_NOTE_MS + 40,
                          .amplitude = CHIME_AMPLITUDE };
            play_tone(&t2);
        } else {
            tone_t t = { .freq = NOTE_A5, .duration_ms = TICK_NOTE_MS,
                         .amplitude = (float)CHIME_AMPLITUDE * 0.6f };
            play_tone(&t);
        }
    }
}

static void chime_start(uint32_t kind)
{
    if (!s_ready || !s_queue) return;
    /* 队列满则丢弃（提示音不需要堆积） */
    xQueueSend(s_queue, &kind, 0);
}

void chime_init(void)
{
    if (s_ready) return;

    s_task_stack = heap_caps_malloc(CHIME_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM);
    s_queue_storage = heap_caps_malloc(CHIME_QUEUE_LEN * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_task_stack || !s_queue_storage) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        return;
    }

    s_queue = xQueueCreateStatic(CHIME_QUEUE_LEN, sizeof(uint32_t),
                                 s_queue_storage, &s_queue_buf);
    if (!s_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    /* 记忆教训：xTaskCreateStaticPinnedToCore 前必须 memset TCB，否则 StoreProhibited */
    memset(&s_task_tcb, 0, sizeof(s_task_tcb));

    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        chime_task, "chime", CHIME_TASK_STACK_BYTES / sizeof(StackType_t),
        NULL, 5, s_task_stack, &s_task_tcb, 1);
    if (!h) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready");
}

void chime_set_busy_check(bool (*is_busy)(void))
{
    s_is_busy = is_busy;
}

void chime_play_charge(void)
{
    chime_start(0);
}

void chime_play_tick(void)
{
    chime_start(1);
}

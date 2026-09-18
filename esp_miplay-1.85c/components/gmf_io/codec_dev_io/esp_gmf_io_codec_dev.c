/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO., LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include "esp_gmf_io_codec_dev.h"
#include "esp_gmf_oal_mem.h"
#include "esp_log.h"
#include "board.h"
#include "esp_ae_sonic.h"
#include "esp_heap_caps.h"

/* ESP-NOW 音频对齐：UI 任务设置每秒应多写的字节数（正值=本机超前需减速） */
volatile int32_t g_espnow_adjust_bytes_per_sec = 0;
/* I2S 实际写入字节计数（32位，够用6小时，ESP32-S3原子读取） */
static volatile uint32_t s_i2s_bytes_written = 0;
uint32_t codec_dev_get_i2s_bytes_written(void) { return s_i2s_bytes_written; }
void codec_dev_reset_i2s_bytes_written(void) { s_i2s_bytes_written = 0; }

/* ── Sonic 变速不变调（多设备音频对齐执行器）── */
static esp_ae_sonic_handle_t s_sonic_handle = NULL;
static int16_t *s_sonic_out_buf = NULL;
static volatile float s_sonic_speed = 1.0f;
static volatile bool s_sonic_active = false;   /* MiPlay 播放时为 true */
#define SONIC_OUT_BUF_SAMPLES  4096   /* sample points, stereo = 16KB */

void codec_dev_sonic_init(int sample_rate, int channels, int bits)
{
    if (s_sonic_handle) return;  /* 已初始化 */
    esp_ae_sonic_cfg_t cfg = {
        .sample_rate = sample_rate,
        .channel = channels,
        .bits_per_sample = bits,
    };
    esp_ae_err_t err = esp_ae_sonic_open(&cfg, &s_sonic_handle);
    if (err != ESP_AE_ERR_OK || !s_sonic_handle) {
        ESP_LOGW("CODEC_IO", "Sonic open failed: %d", err);
        s_sonic_handle = NULL;
        return;
    }
    esp_ae_sonic_set_speed(s_sonic_handle, 1.0f);
    s_sonic_out_buf = heap_caps_malloc(SONIC_OUT_BUF_SAMPLES * channels * (bits / 8), MALLOC_CAP_SPIRAM);
    s_sonic_speed = 1.0f;
    s_sonic_active = true;
    ESP_LOGI("CODEC_IO", "Sonic init: %dHz %dch %dbit", sample_rate, channels, bits);
}

void codec_dev_sonic_deinit(void)
{
    s_sonic_active = false;
    if (s_sonic_handle) {
        esp_ae_sonic_close(s_sonic_handle);
        s_sonic_handle = NULL;
    }
    if (s_sonic_out_buf) {
        heap_caps_free(s_sonic_out_buf);
        s_sonic_out_buf = NULL;
    }
    s_sonic_speed = 1.0f;
}

void codec_dev_sonic_set_speed(float speed)
{
    s_sonic_speed = speed;
    if (s_sonic_handle) {
        esp_ae_sonic_set_speed(s_sonic_handle, speed);
    }
}

bool codec_dev_sonic_is_active(void)
{
    return s_sonic_active;
}
/* 累积的微调余量（字节），每次 I2S 写入时消耗 */
static int32_t s_sync_adjust_bytes = 0;

/**
 * @brief  Codec device io context in GMF
 */
typedef struct {
    esp_gmf_io_t  base;     /*!< The GMF codec dev io handle */
    bool          is_open;  /*!< The flag of whether opened */
} codec_dev_io_stream_t;

static const char *TAG = "ESP_GMF_CODEC_DEV";

static esp_gmf_err_t esp_gmf_io_codec_dev_new(void *cfg, esp_gmf_obj_handle_t *io)
{
    return esp_gmf_io_codec_dev_init(cfg, io);
}

static esp_gmf_err_t _codec_dev_open(esp_gmf_io_handle_t io)
{
    codec_dev_io_stream_t *codec_dev_io = (codec_dev_io_stream_t *)io;
    codec_dev_io_cfg_t *cfg = (codec_dev_io_cfg_t *)OBJ_GET_CFG(codec_dev_io);
    ESP_GMF_NULL_CHECK(TAG, cfg, return ESP_GMF_ERR_FAIL;);
    ESP_GMF_CHECK(TAG, cfg->dev, {return ESP_GMF_ERR_FAIL;}, "There is no activated I2S driver handle");
    codec_dev_io->is_open = true;
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_io_t _codec_dev_acquire_read(esp_gmf_io_handle_t handle, void *payload, uint32_t wanted_size, int block_ticks)
{
    codec_dev_io_stream_t *codec_dev_io = (codec_dev_io_stream_t *)handle;
    esp_gmf_payload_t *pload = (esp_gmf_payload_t *)payload;
    codec_dev_io_cfg_t *cfg = (codec_dev_io_cfg_t *)OBJ_GET_CFG(codec_dev_io);
    ESP_GMF_NULL_CHECK(TAG, cfg, return ESP_GMF_IO_FAIL;);
    if (wanted_size == 0) {
        pload->valid_size = 0;
        return ESP_GMF_IO_OK;
    }
    if (esp_codec_dev_read(cfg->dev, pload->buf, wanted_size) != ESP_GMF_IO_OK) {
        ESP_LOGE(TAG, "Read failed, wanted: %ld", wanted_size);
        return ESP_GMF_IO_FAIL;
    }
    pload->valid_size = wanted_size;
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _codec_dev_release_read(esp_gmf_io_handle_t handle, void *payload, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _codec_dev_acquire_write(esp_gmf_io_handle_t handle, void *payload, uint32_t wanted_size, int block_ticks)
{
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_io_t _codec_dev_release_write(esp_gmf_io_handle_t handle, void *payload, int block_ticks)
{
    esp_gmf_payload_t *pload = (esp_gmf_payload_t *)payload;
    (void)handle;
    if (pload->valid_size == 0) {
        return ESP_GMF_IO_OK;
    }
    ESP_LOGD(TAG, "Write %d bytes", pload->valid_size);
    i2s_chan_handle_t tx_handle = audio_out_get_tx_handle();
    if (!tx_handle) {
        ESP_LOGE(TAG, "tx_handle is NULL");
        return ESP_GMF_IO_FAIL;
    }

    uint8_t *write_buf = (uint8_t *)pload->buf;
    size_t write_size = pload->valid_size;

    /* Sonic 变速不变调处理（多设备音频对齐）：
     * speed ≈ 1.0 时绕过 Sonic 直通，避免内部 look-ahead 缓冲导致 I2S 欠载卡顿。 */
    float spd = s_sonic_speed;
    if (s_sonic_active && s_sonic_handle && s_sonic_out_buf && (spd < 0.995f || spd > 1.005f)) {
        int16_t *in = (int16_t *)pload->buf;
        uint32_t sample_points = pload->valid_size / (2 * sizeof(int16_t)); /* 2ch, 16bit */
        esp_ae_sonic_in_data_t  sin  = { .samples = in, .num = sample_points };
        esp_ae_sonic_out_data_t sout = { .samples = s_sonic_out_buf,
                                         .needed_num = sample_points };
        esp_ae_sonic_process(s_sonic_handle, &sin, &sout);
        if (sout.out_num > 0) {
            write_buf = (uint8_t *)s_sonic_out_buf;
            write_size = sout.out_num * 2 * sizeof(int16_t);
        } else {
            return ESP_GMF_IO_OK;  /* Sonic 无输出（内部缓冲中），跳过本次写入 */
        }
    }

    size_t bytes_written = 0;
    size_t total_written = 0;
    uint8_t *data = write_buf;
    size_t remaining = write_size;
    while (remaining > 0) {
        esp_err_t ret = i2s_channel_write(tx_handle, data, remaining, &bytes_written, 500);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed, remaining: %d, ret: %d", remaining, ret);
            return ESP_GMF_IO_FAIL;
        }
        total_written += bytes_written;
        data += bytes_written;
        remaining -= bytes_written;
    }
    s_i2s_bytes_written += total_written;
    return ESP_GMF_IO_OK;
}

static esp_gmf_err_t _codec_dev_seek(esp_gmf_io_handle_t io, uint64_t seek_byte_pos)
{
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _codec_dev_close(esp_gmf_io_handle_t io)
{
    codec_dev_io_stream_t *codec_dev_io = (codec_dev_io_stream_t *)io;
    esp_gmf_info_file_t info = {0};
    esp_gmf_io_get_info((esp_gmf_io_handle_t)codec_dev_io, &info);
    ESP_LOGI(TAG, "CLose, %p, pos = %d/%d", codec_dev_io, (int)info.pos, (int)info.size);
    if (codec_dev_io->is_open) {
        codec_dev_io->is_open = false;
    }
    esp_gmf_io_set_pos((esp_gmf_io_handle_t)io, 0);
    return ESP_GMF_ERR_OK;
}

static esp_gmf_err_t _codec_dev_delete(esp_gmf_io_handle_t io)
{
    if (io != NULL) {
        codec_dev_io_stream_t *codec_dev_io = (codec_dev_io_stream_t *)io;
        ESP_LOGD(TAG, "Delete, %s-%p", OBJ_GET_TAG(codec_dev_io), codec_dev_io);
        esp_gmf_oal_free(OBJ_GET_CFG(codec_dev_io));
        esp_gmf_io_deinit(io);
        esp_gmf_oal_free(codec_dev_io);
    }
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t esp_gmf_io_codec_dev_init(codec_dev_io_cfg_t *config, esp_gmf_io_handle_t *io)
{
    ESP_GMF_NULL_CHECK(TAG, config, {return ESP_GMF_ERR_INVALID_ARG;});
    ESP_GMF_NULL_CHECK(TAG, io, {return ESP_GMF_ERR_INVALID_ARG;});
    *io = NULL;
    esp_gmf_err_t ret = ESP_GMF_ERR_OK;
    codec_dev_io_stream_t *codec_dev_io = esp_gmf_oal_calloc(1, sizeof(codec_dev_io_stream_t));
    ESP_GMF_MEM_VERIFY(TAG, codec_dev_io, return ESP_GMF_ERR_MEMORY_LACK,
                       "codec device", sizeof(codec_dev_io_stream_t));
    codec_dev_io->base.dir = config->dir;
    codec_dev_io->base.type = ESP_GMF_IO_TYPE_BYTE;
    esp_gmf_obj_t *obj = (esp_gmf_obj_t *)codec_dev_io;
    obj->new_obj = esp_gmf_io_codec_dev_new;
    obj->del_obj = _codec_dev_delete;
    codec_dev_io_cfg_t *cfg = esp_gmf_oal_calloc(1, sizeof(*config));
    ESP_GMF_MEM_VERIFY(TAG, cfg, {ret = ESP_GMF_ERR_MEMORY_LACK; goto _codec_dev_fail;}, "codec device configuration", sizeof(*config));
    memcpy(cfg, config, sizeof(*config));
    esp_gmf_obj_set_config(obj, cfg, sizeof(*config));
    ret = esp_gmf_obj_set_tag(obj, (config->name == NULL ? "io_codec_dev" : config->name));
    ESP_GMF_RET_ON_NOT_OK(TAG, ret, goto _codec_dev_fail, "Failed to set obj tag");
    codec_dev_io->base.close = _codec_dev_close;
    codec_dev_io->base.open = _codec_dev_open;
    codec_dev_io->base.seek = _codec_dev_seek;
    codec_dev_io->base.reset = NULL;
    if (codec_dev_io->base.dir == ESP_GMF_IO_DIR_WRITER) {
        codec_dev_io->base.acquire_write = _codec_dev_acquire_write;
        codec_dev_io->base.release_write = _codec_dev_release_write;
    } else if (codec_dev_io->base.dir == ESP_GMF_IO_DIR_READER) {
        codec_dev_io->base.acquire_read = _codec_dev_acquire_read;
        codec_dev_io->base.release_read = _codec_dev_release_read;
    } else {
        ESP_LOGE(TAG, "Does not set read or write function");
        ret = ESP_GMF_ERR_NOT_SUPPORT;
        goto _codec_dev_fail;
    }
    esp_gmf_io_cfg_t io_cfg = {
        .thread = {
            .stack = config->io_cfg.thread.stack,
            .prio = config->io_cfg.thread.prio,
            .core = config->io_cfg.thread.core,
            .stack_in_ext = config->io_cfg.thread.stack_in_ext,
        },
        .buffer_cfg = {
            .io_size = config->io_cfg.buffer_cfg.io_size,
            .buffer_size = config->io_cfg.buffer_cfg.buffer_size,
        },
        .enable_speed_monitor = config->io_cfg.enable_speed_monitor,
    };
    ret = esp_gmf_io_init(obj, &io_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        goto _codec_dev_fail;
    }
    *io = obj;
    ESP_LOGD(TAG, "Initialization, %s-%p", OBJ_GET_TAG(obj), codec_dev_io);
    return ESP_GMF_ERR_OK;
_codec_dev_fail:
    esp_gmf_obj_delete(obj);
    return ret;
}

esp_gmf_err_t esp_gmf_io_codec_dev_set_dev(esp_gmf_io_handle_t io, esp_codec_dev_handle_t dev)
{
    codec_dev_io_stream_t *codec_dev_io = (codec_dev_io_stream_t *)io;
    ESP_GMF_NULL_CHECK(TAG, codec_dev_io, return ESP_GMF_ERR_INVALID_ARG;);

    codec_dev_io_cfg_t *cfg = (codec_dev_io_cfg_t *)OBJ_GET_CFG(codec_dev_io);
    ESP_GMF_NULL_CHECK(TAG, cfg, return ESP_GMF_ERR_INVALID_STATE;);

    cfg->dev = dev;
    return ESP_GMF_ERR_OK;
}

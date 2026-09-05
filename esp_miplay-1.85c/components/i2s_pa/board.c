/*
 * ESP32-S3-Touch-LCD-1.85C audio output: PCM5101A pure I2S DAC.
 * No MCLK, no I2C codec, no PA control — data flows directly.
 */

#include "esp_check.h"
#include "esp_log.h"
#include "esp_err.h"
#include "echopal_board.h"
#include "include/board.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "AUDIO_OUT";

static i2s_chan_handle_t s_tx_handle;
static esp_codec_dev_handle_t s_codec_dev;
static bool s_initialized;
static SemaphoreHandle_t s_audio_mux;
static bool s_output_enabled;
static bool s_output_paused = true;
static int s_current_rate;
static int s_current_ch;
static int s_current_bits;

i2s_chan_handle_t audio_out_get_tx_handle(void)
{
    return s_tx_handle;
}

esp_codec_dev_handle_t audio_out_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return s_codec_dev;
    }

    if (!s_audio_mux) {
        s_audio_mux = xSemaphoreCreateMutex();
        if (!s_audio_mux) {
            ESP_LOGE(TAG, "Create audio output mutex failed");
            return NULL;
        }
    }

    ESP_RETURN_ON_FALSE(echopal_board_init() == ESP_OK, NULL, TAG, "Init board failed");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 12;
    chan_cfg.dma_frame_num = 512;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        goto error;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,  /* PCM5101A: no MCLK */
            .bclk = GPIO_I2S_SCLK,
            .ws = GPIO_I2S_LRCK,
            .dout = GPIO_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        goto error;
    }

    /* PCM5101A: no I2C codec — create a minimal codec_dev wrapper for esp_gmf compatibility */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx_handle,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *codec_data = audio_codec_new_i2s_data(&i2s_cfg);
    if (codec_data == NULL) goto error;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = NULL,  /* No control interface for PCM5101A */
        .data_if = codec_data,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == NULL) goto error;

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = 48000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    int codec_ret = esp_codec_dev_open(s_codec_dev, &sample_info);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", codec_ret);
        goto error;
    }

    i2s_channel_enable(s_tx_handle);

    s_initialized = true;
    s_output_enabled = true;
    s_output_paused = false;
    s_current_rate = sample_info.sample_rate;
    s_current_ch = sample_info.channel;
    s_current_bits = sample_info.bits_per_sample;
    ESP_LOGI(TAG, "PCM5101A ready: BCK=%d LRCK=%d DOUT=%d (no MCLK)",
             GPIO_I2S_SCLK, GPIO_I2S_LRCK, GPIO_I2S_DOUT);
    return s_codec_dev;

error:
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    if (s_tx_handle) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    s_output_enabled = false;
    s_output_paused = true;
    s_current_rate = 0;
    s_current_ch = 0;
    s_current_bits = 0;
    return NULL;
}

esp_err_t audio_out_pause(void)
{
    if (!s_initialized || !s_tx_handle) return ESP_ERR_INVALID_STATE;
    if (s_audio_mux) xSemaphoreTake(s_audio_mux, portMAX_DELAY);

    esp_err_t ret = ESP_OK;
    if (s_output_enabled) {
        ret = i2s_channel_disable(s_tx_handle);
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            ret = ESP_OK;
            s_output_enabled = false;
        }
    }
    s_output_paused = true;

    if (s_audio_mux) xSemaphoreGive(s_audio_mux);
    return ret;
}

esp_err_t audio_out_resume(void)
{
    if (!s_initialized || !s_tx_handle) return ESP_ERR_INVALID_STATE;
    if (s_audio_mux) xSemaphoreTake(s_audio_mux, portMAX_DELAY);

    esp_err_t ret = ESP_OK;
    if (!s_output_enabled) {
        ret = i2s_channel_enable(s_tx_handle);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            if (s_audio_mux) xSemaphoreGive(s_audio_mux);
            return ret;
        }
    }
    s_output_enabled = true;
    s_output_paused = false;

    if (s_audio_mux) xSemaphoreGive(s_audio_mux);
    return ret;
}

esp_err_t audio_out_write(const void *data, size_t size,
                          size_t *bytes_written, uint32_t timeout_ms)
{
    if (bytes_written) *bytes_written = 0;
    if (!s_initialized || !s_tx_handle || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio_mux) xSemaphoreTake(s_audio_mux, portMAX_DELAY);
    if (!s_output_enabled || s_output_paused) {
        if (s_audio_mux) xSemaphoreGive(s_audio_mux);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2s_channel_write(s_tx_handle, data, size, bytes_written,
                                      pdMS_TO_TICKS(timeout_ms));
    if (s_audio_mux) xSemaphoreGive(s_audio_mux);
    return ret;
}

bool audio_out_is_paused(void)
{
    if (s_audio_mux) xSemaphoreTake(s_audio_mux, portMAX_DELAY);
    bool paused = !s_initialized || s_output_paused;
    if (s_audio_mux) xSemaphoreGive(s_audio_mux);
    return paused;
}

void audio_out_set_clk(esp_codec_dev_handle_t dev, int rate, int ch, int bits)
{
    (void)dev;
    if (!s_initialized || !s_tx_handle) {
        ESP_LOGW(TAG, "I2S not ready, skip clock reconfiguration");
        return;
    }
    int target_ch = ch < 2 ? 1 : 2;

    if (s_audio_mux) xSemaphoreTake(s_audio_mux, portMAX_DELAY);
    if (rate == s_current_rate && target_ch == s_current_ch &&
        bits == s_current_bits) {
        if (s_audio_mux) xSemaphoreGive(s_audio_mux);
        return;
    }

    /* 禁用通道 → 重配时钟/时隙 → 重新启用（原子操作）
     * 直接操作 I2S 驱动 API，不走 esp_codec_dev（PCM5101A 无控制接口，
     * codec_if=NULL 时 close/open 不会触发 I2S 硬件重配） */
    bool restore_output = s_output_enabled && !s_output_paused;
    if (s_output_enabled) {
        esp_err_t disable_ret = i2s_channel_disable(s_tx_handle);
        if (disable_ret != ESP_OK && disable_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Disable I2S before reconfiguration failed: %s",
                     esp_err_to_name(disable_ret));
        }
        s_output_enabled = false;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    if (bits == 24) {
        clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    }
    esp_err_t ret = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reconfig_std_clock(%d) failed: %s", rate, esp_err_to_name(ret));
    }

    i2s_data_bit_width_t bit_w;
    switch (bits) {
        case 24: bit_w = I2S_DATA_BIT_WIDTH_24BIT; break;
        case 32: bit_w = I2S_DATA_BIT_WIDTH_32BIT; break;
        default: bit_w = I2S_DATA_BIT_WIDTH_16BIT; break;
    }
    i2s_slot_mode_t slot_mode = (target_ch <= 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_w, slot_mode);
    ret = i2s_channel_reconfig_std_slot(s_tx_handle, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reconfig_std_slot failed: %s", esp_err_to_name(ret));
    }

    s_current_rate = rate;
    s_current_ch = target_ch;
    s_current_bits = bits;

    if (restore_output && !s_output_paused) {
        esp_err_t enable_ret = i2s_channel_enable(s_tx_handle);
        if (enable_ret == ESP_OK || enable_ret == ESP_ERR_INVALID_STATE) {
            s_output_enabled = true;
            s_output_paused = false;
        } else {
            ESP_LOGW(TAG, "Restore I2S after reconfiguration failed: %s",
                     esp_err_to_name(enable_ret));
        }
    }
    ESP_LOGI(TAG, "I2S reconfig: %d Hz, %d bit, %d ch", rate, bits, ch);
    if (s_audio_mux) xSemaphoreGive(s_audio_mux);
}

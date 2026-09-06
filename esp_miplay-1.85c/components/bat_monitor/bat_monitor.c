#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "bat_monitor.h"

static const char *TAG = "BAT_MON";

/* ADC1 CH7 = GPIO8，板载电池分压检测点 */
#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHAN        ADC_CHANNEL_7
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12

/* 分压比：硬件 1/3 分压，读数需 ×3 */
#define BAT_DIVIDER_RATIO   3.0f
/* 实测校准偏移（来自厂商 Demo） */
#define BAT_MEASURE_OFFSET  0.9945f

/* 锂电电压区间（V） */
#define BAT_VOLT_MIN        3.30f   /* 0%   — 3.0V 已接近过放保护，留余量 */
#define BAT_VOLT_LOW        3.60f   /* 拐点 */
#define BAT_VOLT_MID        3.85f
#define BAT_VOLT_MAX        4.20f   /* 100% */
#define BAT_VOLT_CHARGING   4.15f   /* 高于此值视为充电中 */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_calibrated = false;
static bool                      s_ready = false;

static bool adc_calibration_init(adc_unit_t unit, adc_channel_t chan, adc_atten_t atten,
                                 adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cfg = {
            .unit_id = unit,
            .chan = chan,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cfg, &handle);
        if (ret == ESP_OK) calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cfg = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cfg, &handle);
        if (ret == ESP_OK) calibrated = true;
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "eFuse not burnt, skip calibration");
    } else {
        ESP_LOGE(TAG, "calibration failed: %s", esp_err_to_name(ret));
    }
    return calibrated;
}

void bat_monitor_init(void)
{
    if (s_ready) return;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHAN, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return;
    }

    s_calibrated = adc_calibration_init(BAT_ADC_UNIT, BAT_ADC_CHAN, BAT_ADC_ATTEN, &s_cali_handle);
    s_ready = true;
    ESP_LOGI(TAG, "init done (chan=%d, calibrated=%d)", BAT_ADC_CHAN, s_calibrated);
}

float bat_monitor_get_volts(void)
{
    if (!s_ready) return 0.0f;

    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, BAT_ADC_CHAN, &raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc read failed: %s", esp_err_to_name(err));
        return 0.0f;
    }

    int mv = 0;
    if (s_calibrated) {
        err = adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "raw_to_voltage failed: %s", esp_err_to_name(err));
            return 0.0f;
        }
    } else {
        /* 无校准：12dB 衰减约 0~3.3V，12bit */
        mv = (raw * 3300) / 4095;
    }

    return ((float)mv / 1000.0f) * BAT_DIVIDER_RATIO / BAT_MEASURE_OFFSET;
}

int bat_monitor_get_percent(void)
{
    float volts = bat_monitor_get_volts();
    if (volts <= 0.0f) return 0;
    if (volts <= BAT_VOLT_MIN) return 0;
    if (volts >= BAT_VOLT_MAX) return 100;

    /* 分段线性：低段斜率小（3.3~3.6 掉得快），中段陡，高段缓 */
    if (volts < BAT_VOLT_LOW) {
        return (int)((volts - BAT_VOLT_MIN) / (BAT_VOLT_LOW - BAT_VOLT_MIN) * 20.0f);
    } else if (volts < BAT_VOLT_MID) {
        return 20 + (int)((volts - BAT_VOLT_LOW) / (BAT_VOLT_MID - BAT_VOLT_LOW) * 40.0f);
    } else {
        return 60 + (int)((volts - BAT_VOLT_MID) / (BAT_VOLT_MAX - BAT_VOLT_MID) * 40.0f);
    }
}

bool bat_monitor_is_charging(void)
{
    float volts = bat_monitor_get_volts();
    return (volts > 0.0f && volts >= BAT_VOLT_CHARGING);
}

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "bat_monitor.h"

static const char *TAG = "BAT_MON";

/* ======== 硬件参数（ESP32-S3-Touch-LCD-1.85C） ======== */
#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHAN        ADC_CHANNEL_7   /* GPIO8 */
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12

#define BAT_DIVIDER_RATIO   3.0f    /* 板载 1/3 分压 */
#define BAT_MEASURE_OFFSET  0.9945f /* 厂商校准偏移 */

/* ======== 采样与滤波参数 ======== */
#define BAT_SAMPLE_COUNT    16      /* 每次读取的 ADC 采样次数 */
#define BAT_EMA_ALPHA       32      /* EMA 系数 (0~256)，32≈12.5%，平滑约8个周期 */
#define BAT_INIT_SAMPLES    32      /* 启动时预填充采样次数 */

/* ======== 电池电压范围 ======== *
 * 保护板 1S LiPo：满电 4.0V，过放保护 3.0V，实际使用 3.3~4.0V */
#define BAT_VOLT_EMPTY      3300    /* mV — 0%  (保护板余量) */
#define BAT_VOLT_FULL       4000    /* mV — 100% (保护板满电) */

/* ======== 充电检测 ======== */
#define BAT_VOLT_CHARGING   3850    /* mV — 高于此值视为充电中 */
#define BAT_CHARGE_DEBOUNCE_N 2     /* 连续N次一致才翻转 (EMA已滤波，2次够用) */
#define BAT_MAX_DROP_PER_READ 2    /* 放电态单次最大下降% */

/* ======== 内部状态 ======== */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_calibrated = false;
static bool                      s_ready = false;

static int   s_filt_mv = 0;       /* EMA 滤波后的 mV */
static int   s_prev_pct = -1;     /* 上次百分比 */
static bool  s_prev_charging = false;

/* ============================================================
 * ADC 校准初始化
 * ============================================================ */
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t chan,
                                 adc_atten_t atten, adc_cali_handle_t *out)
{
    adc_cali_handle_t h = NULL;
    esp_err_t ret = ESP_FAIL;
    bool ok = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!ok) {
        adc_cali_curve_fitting_config_t c = {
            .unit_id = unit, .chan = chan, .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&c, &h);
        if (ret == ESP_OK) ok = true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!ok) {
        adc_cali_line_fitting_config_t c = {
            .unit_id = unit, .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&c, &h);
        if (ret == ESP_OK) ok = true;
    }
#endif
    *out = h;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !ok) {
        ESP_LOGW(TAG, "eFuse not burnt, skip calibration");
    } else {
        ESP_LOGE(TAG, "calibration failed: %s", esp_err_to_name(ret));
    }
    return ok;
}

/* ============================================================
 * 初始化
 * ============================================================ */
void bat_monitor_init(void)
{
    if (s_ready) return;

    adc_oneshot_unit_init_cfg_t uc = { .unit_id = BAT_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&uc, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t cc = {
        .atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHAN, &cc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        return;
    }

    s_calibrated = adc_calibration_init(BAT_ADC_UNIT, BAT_ADC_CHAN,
                                        BAT_ADC_ATTEN, &s_cali_handle);

    /* 预填充 EMA 滤波器 */
    int sum = 0;
    for (int i = 0; i < BAT_INIT_SAMPLES; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(s_adc_handle, BAT_ADC_CHAN, &raw);
        if (s_calibrated) {
            adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
        } else {
            mv = (raw * 3300) / 4095;
        }
        sum += mv;
    }
    s_filt_mv = sum / BAT_INIT_SAMPLES;

    s_ready = true;
    ESP_LOGI(TAG, "init done (chan=%d cal=%d init_mv=%d)",
             BAT_ADC_CHAN, s_calibrated, s_filt_mv);
}

/* ============================================================
 * 读取电池电压（V）— 多次采样 + EMA 滤波
 * ============================================================ */
float bat_monitor_get_volts(void)
{
    if (!s_ready) return 0.0f;

    int sum = 0, valid = 0;
    for (int i = 0; i < BAT_SAMPLE_COUNT; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc_handle, BAT_ADC_CHAN, &raw) != ESP_OK)
            continue;
        if (s_calibrated) {
            if (adc_cali_raw_to_voltage(s_cali_handle, raw, &mv) != ESP_OK)
                continue;
        } else {
            mv = (raw * 3300) / 4095;
        }
        sum += mv;
        valid++;
    }

    if (valid > 0) {
        s_filt_mv = s_filt_mv + ((sum / valid - s_filt_mv) * BAT_EMA_ALPHA) / 256;
    }

    return ((float)s_filt_mv / 1000.0f) * BAT_DIVIDER_RATIO / BAT_MEASURE_OFFSET;
}

/* ============================================================
 * 非对称 Sigmoid 电压→百分比
 *
 * 来源：BatterySense 库 (rlogiacco/BatterySense, LGPL-3.0)
 *       https://www.desmos.com/calculator/umxfnnpj7r
 *
 * 公式：101 - 101 / (1 + (1.33 * x)^4.5)^3
 * 其中 x = (V - Vmin) / (Vmax - Vmin)，归一化到 [0,1]
 *
 * 曲线特征（天然拟合锂电放电曲线）：
 *   4.00V → 100%    3.80V → 87%    3.60V → 53%
 *   3.90V →  96%    3.70V → 72%    3.50V → 30%
 *   3.85V →  93%    3.65V → 63%    3.40V → 12%
 *
 * 优点：两端陡、中间平，完美匹配锂电"高平台+线性+低急降"特征
 * ============================================================ */
static int voltage_to_percent(int mv)
{
    if (mv <= BAT_VOLT_EMPTY) return 0;
    if (mv >= BAT_VOLT_FULL)  return 100;

    float x = (float)(mv - BAT_VOLT_EMPTY) / (float)(BAT_VOLT_FULL - BAT_VOLT_EMPTY);
    float kx = 1.33f * x;
    float result = 101.0f - (101.0f / powf(1.0f + powf(kx, 4.5f), 3.0f));

    int pct = (int)result;
    if (pct > 100) pct = 100;
    if (pct < 0)   pct = 0;
    return pct;
}

/* ============================================================
 * 百分比接口 — 带防跳变
 * ============================================================ */
int bat_monitor_get_percent(void)
{
    float volts = bat_monitor_get_volts();
    int mv = (int)(volts * 1000.0f);

    if (mv <= 0) return s_prev_pct >= 0 ? s_prev_pct : 0;

    bool charging = bat_monitor_is_charging();
    if (charging != s_prev_charging) {
        s_prev_charging = charging;
    }

    int pct = voltage_to_percent(mv);

    /* 防跳变 */
    if (s_prev_pct < 0) {
        s_prev_pct = pct;
    } else if (charging) {
        if (pct < s_prev_pct) pct = s_prev_pct;   /* 充电中只升不降 */
    } else {
        if (s_prev_pct - pct > BAT_MAX_DROP_PER_READ)  /* 放电限制降幅 */
            pct = s_prev_pct - BAT_MAX_DROP_PER_READ;
    }

    s_prev_pct = pct;
    return pct;
}

/* ============================================================
 * 充电状态检测（带去抖）
 * ============================================================ */
bool bat_monitor_is_charging(void)
{
    /*
     * 纯电压检测无法区分"USB插着+满电"和"刚拔线+满电"（都是 4.0V）。
     * 但拔线后电压会在几秒内从 4.00V 降到 3.98V。
     *
     * 策略：用 EMA 滤波后的电压变化趋势判断——
     *   - 电压上升或持平 → "充电中"
     *   - 电压连续下降超过 8mV → "未充电"（拔线了）
     *
     * 这样满电插着USB时保持充电图标，拔线后约 3~5 秒切换。
     */
    static bool  s_state = false;
    static int   s_peak_mv = 0;     /* 近期电压峰值 */
    static int   s_drop_count = 0;  /* 连续下降计数 */

    int mv = (int)(bat_monitor_get_volts() * 1000.0f);
    if (mv <= 0) return s_state;

    /* 无滤波的原始读数做充电判定：电压高于阈值才有可能是充电 */
    if (mv < BAT_VOLT_CHARGING) {
        if (s_state) {
            s_state = false;
            s_peak_mv = 0;
            s_drop_count = 0;
            ESP_LOGI(TAG, "charging -> 0 (%dmV, below threshold)", mv);
        }
        return false;
    }

    /* 满电(100%)时返回 false，避免"插着USB+满电"一直显示充电图标。
     * 只有 99% 及以下才根据电压趋势判断充电状态。 */
    if (mv >= BAT_VOLT_FULL) {
        if (s_state) {
            s_state = false;
            s_peak_mv = mv;
            s_drop_count = 0;
            ESP_LOGI(TAG, "charging -> 0 (%dmV, full)", mv);
        }
        return false;
    }

    /* 电压高于充电阈值：看趋势 */
    if (mv >= s_peak_mv) {
        /* 电压上升或持平 → 充电中 */
        s_peak_mv = mv;
        s_drop_count = 0;
        if (!s_state) {
            s_state = true;
            ESP_LOGI(TAG, "charging -> 1 (%dmV, rising)", mv);
        }
    } else {
        /* 电压下降 */
        s_drop_count++;
        int drop = s_peak_mv - mv;
        if (drop >= 8 && s_drop_count >= 2) {
            /* 连续下降超过 8mV → 判定拔线 */
            if (s_state) {
                s_state = false;
                ESP_LOGI(TAG, "charging -> 0 (%dmV, drop %dmV from peak)", mv, drop);
            }
            s_peak_mv = mv;
            s_drop_count = 0;
        }
    }

    return s_state;
}

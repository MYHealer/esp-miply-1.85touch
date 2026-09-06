#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化电池 ADC（ADC1 CH7 / GPIO8, 12dB 衰减）
 * 可重复调用，内部已初始化则直接返回。
 */
void bat_monitor_init(void);

/**
 * 读取电池电压（伏特）
 * @return 电压值，单位 V；未初始化或读取失败返回 0
 */
float bat_monitor_get_volts(void);

/**
 * 电压 → 电量百分比
 * 锂电放电曲线：3.0V=0%, 3.7V=~50%, 4.2V=100%（分段线性近似）
 * @return 0~100
 */
int bat_monitor_get_percent(void);

/**
 * 是否在充电（电压高于 4.15V 视为充电中）
 */
bool bat_monitor_is_charging(void);

#ifdef __cplusplus
}
#endif

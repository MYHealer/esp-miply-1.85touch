#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化提示音模块（需在 I2S audio_out_init() 之后调用）
 * 内部创建常驻任务 + 队列，不占用内部 SRAM。
 */
void chime_init(void);

/**
 * 注册"是否正在播放音乐"的查询回调。
 * 提示音在音乐播放时会跳过，避免与 GMF 管线抢 I2S 产生爆音。
 */
void chime_set_busy_check(bool (*is_busy)(void));

/**
 * 播放小米风格充电提示音（E5 → B5 上行双音）
 * 非阻塞，通过队列投递给常驻任务。
 */
void chime_play_charge(void);

/**
 * 播放轻柔单音，用于触摸反馈等
 */
void chime_play_tick(void);

#ifdef __cplusplus
}
#endif

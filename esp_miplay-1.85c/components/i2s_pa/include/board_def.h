/*
 * ESP32-S3-Touch-LCD-1.85C I2S pin definitions.
 * PCM5101A DAC — no MCLK needed.
 */

#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

#include "driver/gpio.h"

#define GPIO_I2S_LRCK       (GPIO_NUM_38)
#define GPIO_I2S_SCLK       (GPIO_NUM_48)
#define GPIO_I2S_DOUT       (GPIO_NUM_47)

#endif

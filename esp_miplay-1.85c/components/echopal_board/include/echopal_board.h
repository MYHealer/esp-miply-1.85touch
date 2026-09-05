#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C bus pins for ESP32-S3-Touch-LCD-1.85C */
#define ECHOPAL_I2C_SDA_GPIO        GPIO_NUM_11
#define ECHOPAL_I2C_SCL_GPIO        GPIO_NUM_10

/* TCA9554 I/O expander (8-bit, active on this board) */
#define ECHOPAL_TCA9554_ADDR        0x20

/* TCA9554 EXIO pin assignments. Demo TCA9554PWR Set_EXIO(Pin) uses bit (Pin-1):
 * EXIO1 = bit0 (touch reset), EXIO2 = bit1 (LCD reset). Active-low outputs. */
#define ECHOPAL_TOUCH_RST_PIN       0   /* EXIO1 - touch reset (active low) */
#define ECHOPAL_LCD_RST_PIN         1   /* EXIO2 - LCD reset (active low) */

esp_err_t echopal_board_init(void);
i2c_master_bus_handle_t echopal_board_get_i2c(void);
esp_err_t echopal_board_set_touch_reset(bool asserted);
esp_err_t echopal_board_set_lcd_reset(bool asserted);

#ifdef __cplusplus
}
#endif

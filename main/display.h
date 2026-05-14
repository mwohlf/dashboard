#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Initialise the full display subsystem:
 *        I2C bus → backlight controller → MIPI-DSI panel (JD9365) →
 *        GT911 touch → esp_lvgl_port + LVGL display/touch registration.
 *
 * Must be called before any lv_* calls.
 */
esp_err_t display_init(void);

/**
 * @brief Set backlight brightness via I2C controller.
 * @param val  0–255  (values < 4 turn the backlight off)
 */
esp_err_t display_set_brightness(uint8_t val);

/**
 * @brief Return the LVGL display handle (valid after display_init).
 */
lv_disp_t *display_get_lvgl_disp(void);

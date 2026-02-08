/**
 * @file led_status.h
 * @brief Status LED control using WS2812
 */

#pragma once

#include "config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize status LED
 * @return ESP_OK on success
 */
esp_err_t led_status_init(void);

/**
 * @brief Set LED status
 * @param status LED status to set
 */
void led_status_set(led_status_t status);

/**
 * @brief Set custom LED color
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 */
void led_status_set_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif

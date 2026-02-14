#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// DAC80508 channel mapping: laser signal → physical DAC output channel
// ============================================================================
#define DAC_CH_X        6   // X axis → DAC OUT6
#define DAC_CH_Y        7   // Y axis → DAC OUT7
#define DAC_CH_RED      5   // Red    → DAC OUT5
#define DAC_CH_GREEN    4   // Green  → DAC OUT4
#define DAC_CH_BLUE     3   // Blue   → DAC OUT3

/**
 * @brief Initialize DAC80508 
 * @return ESP_OK on success
 */
esp_err_t dac_init(void);

/**
 * @brief Write a single DAC register (channel or config)
 * @param reg Register address (0x00-0x0F)
 * @param value 16-bit value
 * @return ESP_OK on success
 */
esp_err_t dac_write_register(uint8_t reg, uint16_t value);

/**
 * @brief Output a laser point to the correct DAC channels
 * Maps X→OUT6, Y→OUT7, R→OUT5, G→OUT4, B→OUT3
 * @param point Pointer to laser point
 */
void dac_output_point(const laser_point_t* point);

#ifdef __cplusplus
}
#endif

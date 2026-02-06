/**
 * @file dac80508.h
 * @brief DAC80508 8-channel 16-bit DAC driver
 * 
 * Hardware channel mapping (ESP32-S3 → DAC80508 → ILDA):
 *   DAC OUT6 (pin10) = X axis
 *   DAC OUT7 (pin11) = Y axis
 *   DAC OUT3 (pin5)  = Blue
 *   DAC OUT4 (pin8)  = Green
 *   DAC OUT5 (pin9)  = Red
 * 
 * SPI wiring:
 *   GPIO11 (SDI)  → DAC pin14 (SDI)
 *   GPIO10 (CS)   → DAC pin12 (SYNC)
 *   GPIO12 (SCK)  → DAC pin13 (SCLK)
 * 
 * DAC80508 SPI protocol: 24-bit frames, MSB first
 *   [7-bit addr | R/W] [DATA_HI] [DATA_LO]
 *   CPOL=0 CPHA=1 → SPI mode 1 per datasheet (SCLK idle low, data latched on falling edge)
 */

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
 * @brief Read a DAC register
 * @param reg Register address (0x00-0x0F)
 * @param value Output: 16-bit value read
 * @return ESP_OK on success
 */
esp_err_t dac_read_register(uint8_t reg, uint16_t* value);

/**
 * @brief Output a laser point to the correct DAC channels
 * Maps X→OUT6, Y→OUT7, R→OUT5, G→OUT4, B→OUT3
 * @param point Pointer to laser point
 */
void dac_output_point(const laser_point_t* point);

/**
 * @brief Output a test pattern to verify DAC operation
 * @param pattern_id 0=center, 1=square, 2=circle, 3=full-scale ramp, 4=channel-walk
 */
void dac_test_pattern(uint8_t pattern_id);

/**
 * @brief Set all DAC channels to a specific value (for testing)
 * @param value 16-bit value to write to all channels
 */
void dac_set_all(uint16_t value);

/**
 * @brief Dump all DAC registers to log for debugging
 */
void dac_debug_dump(void);

#ifdef __cplusplus
}
#endif

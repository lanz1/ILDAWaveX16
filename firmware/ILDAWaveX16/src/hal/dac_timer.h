/**
 * @file dac_timer.h
 * @brief Hardware-timed DAC output using GPTimer
 * 
 * This module provides precise, hardware-timed laser point output.
 * It uses a GPTimer ISR to signal a semaphore at the scan rate frequency,
 * and a high-priority task that outputs points via queued SPI transactions.
 * 
 * Replaces the busy-wait approach in dac_engine for better timing precision
 * and reduced CPU usage.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize hardware-timed DAC output system
 * 
 * Initializes:
 *   - Frame buffer
 *   - Base DAC (via dac_init())
 *   - GPTimer for scan rate timing
 *   - SPI device for queued transactions
 *   - Output task on Core 1
 * 
 * @return ESP_OK on success
 */
esp_err_t dac_timer_init(void);

/**
 * @brief Start timed output at current scan rate
 * 
 * Starts the GPTimer which triggers point output at scan_rate frequency.
 * 
 * @return ESP_OK on success
 */
esp_err_t dac_timer_start(void);

/**
 * @brief Stop timed output
 * 
 * Stops the GPTimer. DAC outputs rest point when stopped.
 * 
 * @return ESP_OK on success
 */
esp_err_t dac_timer_stop(void);

/**
 * @brief Set scan rate (points per second)
 * 
 * Updates the GPTimer period. Can be called while running.
 * 
 * @param rate_hz Scan rate in Hz (clamped to SCAN_RATE_MIN_HZ..SCAN_RATE_MAX_HZ)
 * @return ESP_OK on success
 */
esp_err_t dac_timer_set_scan_rate(uint32_t rate_hz);

/**
 * @brief Get current scan rate
 * @return Current scan rate in Hz
 */
uint32_t dac_timer_get_scan_rate(void);

/**
 * @brief Check if timed output is running
 * @return true if running, false if stopped
 */
bool dac_timer_is_running(void);

#ifdef __cplusplus
}
#endif

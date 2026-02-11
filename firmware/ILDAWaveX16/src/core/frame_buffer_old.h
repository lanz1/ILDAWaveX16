/**
 * @file frame_buffer.h
 * @brief Lock-free SPSC ring buffer for laser points
 * 
 * SIMPLIFIED ARCHITECTURE (like j4cDAC):
 *   - Single buffer for both network and DAC ISR
 *   - Network writes via frame_buffer_write()
 *   - GPTimer ISR reads via ISR-safe functions
 */

#pragma once

#include "config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t frame_buffer_init(void);

/**
 * @brief Write batch of points to buffer
 * @return Number of points actually written
 */
size_t frame_buffer_write(const laser_point_t* points, size_t count);

/**
 * @brief Read batch of points from buffer (NOT FOR ISR - use ISR functions below)
 * @param points Output array
 * @param max Maximum number of points to read
 * @return Number of points actually read (0 if empty)
 */
size_t frame_buffer_read(laser_point_t* points, size_t max);

/**
 * @brief Check if count points can fit
 */
bool frame_buffer_can_fit(size_t count);

size_t frame_buffer_level(void);
void frame_buffer_clear(void);

//============================================================================
// ISR-SAFE FUNCTIONS - used directly by GPTimer ISR
//============================================================================

/** @brief Get raw buffer pointer for direct ISR access */
laser_point_t* frame_buffer_get_buffer(void);

/** @brief Get current head index (write position) */
size_t frame_buffer_get_head(void);

/** @brief Get current tail index (read position) */
size_t frame_buffer_get_tail(void);

/** @brief Advance tail by 1 after reading a point */
void frame_buffer_advance_tail(void);

/** @brief Check if buffer has data available */
bool frame_buffer_has_data(void);

/** @brief Get free space in buffer */
size_t frame_buffer_free(void);

#ifdef __cplusplus
}
#endif

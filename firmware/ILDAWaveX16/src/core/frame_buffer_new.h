/**
 * @file frame_buffer.h
 * @brief Simple SPSC ring buffer for laser points
 * 
 * DESIGN: Like j4cDAC - single buffer, no atomics overhead
 * - Producer (network task): frame_buffer_write()
 * - Consumer (GPTimer ISR): frame_buffer_read_one()
 */

#pragma once

#include "config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize buffer (call once at startup)
 */
esp_err_t frame_buffer_init(void);

/**
 * @brief Clear buffer (only when playback stopped!)
 */
void frame_buffer_clear(void);

//=============================================================================
// PRODUCER API (network task)
//=============================================================================

/**
 * @brief Write points to buffer
 * @param points Array of points to write
 * @param count Number of points
 * @return Number of points actually written (may be less if buffer full)
 */
size_t frame_buffer_write(const laser_point_t* points, size_t count);

/**
 * @brief Get free space in buffer
 * @return Number of points that can be written
 */
size_t frame_buffer_free(void);

/**
 * @brief Check if buffer can fit count points
 */
bool frame_buffer_can_fit(size_t count);

//=============================================================================
// CONSUMER API (ISR) - all IRAM_ATTR
//=============================================================================

/**
 * @brief Check if buffer has data (ISR-safe)
 */
bool frame_buffer_has_data(void);

/**
 * @brief Peek at next point without consuming (ISR-safe)
 * @return Pointer to point, or NULL if buffer empty
 */
const laser_point_t* frame_buffer_peek(void);

/**
 * @brief Consume one point after peeking (ISR-safe)
 */
void frame_buffer_consume(void);

/**
 * @brief Read and consume one point (ISR-safe, combined operation)
 * @return Pointer to point (valid until next call), or NULL if empty
 */
const laser_point_t* frame_buffer_read_one(void);

//=============================================================================
// STATUS API
//=============================================================================

/**
 * @brief Get current buffer level (points in buffer)
 */
size_t frame_buffer_level(void);

/**
 * @brief Get underrun count (reads from empty buffer)
 */
uint32_t frame_buffer_get_underruns(void);

/**
 * @brief Get overrun count (writes to full buffer)
 */
uint32_t frame_buffer_get_overruns(void);

/**
 * @brief Reset underrun/overrun counters
 */
void frame_buffer_reset_stats(void);

//=============================================================================
// LEGACY API (for compatibility)
//=============================================================================

/**
 * @brief Read multiple points (NOT for ISR - use frame_buffer_read_one instead)
 */
size_t frame_buffer_read(laser_point_t* points, size_t max);

#ifdef __cplusplus
}
#endif

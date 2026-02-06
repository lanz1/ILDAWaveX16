/**
 * @file frame_buffer.h
 * @brief Ring buffer for laser points (based on Stanley's PointRingBuffer)
 * 
 * Uses portMUX spinlock for thread-safe access.
 * Supports batch read (up to 512 points) for efficient DAC output.
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
 * @brief Read batch of points from buffer
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

#ifdef __cplusplus
}
#endif

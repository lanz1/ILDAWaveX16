#pragma once

#include "config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t frame_buffer_init(void);
size_t frame_buffer_write(const laser_point_t* points, size_t count);
size_t frame_buffer_read(laser_point_t* points, size_t max);
bool frame_buffer_can_fit(size_t count);

size_t frame_buffer_level(void);
void frame_buffer_clear(void);

#ifdef __cplusplus
}
#endif

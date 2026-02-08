/**
 * @file frame_compositor.c
 * @brief Frame processing
 */

#include "frame_compositor.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "COMPOSITOR";

void frame_compositor_init(void) {
    ESP_LOGI(TAG, "Frame compositor initialized (passthrough mode)");
}

size_t frame_compositor_process(const laser_point_t* input, size_t input_count,
                                laser_point_t* output, size_t output_max) {
    if (!input || !output || input_count == 0) return 0;
    
    size_t count = (input_count < output_max) ? input_count : output_max;
    memcpy(output, input, count * sizeof(laser_point_t));
    return count;
}

void frame_compositor_apply_blank_shift(laser_point_t* points, size_t count, int shift) {
    if (!points || count == 0 || shift == 0) return;
    
    if (shift > 0) {
        for (int i = (int)count - 1; i >= shift; i--) {
            points[i].r = points[i - shift].r;
            points[i].g = points[i - shift].g;
            points[i].b = points[i - shift].b;
        }
        for (int i = 0; i < shift && i < (int)count; i++) {
            points[i].r = 0;
            points[i].g = 0;
            points[i].b = 0;
        }
    } else {
        shift = -shift;
        for (size_t i = 0; i < count - shift; i++) {
            points[i].r = points[i + shift].r;
            points[i].g = points[i + shift].g;
            points[i].b = points[i + shift].b;
        }
        for (size_t i = count - shift; i < count; i++) {
            points[i].r = 0;
            points[i].g = 0;
            points[i].b = 0;
        }
    }
}

size_t frame_compositor_insert_blanks(const laser_point_t* input, size_t input_count,
                                       laser_point_t* output, size_t output_max,
                                       int blank_on_points, int blank_off_points) {
    if (!input || !output || input_count == 0) return 0;
    
    size_t out_idx = 0;
    bool prev_blank = true;
    
    for (size_t i = 0; i < input_count && out_idx < output_max; i++) {
        bool curr_blank = (input[i].flags & POINT_FLAG_BLANK) || 
                          (input[i].r == 0 && input[i].g == 0 && input[i].b == 0);
        
        if (prev_blank && !curr_blank) {
            for (int j = 0; j < blank_on_points && out_idx < output_max; j++) {
                output[out_idx] = input[i];
                output[out_idx].r = 0;
                output[out_idx].g = 0;
                output[out_idx].b = 0;
                output[out_idx].flags |= POINT_FLAG_BLANK;
                out_idx++;
            }
        }
        
        if (out_idx < output_max) {
            output[out_idx++] = input[i];
        }
        
        if (!prev_blank && curr_blank) {
            for (int j = 0; j < blank_off_points && out_idx < output_max; j++) {
                output[out_idx] = input[i];
                output[out_idx].flags |= POINT_FLAG_BLANK;
                out_idx++;
            }
        }
        
        prev_blank = curr_blank;
    }
    
    return out_idx;
}

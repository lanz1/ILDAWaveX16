/**
 * @file frame_buffer.c
 * @brief Lock-free SPSC ring buffer for laser points
 */

#include "frame_buffer.h"
#include <string.h>
#include <stdatomic.h>

static laser_point_t buffer[FRAME_BUFFER_SIZE];
static atomic_size_t head = 0;  // Written by producer
static atomic_size_t tail = 0;  // Written by consumer

esp_err_t frame_buffer_init(void)
{
    atomic_store(&head, 0);
    atomic_store(&tail, 0);
    memset(buffer, 0, sizeof(buffer));
    return ESP_OK;
}

size_t frame_buffer_write(const laser_point_t* points, size_t count)
{
    if (!points || count == 0) return 0;
    
    size_t h = atomic_load_explicit(&head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&tail, memory_order_acquire);
    
    size_t written = 0;
    for (size_t i = 0; i < count; i++) {
        size_t next_h = (h + 1) % FRAME_BUFFER_SIZE;
        if (next_h == t) {
            // Buffer full
            break;
        }
        buffer[h] = points[i];
        h = next_h;
        written++;
    }
    
    atomic_store_explicit(&head, h, memory_order_release);
    return written;
}

size_t frame_buffer_read(laser_point_t* point)
{
    if (!point) return 0;
    
    size_t h = atomic_load_explicit(&head, memory_order_acquire);
    size_t t = atomic_load_explicit(&tail, memory_order_relaxed);
    
    if (h == t) {
        point->x = 0;
        point->y = 0;
        point->r = 0;
        point->g = 0;
        point->b = 0;
        point->user1 = 0;
        point->user2 = 0;
        point->flags = POINT_FLAG_BLANK;
        return 0;
    }
    
    *point = buffer[t];
    atomic_store_explicit(&tail, (t + 1) % FRAME_BUFFER_SIZE, memory_order_release);
    return 1;
}

size_t frame_buffer_level(void)
{
    size_t h = atomic_load_explicit(&head, memory_order_acquire);
    size_t t = atomic_load_explicit(&tail, memory_order_acquire);
    
    if (h >= t) {
        return h - t;
    } else {
        return FRAME_BUFFER_SIZE - t + h;
    }
}

void frame_buffer_clear(void)
{
    atomic_store(&head, 0);
    atomic_store(&tail, 0);
}

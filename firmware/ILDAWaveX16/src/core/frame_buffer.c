/**
 * @file frame_buffer.c
 * @brief Lock-free SPSC ring buffer for laser points
 * 
 * Single-Producer Single-Consumer (SPSC) lock-free design:
 *   - Producer (network_task on Core 0): writes via frame_buffer_write()
 *   - Consumer (dac_refill_task on Core 1): reads via frame_buffer_read()
 * 
 * No mutex needed! Uses memory barriers for cross-core synchronization.
 */

#include "frame_buffer.h"
#include <string.h>
#include <stdatomic.h>
#include "esp_attr.h"

static laser_point_t buffer[FRAME_BUFFER_SIZE];

// Atomic indices for lock-free operation
// head: written by producer, read by consumer
// tail: written by consumer, read by producer
static atomic_size_t head = 0;
static atomic_size_t tail = 0;

esp_err_t frame_buffer_init(void)
{
    atomic_store_explicit(&head, 0, memory_order_relaxed);
    atomic_store_explicit(&tail, 0, memory_order_relaxed);
    memset(buffer, 0, sizeof(buffer));
    return ESP_OK;
}

// Producer function - called from network_task (Core 0)
size_t frame_buffer_write(const laser_point_t* points, size_t count)
{
    if (!points || count == 0) return 0;

    size_t written = 0;
    size_t h = atomic_load_explicit(&head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&tail, memory_order_acquire);  // Sync with consumer
    
    for (size_t i = 0; i < count; i++) {
        size_t next = (h + 1) % FRAME_BUFFER_SIZE;
        if (next == t) break;  // Buffer full
        
        buffer[h] = points[i];
        h = next;
        written++;
    }
    
    // Publish new head position to consumer
    atomic_store_explicit(&head, h, memory_order_release);
    return written;
}

// Consumer function - called from dac_refill_task (Core 1)
size_t frame_buffer_read(laser_point_t* points, size_t max)
{
    if (!points || max == 0) return 0;

    size_t count = 0;
    size_t t = atomic_load_explicit(&tail, memory_order_relaxed);
    size_t h = atomic_load_explicit(&head, memory_order_acquire);  // Sync with producer
    
    while (count < max && t != h) {
        points[count] = buffer[t];
        t = (t + 1) % FRAME_BUFFER_SIZE;
        count++;
    }
    
    // Publish new tail position to producer
    atomic_store_explicit(&tail, t, memory_order_release);
    return count;
}

bool frame_buffer_can_fit(size_t count)
{
    size_t h = atomic_load_explicit(&head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&tail, memory_order_acquire);
    
    size_t free_space;
    if (h >= t)
        free_space = FRAME_BUFFER_SIZE - (h - t) - 1;
    else
        free_space = t - h - 1;
    
    return count <= free_space;
}

size_t frame_buffer_level(void)
{
    size_t h = atomic_load_explicit(&head, memory_order_acquire);
    size_t t = atomic_load_explicit(&tail, memory_order_acquire);

    if (h >= t)
        return h - t;
    else
        return FRAME_BUFFER_SIZE - t + h;
}

void frame_buffer_clear(void)
{
    // Reset both to 0 - safe because clear is called when system is idle
    atomic_store_explicit(&tail, 0, memory_order_relaxed);
    atomic_store_explicit(&head, 0, memory_order_release);
}

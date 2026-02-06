/**
 * @file frame_buffer.c
 * @brief Ring buffer for laser points (based on Stanley's PointRingBuffer)
 * 
 * Uses portMUX spinlock for thread-safe access.
 * Supports batch read (up to 512 points) for efficient DAC output.
 */

#include "frame_buffer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static laser_point_t buffer[FRAME_BUFFER_SIZE];
static volatile size_t head = 0;
static volatile size_t tail = 0;
static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t frame_buffer_init(void)
{
    portENTER_CRITICAL(&spinlock);
    head = 0;
    tail = 0;
    portEXIT_CRITICAL(&spinlock);
    memset(buffer, 0, sizeof(buffer));
    return ESP_OK;
}

size_t frame_buffer_write(const laser_point_t* points, size_t count)
{
    if (!points || count == 0) return 0;

    size_t written = 0;
    portENTER_CRITICAL(&spinlock);
    for (size_t i = 0; i < count; i++) {
        size_t next = (head + 1) % FRAME_BUFFER_SIZE;
        if (next == tail) break;  // full
        buffer[head] = points[i];
        head = next;
        written++;
    }
    portEXIT_CRITICAL(&spinlock);
    return written;
}

size_t frame_buffer_read(laser_point_t* points, size_t max)
{
    if (!points || max == 0) return 0;

    size_t count = 0;
    portENTER_CRITICAL(&spinlock);
    while (count < max && tail != head) {
        points[count] = buffer[tail];
        tail = (tail + 1) % FRAME_BUFFER_SIZE;
        count++;
    }
    portEXIT_CRITICAL(&spinlock);
    return count;
}

bool frame_buffer_can_fit(size_t count)
{
    size_t free_space;
    portENTER_CRITICAL(&spinlock);
    if (head >= tail)
        free_space = FRAME_BUFFER_SIZE - (head - tail) - 1;
    else
        free_space = tail - head - 1;
    portEXIT_CRITICAL(&spinlock);
    return count <= free_space;
}

size_t frame_buffer_level(void)
{
    size_t h, t;
    portENTER_CRITICAL(&spinlock);
    h = head;
    t = tail;
    portEXIT_CRITICAL(&spinlock);

    if (h >= t)
        return h - t;
    else
        return FRAME_BUFFER_SIZE - t + h;
}

void frame_buffer_clear(void)
{
    portENTER_CRITICAL(&spinlock);
    head = 0;
    tail = 0;
    portEXIT_CRITICAL(&spinlock);
}

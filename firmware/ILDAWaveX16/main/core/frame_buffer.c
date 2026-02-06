/**
 * @file frame_buffer.c
 * @brief Ring buffer for laser points (based on Stanley's PointRingBuffer)
 * 
 * Uses FreeRTOS mutex for thread-safe access with task yielding.
 * Supports batch read (up to 128 points) for efficient DAC output.
 */

#include "frame_buffer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static laser_point_t buffer[FRAME_BUFFER_SIZE];
static volatile size_t head = 0;
static volatile size_t tail = 0;
static SemaphoreHandle_t mutex = NULL;

esp_err_t frame_buffer_init(void)
{
    if (!mutex) {
        mutex = xSemaphoreCreateMutex();
        if (!mutex) return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    head = 0;
    tail = 0;
    xSemaphoreGive(mutex);
    memset(buffer, 0, sizeof(buffer));
    return ESP_OK;
}

size_t frame_buffer_write(const laser_point_t* points, size_t count)
{
    if (!points || count == 0 || !mutex) return 0;

    size_t written = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (size_t i = 0; i < count; i++) {
        size_t next = (head + 1) % FRAME_BUFFER_SIZE;
        if (next == tail) break;  // full
        buffer[head] = points[i];
        head = next;
        written++;
    }
    xSemaphoreGive(mutex);
    return written;
}

size_t frame_buffer_read(laser_point_t* points, size_t max)
{
    if (!points || max == 0 || !mutex) return 0;

    size_t count = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    while (count < max && tail != head) {
        points[count] = buffer[tail];
        tail = (tail + 1) % FRAME_BUFFER_SIZE;
        count++;
    }
    xSemaphoreGive(mutex);
    return count;
}

bool frame_buffer_can_fit(size_t count)
{
    if (!mutex) return false;
    size_t free_space;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (head >= tail)
        free_space = FRAME_BUFFER_SIZE - (head - tail) - 1;
    else
        free_space = tail - head - 1;
    xSemaphoreGive(mutex);
    return count <= free_space;
}

size_t frame_buffer_level(void)
{
    if (!mutex) return 0;
    size_t h, t;
    xSemaphoreTake(mutex, portMAX_DELAY);
    h = head;
    t = tail;
    xSemaphoreGive(mutex);

    if (h >= t)
        return h - t;
    else
        return FRAME_BUFFER_SIZE - t + h;
}

void frame_buffer_clear(void)
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    head = 0;
    tail = 0;
    xSemaphoreGive(mutex);
}

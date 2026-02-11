/**
 * @file frame_buffer.c
 * @brief Simple SPSC ring buffer for laser points
 * 
 * DESIGN PRINCIPLES (like j4cDAC):
 *   - Single buffer shared between network and DAC ISR
 *   - NO atomics in ISR path - just volatile + compiler barriers
 *   - Power-of-2 size for fast modulo (mask instead of division)
 *   - Producer (network): writes head
 *   - Consumer (ISR): writes tail
 *   - No conflicts: each side owns one index
 */

#include "frame_buffer.h"
#include <string.h>
#include "esp_attr.h"

// Power-of-2 buffer for fast masking
#define BUFFER_MASK (FRAME_BUFFER_SIZE - 1)

// Verify FRAME_BUFFER_SIZE is power of 2
_Static_assert((FRAME_BUFFER_SIZE & (FRAME_BUFFER_SIZE - 1)) == 0, 
               "FRAME_BUFFER_SIZE must be power of 2");

// Buffer and indices
static laser_point_t s_buffer[FRAME_BUFFER_SIZE] __attribute__((aligned(4)));
static volatile size_t s_head = 0;  // Write position (owned by producer/network)
static volatile size_t s_tail = 0;  // Read position (owned by consumer/ISR)

// Stats
static volatile uint32_t s_underruns = 0;
static volatile uint32_t s_overruns = 0;

esp_err_t frame_buffer_init(void)
{
    s_head = 0;
    s_tail = 0;
    s_underruns = 0;
    s_overruns = 0;
    memset(s_buffer, 0, sizeof(s_buffer));
    return ESP_OK;
}

void frame_buffer_clear(void)
{
    // Safe only when playback stopped
    s_tail = 0;
    s_head = 0;
}

//=============================================================================
// PRODUCER API (network task) - writes to head
//=============================================================================

size_t frame_buffer_write(const laser_point_t* points, size_t count)
{
    if (!points || count == 0) return 0;
    
    size_t written = 0;
    size_t h = s_head;
    size_t t = s_tail;  // Read once
    
    for (size_t i = 0; i < count; i++) {
        size_t next = (h + 1) & BUFFER_MASK;
        if (next == t) {
            // Buffer full
            s_overruns++;
            break;
        }
        s_buffer[h] = points[i];
        h = next;
        written++;
    }
    
    // Memory barrier to ensure buffer writes complete before head update
    __asm__ __volatile__("" ::: "memory");
    s_head = h;
    
    return written;
}

size_t frame_buffer_free(void)
{
    size_t h = s_head;
    size_t t = s_tail;
    
    // Free space = BUFFER_SIZE - 1 - used
    // Used = (h - t) & MASK, but we need actual free slots
    if (h >= t) {
        return FRAME_BUFFER_SIZE - 1 - (h - t);
    } else {
        return t - h - 1;
    }
}

bool frame_buffer_can_fit(size_t count)
{
    return frame_buffer_free() >= count;
}

//=============================================================================
// CONSUMER API (ISR) - reads from tail
//=============================================================================

// Check if buffer has data - IRAM for ISR
bool IRAM_ATTR frame_buffer_has_data(void)
{
    return s_head != s_tail;
}

// Get pointer to next point without removing it - IRAM for ISR
const laser_point_t* IRAM_ATTR frame_buffer_peek(void)
{
    if (s_head == s_tail) {
        s_underruns++;
        return NULL;
    }
    return &s_buffer[s_tail];
}

// Consume one point (advance tail) - IRAM for ISR
void IRAM_ATTR frame_buffer_consume(void)
{
    // Compiler barrier before advancing tail
    __asm__ __volatile__("" ::: "memory");
    s_tail = (s_tail + 1) & BUFFER_MASK;
}

// Combined peek+consume for efficiency - IRAM for ISR
const laser_point_t* IRAM_ATTR frame_buffer_read_one(void)
{
    if (s_head == s_tail) {
        s_underruns++;
        return NULL;
    }
    
    const laser_point_t* p = &s_buffer[s_tail];
    __asm__ __volatile__("" ::: "memory");
    s_tail = (s_tail + 1) & BUFFER_MASK;
    return p;
}

//=============================================================================
// STATUS API
//=============================================================================

size_t frame_buffer_level(void)
{
    size_t h = s_head;
    size_t t = s_tail;
    
    if (h >= t) {
        return h - t;
    } else {
        return FRAME_BUFFER_SIZE - t + h;
    }
}

uint32_t frame_buffer_get_underruns(void)
{
    return s_underruns;
}

uint32_t frame_buffer_get_overruns(void)
{
    return s_overruns;
}

void frame_buffer_reset_stats(void)
{
    s_underruns = 0;
    s_overruns = 0;
}

//=============================================================================
// LEGACY API (for compatibility)
//=============================================================================

size_t frame_buffer_read(laser_point_t* points, size_t max)
{
    if (!points || max == 0) return 0;
    
    size_t count = 0;
    while (count < max && s_head != s_tail) {
        points[count] = s_buffer[s_tail];
        s_tail = (s_tail + 1) & BUFFER_MASK;
        count++;
    }
    return count;
}

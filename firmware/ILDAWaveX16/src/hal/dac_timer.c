/**
 * @file dac_timer.c
 * @brief Hardware-timed DAC output using GPTimer ISR per-point
 * 
 * Architecture (ISR-per-point):
 *   GPTimer fires at scan_rate Hz - ISR writes point directly to DAC
 *   Background task refills ISR buffer from frame_buffer
 *   
 * Benefits:
 *   - Hardware timer guarantees exact scan rate (no drift!)
 *   - ISR uses direct SPI (~6us) - fits in 22us @ 45kpps
 *   - Zero jitter between points
 */

#include "dac_timer.h"
#include "dac80508.h"
#include "core/frame_buffer.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <string.h>

static const char* TAG = "DAC_TIMER";

// Configuration
#define ISR_BUFFER_SIZE 512
#define REFILL_THRESHOLD 64      // Refill more often with smaller batches
#define REFILL_MAX_BATCH 256     // But read up to this many at once

// Hardware resources
static gptimer_handle_t s_timer = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_playback_active = false;  // Gate: only drain frame_buffer when true
static volatile uint32_t s_scan_rate = SCAN_RATE_DEFAULT_HZ;

// ISR ring buffer - written by task, read by ISR
static laser_point_t s_isr_buffer[ISR_BUFFER_SIZE];
static volatile size_t s_isr_head = 0;
static volatile size_t s_isr_tail = 0;
static volatile uint32_t s_isr_underruns = 0;

// Timing statistics
static volatile uint32_t s_points_output = 0;
static volatile int64_t s_last_log_time = 0;

// Rest point (blanked, centered) - used for stop and underruns
static const laser_point_t REST_POINT = {
    .x = 0, .y = 0, .r = 0, .g = 0, .b = 0,
    .user1 = 0, .user2 = 0, .flags = POINT_FLAG_BLANK
};

// ISR buffer helpers (lock-free single producer/consumer)
static inline size_t IRAM_ATTR isr_buffer_count(void) {
    size_t h = s_isr_head;
    size_t t = s_isr_tail;
    if (h >= t) return h - t;
    return ISR_BUFFER_SIZE - t + h;
}

static inline size_t isr_buffer_free(void) {
    return ISR_BUFFER_SIZE - 1 - isr_buffer_count();
}

// GPTimer ISR callback - outputs ONE point per call
static bool IRAM_ATTR timer_isr_callback(gptimer_handle_t timer, 
                                          const gptimer_alarm_event_data_t *edata, 
                                          void *user_ctx)
{
    if (!s_running) {
        return false;
    }
    
    // Check if we have data
    if (s_isr_tail == s_isr_head) {
        // Buffer empty - output rest point
        dac_output_point(&REST_POINT);
        s_isr_underruns++;
        return false;
    }
    
    // Get next point
    const laser_point_t* point = &s_isr_buffer[s_isr_tail];
    
    // Output point (uses direct SPI - ~6us)
    dac_output_point(point);
    
    // Advance tail
    s_isr_tail = (s_isr_tail + 1) % ISR_BUFFER_SIZE;
    s_points_output++;
    
    return false;
}

// Buffer refill task - keeps ISR buffer full
// CRITICAL: Must be very fast - no delays when buffer needs filling!
static void dac_refill_task(void* arg)
{
    ESP_LOGI(TAG, "Refill task started on Core %d", xPortGetCoreID());
    
    static laser_point_t temp_batch[REFILL_MAX_BATCH];
    
    s_last_log_time = esp_timer_get_time();
    uint32_t last_underruns = 0;
    uint32_t idle_loops = 0;
    
    while (1) {
        if (!s_running) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // GATE: Only drain frame_buffer into ISR buffer when playback is active.
        // During PREPARED state, the client is filling the buffer — we must NOT
        // consume those points or the client can never accumulate enough to start.
        if (!s_playback_active) {
            idle_loops++;
            if (idle_loops > 100) {
                vTaskDelay(1);
                idle_loops = 0;
            } else {
                taskYIELD();
            }
            
            // Still log stats
            int64_t now_idle = esp_timer_get_time();
            if (now_idle - s_last_log_time >= 1000000) {
                s_points_output = 0;
                last_underruns = s_isr_underruns;
                s_last_log_time = now_idle;
            }
            continue;
        }
        
        // Check if ISR buffer needs refill
        size_t free_space = isr_buffer_free();
        
        if (free_space >= REFILL_THRESHOLD) {
            // Read as much as we can fit (up to REFILL_MAX_BATCH)
            size_t to_read = (free_space < REFILL_MAX_BATCH) ? free_space : REFILL_MAX_BATCH;
            size_t n = frame_buffer_read(temp_batch, to_read);
            
            if (n > 0) {
                // Copy to ISR buffer
                for (size_t i = 0; i < n; i++) {
                    s_isr_buffer[s_isr_head] = temp_batch[i];
                    s_isr_head = (s_isr_head + 1) % ISR_BUFFER_SIZE;
                }
                idle_loops = 0;  // Reset idle counter
            } else {
                // No data available from frame_buffer
                idle_loops++;
            }
        } else {
            // Buffer is full enough
            idle_loops++;
        }
        
        // Log stats every second
        int64_t now = esp_timer_get_time();
        if (now - s_last_log_time >= 1000000) {
            uint32_t pts = s_points_output;
            uint32_t underruns = s_isr_underruns - last_underruns;
            size_t buf_level = isr_buffer_count();
            
            uint32_t expected_pts = s_scan_rate;
            int32_t error_ppm = (expected_pts > 0) ? 
                (int32_t)(((int64_t)pts - expected_pts) * 1000000 / expected_pts) : 0;
            
            ESP_LOGI(TAG, "ISR: pts/s=%"PRIu32" expected=%"PRIu32" err=%+"PRId32"ppm | buf=%zu underruns=%"PRIu32,
                     pts, expected_pts, error_ppm, buf_level, underruns);
            
            s_points_output = 0;
            last_underruns = s_isr_underruns;
            s_last_log_time = now;
        }
        
        // Yield strategy: only delay if truly idle for many loops
        // This keeps the task responsive while not hogging CPU when no data
        if (idle_loops > 1000) {
            vTaskDelay(1);  // Only delay after extended idle period
            idle_loops = 0;
        } else {
            taskYIELD();    // Let other tasks run but come back ASAP
        }
    }
}

// Update timer period based on scan rate
static esp_err_t update_timer_period(void)
{
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    
    // Timer period = time for ONE point
    uint64_t point_period_us = 1000000 / s_scan_rate;
    
    if (point_period_us < 10) {
        ESP_LOGW(TAG, "Scan rate too high, clamping period to 10us");
        point_period_us = 10;
    }
    
    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = point_period_us,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    
    ESP_LOGI(TAG, "Timer period: %"PRIu64"us (%"PRIu32" pps)", point_period_us, s_scan_rate);
    
    return gptimer_set_alarm_action(s_timer, &alarm_cfg);
}

// Public API

esp_err_t dac_timer_init(void)
{
    ESP_LOGI(TAG, "Initializing ISR-per-point DAC output");
    
    esp_err_t ret = frame_buffer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init frame buffer");
        return ret;
    }
    
    ret = dac_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init DAC");
        return ret;
    }
    
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 1,
    };
    
    ret = gptimer_new_timer(&timer_cfg, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_isr_callback,
    };
    ret = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register timer callback");
        return ret;
    }
    
    ret = gptimer_enable(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer");
        return ret;
    }
    
    // Pin refill task to Core 1 (same as timer ISR) with high priority
    // This avoids contention with WiFi stack on Core 0
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        dac_refill_task,
        "dac_refill",
        4096,
        NULL,
        configMAX_PRIORITIES - 2,  // Very high priority
        &s_task,
        1  // Core 1 - away from WiFi
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create refill task");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Initialized: ISR buffer=%d, refill threshold=%d", ISR_BUFFER_SIZE, REFILL_THRESHOLD);
    return ESP_OK;
}

esp_err_t dac_timer_start(void)
{
    if (s_running) return ESP_OK;
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    
    ESP_LOGI(TAG, "Starting at %lu pps", (unsigned long)s_scan_rate);
    
    s_isr_head = 0;
    s_isr_tail = 0;
    s_isr_underruns = 0;
    s_points_output = 0;
    
    esp_err_t ret = update_timer_period();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timer period");
        return ret;
    }
    
    gptimer_set_raw_count(s_timer, 0);
    s_running = true;
    
    ret = gptimer_start(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start timer");
        s_running = false;
        return ret;
    }
    
    ESP_LOGI(TAG, "Started");
    return ESP_OK;
}

esp_err_t dac_timer_stop(void)
{
    if (!s_running) return ESP_OK;
    
    ESP_LOGI(TAG, "Stopping");
    s_running = false;
    
    if (s_timer) {
        gptimer_stop(s_timer);
    }
    
    dac_output_point(&REST_POINT);
    return ESP_OK;
}

esp_err_t dac_timer_set_scan_rate(uint32_t rate_hz)
{
    if (rate_hz < SCAN_RATE_MIN_HZ) rate_hz = SCAN_RATE_MIN_HZ;
    if (rate_hz > SCAN_RATE_MAX_HZ) rate_hz = SCAN_RATE_MAX_HZ;
    
    uint32_t old_rate = s_scan_rate;
    s_scan_rate = rate_hz;
    g_config.scan_rate_hz = rate_hz;
    
    ESP_LOGI(TAG, "Scan rate: %lu -> %lu pps", (unsigned long)old_rate, (unsigned long)rate_hz);
    
    if (s_running && s_timer) {
        gptimer_stop(s_timer);
        gptimer_set_raw_count(s_timer, 0);
        update_timer_period();
        gptimer_start(s_timer);
    }
    
    return ESP_OK;
}

uint32_t dac_timer_get_scan_rate(void)
{
    return s_scan_rate;
}

bool dac_timer_is_running(void)
{
    return s_running;
}

void dac_timer_set_playback_active(bool active)
{
    if (active && !s_playback_active) {
        // Transitioning to active: clear ISR buffer for clean start
        s_isr_head = 0;
        s_isr_tail = 0;
    }
    s_playback_active = active;
}

bool dac_timer_is_playback_active(void)
{
    return s_playback_active;
}

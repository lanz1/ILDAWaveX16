/**
 * @file dac_timer.c
 * @brief Hardware-timed DAC output using GPTimer ISR per-point
 * 
 * SIMPLIFIED ARCHITECTURE (like j4cDAC):
 *   - ONE buffer: frame_buffer (8192 points)
 *   - GPTimer ISR reads DIRECTLY from frame_buffer
 *   - No intermediate ISR buffer, no refill task
 *   
 * Benefits:
 *   - Simpler code, fewer moving parts
 *   - buffer_fullness reported to client matches actual buffer state
 *   - Lower latency - no double-buffering overhead
 *   - No watchdog issues from busy-loop tasks
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
#include <inttypes.h>
#include <string.h>

static const char* TAG = "DAC_TIMER";

// Hardware resources
static gptimer_handle_t s_timer = NULL;
static volatile bool s_running = false;
static volatile uint32_t s_scan_rate = SCAN_RATE_DEFAULT_HZ;

// Statistics
static volatile uint32_t s_points_output = 0;
static volatile uint32_t s_underruns = 0;

// Rest point (blanked, centered) - used for stop and underruns
static const laser_point_t REST_POINT = {
    .x = 0, .y = 0, .r = 0, .g = 0, .b = 0,
    .user1 = 0, .user2 = 0, .flags = POINT_FLAG_BLANK
};

// GPTimer ISR callback - outputs ONE point per call
// Reads DIRECTLY from frame_buffer - no intermediate buffer!
static bool IRAM_ATTR timer_isr_callback(gptimer_handle_t timer, 
                                          const gptimer_alarm_event_data_t *edata, 
                                          void *user_ctx)
{
    if (!s_running) {
        return false;
    }
    
    // Check if frame_buffer has data
    if (!frame_buffer_has_data()) {
        // Buffer empty - output rest point
        dac_output_point(&REST_POINT);
        s_underruns++;
        return false;
    }
    
    // Read point directly from frame_buffer
    laser_point_t* buf = frame_buffer_get_buffer();
    size_t t = frame_buffer_get_tail();
    const laser_point_t* point = &buf[t];
    
    // Output point (uses direct SPI - ~6us)
    dac_output_point(point);
    
    // Advance tail
    frame_buffer_advance_tail();
    s_points_output++;
    
    return false;
}

// Background task for logging only (no refill needed anymore!)
static void dac_stats_task(void* arg)
{
    ESP_LOGI(TAG, "Stats task started on Core %d", xPortGetCoreID());
    
    uint32_t last_underruns = 0;
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // Log every second
        
        if (!s_running) continue;
        
        uint32_t pts = s_points_output;
        uint32_t underruns = s_underruns - last_underruns;
        size_t buf_level = frame_buffer_level();
        
        uint32_t expected_pts = s_scan_rate;
        int32_t error_ppm = (expected_pts > 0) ? 
            (int32_t)(((int64_t)pts - expected_pts) * 1000000 / expected_pts) : 0;
        
        ESP_LOGI(TAG, "pts=%"PRIu32" (%+"PRId32"ppm) | BUF=%zu/%d | under=%"PRIu32,
                 pts, error_ppm, buf_level, FRAME_BUFFER_SIZE, underruns);
        
        s_points_output = 0;
        last_underruns = s_underruns;
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
    ESP_LOGI(TAG, "Initializing DIRECT DAC output (no intermediate buffer)");
    
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
        .resolution_hz = 1000000,  // 1MHz = 1us resolution
        .intr_priority = 1,        // Highest priority
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
    
    // Stats task - low priority, just for logging (not critical!)
    xTaskCreatePinnedToCore(
        dac_stats_task,
        "dac_stats",
        4096,  // Need more stack for ESP_LOGI formatting
        NULL,
        1,  // Low priority - just logging
        NULL,
        1   // Core 1
    );
    
    ESP_LOGI(TAG, "Initialized: buffer=%d points, ISR reads directly from frame_buffer", FRAME_BUFFER_SIZE);
    return ESP_OK;
}

esp_err_t dac_timer_start(void)
{
    if (s_running) return ESP_OK;
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    
    ESP_LOGI(TAG, "Starting at %lu pps", (unsigned long)s_scan_rate);
    
    s_underruns = 0;
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

// Legacy function - now just returns frame_buffer_level()
size_t dac_timer_isr_buffer_count(void)
{
    return frame_buffer_level();
}

/**
 * @file dac_timer.c
 * @brief Hardware-timed DAC output - GPTimer ISR per point
 * 
 * SIMPLIFIED ARCHITECTURE:
 *   - GPTimer fires at scan_rate Hz
 *   - ISR reads ONE point from frame_buffer, writes to DAC
 *   - Everything on Core 0 (same as network) - no cross-core sync needed
 *   - ISR has highest priority - preempts network task
 *   
 * TIMING @ 45kpps:
 *   - Period: 22µs per point
 *   - ISR time: ~8-10µs (read + SPI write)
 *   - Margin: 12-14µs for network/other
 */

#include "dac_timer.h"
#include "dac80508.h"
#include "core/frame_buffer.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gptimer.h"
#include <inttypes.h>

static const char* TAG = "DAC";

// Hardware
static gptimer_handle_t s_timer = NULL;
static volatile bool s_running = false;
static volatile uint32_t s_scan_rate = SCAN_RATE_DEFAULT_HZ;

// Statistics
static volatile uint32_t s_points_output = 0;
static volatile uint32_t s_underruns = 0;
static int64_t s_last_stats_time = 0;
static uint32_t s_last_underruns = 0;

// Blanked rest point
static const laser_point_t REST_POINT = {
    .x = 0, .y = 0, .r = 0, .g = 0, .b = 0,
    .user1 = 0, .user2 = 0, .flags = POINT_FLAG_BLANK
};

//=============================================================================
// GPTimer ISR - runs at scan_rate Hz
//=============================================================================

static bool IRAM_ATTR timer_isr_callback(gptimer_handle_t timer,
                                          const gptimer_alarm_event_data_t *edata,
                                          void *user_ctx)
{
    if (!s_running) return false;
    
    // Read one point from buffer
    const laser_point_t* p = frame_buffer_read_one();
    
    if (p) {
        dac_output_point(p);
        s_points_output++;
    } else {
        // Underrun - output blanked center
        dac_output_point(&REST_POINT);
        s_underruns++;
    }
    
    return false;  // No task switch needed
}

//=============================================================================
// Timer Configuration
//=============================================================================

static esp_err_t configure_timer_period(void)
{
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    
    uint64_t period_us = 1000000 / s_scan_rate;
    if (period_us < 10) period_us = 10;  // Min 100kpps
    
    gptimer_alarm_config_t alarm_cfg = {
        .alarm_count = period_us,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    
    return gptimer_set_alarm_action(s_timer, &alarm_cfg);
}

//=============================================================================
// Public API
//=============================================================================

esp_err_t dac_timer_init(void)
{
    ESP_LOGI(TAG, "Initializing DAC timer (direct ISR output)");
    
    // Init buffer
    esp_err_t ret = frame_buffer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init frame buffer");
        return ret;
    }
    
    // Init DAC hardware
    ret = dac_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init DAC");
        return ret;
    }
    
    // Create GPTimer - 1MHz resolution (1µs ticks)
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 1,  // Highest priority
    };
    
    ret = gptimer_new_timer(&timer_cfg, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register ISR callback
    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_isr_callback,
    };
    ret = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callback");
        return ret;
    }
    
    ret = gptimer_enable(s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable timer");
        return ret;
    }
    
    ESP_LOGI(TAG, "DAC timer ready (buffer=%d pts)", FRAME_BUFFER_SIZE);
    return ESP_OK;
}

esp_err_t dac_timer_start(void)
{
    if (s_running) return ESP_OK;
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    
    ESP_LOGI(TAG, "Starting at %lu pps", (unsigned long)s_scan_rate);
    
    // Reset stats
    s_points_output = 0;
    s_underruns = 0;
    s_last_stats_time = esp_timer_get_time();
    s_last_underruns = 0;
    frame_buffer_reset_stats();
    
    // Configure period and start
    configure_timer_period();
    gptimer_set_raw_count(s_timer, 0);
    s_running = true;
    
    esp_err_t ret = gptimer_start(s_timer);
    if (ret != ESP_OK) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to start timer");
        return ret;
    }
    
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
    
    // Output blank point
    dac_output_point(&REST_POINT);
    return ESP_OK;
}

esp_err_t dac_timer_set_scan_rate(uint32_t rate_hz)
{
    if (rate_hz < SCAN_RATE_MIN_HZ) rate_hz = SCAN_RATE_MIN_HZ;
    if (rate_hz > SCAN_RATE_MAX_HZ) rate_hz = SCAN_RATE_MAX_HZ;
    
    if (rate_hz == s_scan_rate) return ESP_OK;
    
    uint32_t old = s_scan_rate;
    s_scan_rate = rate_hz;
    g_config.scan_rate_hz = rate_hz;
    
    ESP_LOGI(TAG, "Rate: %lu -> %lu pps", (unsigned long)old, (unsigned long)rate_hz);
    
    if (s_running && s_timer) {
        // Reconfigure on the fly
        gptimer_stop(s_timer);
        configure_timer_period();
        gptimer_set_raw_count(s_timer, 0);
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

//=============================================================================
// Statistics (call from main loop, not ISR)
//=============================================================================

void dac_timer_print_stats(void)
{
    if (!s_running) return;
    
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - s_last_stats_time;
    
    if (elapsed >= 1000000) {  // Every 1 second
        uint32_t pts = s_points_output;
        uint32_t under = s_underruns - s_last_underruns;
        size_t level = frame_buffer_level();
        
        int32_t ppm = (s_scan_rate > 0) ? 
            (int32_t)(((int64_t)pts - s_scan_rate) * 1000000 / s_scan_rate) : 0;
        
        ESP_LOGI(TAG, "pts=%lu (%+ldppm) buf=%zu/%d under=%lu",
                 (unsigned long)pts, (long)ppm, level, FRAME_BUFFER_SIZE, 
                 (unsigned long)under);
        
        s_points_output = 0;
        s_last_underruns = s_underruns;
        s_last_stats_time = now;
    }
}

uint32_t dac_timer_get_underruns(void)
{
    return s_underruns;
}

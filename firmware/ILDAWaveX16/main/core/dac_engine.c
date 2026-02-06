/**
 * @file dac_engine.c
 * @brief DAC output engine (optimized with batch read and permanent SPI lock)
 */

#include "dac_engine.h"
#include "frame_buffer.h"
#include "hal/dac80508.h"
#include "config.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "DAC";

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile uint32_t s_scan_rate = SCAN_RATE_DEFAULT_HZ;

#define DAC_BATCH_SIZE 256  // Increased for less lock contention

static const laser_point_t REST_POINT = {
    .x = 0, .y = 0, .r = 0, .g = 0, .b = 0,
    .user1 = 0, .user2 = 0, .flags = POINT_FLAG_BLANK
};

static void dac_task(void* arg) {
    ESP_LOGI(TAG, "Task on Core %d", xPortGetCoreID());
    
    laser_point_t batch[DAC_BATCH_SIZE];
    int64_t next_point_time = esp_timer_get_time();
    uint32_t period_us;
    
    // Timing measurements
    uint64_t total_output_time = 0;
    uint32_t point_count = 0;
    uint32_t last_report_time = 0;
    
    while (1) {
        if (!s_running) {
            dac_output_point((laser_point_t*)&REST_POINT);
            vTaskDelay(pdMS_TO_TICKS(10));
            next_point_time = esp_timer_get_time();
            continue;
        }
        
        period_us = 1000000 / s_scan_rate;
        
        // Batch read from buffer
        size_t n = frame_buffer_read(batch, DAC_BATCH_SIZE);
        
        if (n == 0) {
            // Buffer empty - output rest point, yield
            dac_output_point((laser_point_t*)&REST_POINT);
            vTaskDelay(1);
            next_point_time = esp_timer_get_time();
            continue;
        }
        
        // Output each point in batch at precise scan rate
        for (size_t i = 0; i < n; i++) {
            // Wait until next point time
            int64_t wait = next_point_time - esp_timer_get_time();
            if (wait > 1000) {
                vTaskDelay(1);
            }
            while (esp_timer_get_time() < next_point_time) {
                // busy-wait for precise timing
            }
            
            // Measure SPI output time
            uint64_t start = esp_timer_get_time();
            dac_output_point(&batch[i]);
            uint64_t end = esp_timer_get_time();
            
            total_output_time += (end - start);
            point_count++;
            
            next_point_time += period_us;
            
            // Prevent drift
            int64_t now = esp_timer_get_time();
            if (next_point_time < now - 1000) {
                next_point_time = now;
            }
        }
        
        // Yield after batch to prevent watchdog timeout
        taskYIELD();
        
        // Report timing stats every second
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - last_report_time >= 1000) {
            if (point_count > 0) {
                ESP_LOGI(TAG, "Avg SPI time: %llu us/pt | Points: %lu | Buffer: %zu",
                         total_output_time / point_count, point_count, frame_buffer_level());
                total_output_time = 0;
                point_count = 0;
            }
            last_report_time = now_ms;
        }
    }
}

esp_err_t dac_engine_init(void) {
    ESP_LOGI(TAG, "Init at %lu Hz", (unsigned long)s_scan_rate);

    esp_err_t ret = frame_buffer_init();
    if (ret != ESP_OK) return ret;

    ret = dac_init();
    if (ret != ESP_OK) return ret;

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        dac_task,
        "dac",
        8192,  // Increased for 256pt batch (3KB) + function calls
        NULL,
        configMAX_PRIORITIES - 1,
        &s_task,
        1
    );

    if (task_ret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Ready");
    return ESP_OK;
}

esp_err_t dac_engine_start(void) {
    if (s_running) return ESP_OK;
    ESP_LOGI(TAG, "Start at %lu pps", (unsigned long)s_scan_rate);
    s_running = true;
    return ESP_OK;
}

esp_err_t dac_engine_stop(void) {
    if (!s_running) return ESP_OK;
    s_running = false;
    return ESP_OK;
}

bool dac_engine_is_running(void) {
    return s_running;
}

esp_err_t dac_engine_set_scan_rate(uint32_t rate_hz) {
    if (rate_hz < SCAN_RATE_MIN_HZ) rate_hz = SCAN_RATE_MIN_HZ;
    if (rate_hz > SCAN_RATE_MAX_HZ) rate_hz = SCAN_RATE_MAX_HZ;

    s_scan_rate = rate_hz;
    g_config.scan_rate_hz = rate_hz;
    
    ESP_LOGI(TAG, "Rate set to %lu pps", (unsigned long)rate_hz);
    return ESP_OK;
}

uint32_t dac_engine_get_scan_rate(void) {
    return s_scan_rate;
}

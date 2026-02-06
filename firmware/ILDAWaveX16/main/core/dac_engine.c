/**
 * @file dac_engine.c
 * @brief DAC output engine
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

static laser_point_t s_last_point = {0};
static bool s_has_last = false;

static void dac_task(void* arg) {
    ESP_LOGI(TAG, "Task on Core %d", xPortGetCoreID());
    
    laser_point_t pt;
    int64_t next_point_time = esp_timer_get_time();
    uint32_t period_us = 1000000 / s_scan_rate;
    
    while (1) {
        if (!s_running) {
            vTaskDelay(pdMS_TO_TICKS(10));
            next_point_time = esp_timer_get_time();
            continue;
        }
        
        // Wait until next point time
        int64_t now = esp_timer_get_time();
        int64_t wait = next_point_time - now;
        
        if (wait > 0) {
            if (wait > 1000) {
                vTaskDelay(1);
            }
            while (esp_timer_get_time() < next_point_time) {
            }
        }
        
        // Output point
        if (frame_buffer_read(&pt)) {
            dac_output_point(&pt);
            s_last_point = pt;
            s_has_last = true;
        } else {
            // Buffer empty
            if (s_has_last) {
                dac_output_point(&s_last_point);
            }
        }
        
        period_us = 1000000 / s_scan_rate;
        next_point_time += period_us;
        
        now = esp_timer_get_time();
        if (next_point_time < now - 1000) {
            next_point_time = now;
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
        4096,
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

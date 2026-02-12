/**
 * @file main.c
 * @brief ILDAWaveX16 - Simple Ether Dream to Laser DAC
 * 
 * Network interlock:
 *   Boot → init W5500 → if link up → DHCP (10s timeout)
 *     ✓ Got IP → Ethernet only (WiFi off)
 *     ✗ No link / no DHCP → WiFi AP only (Ethernet off)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "config.h"
#include "hal/dac_timer.h"
#include "hal/w5500_eth.h"
#include "core/frame_buffer.h"
#include "input/etherdream_server.h"
#include "web/http_server.h"

static const char* TAG = "MAIN";

system_config_t g_config = {
    .scan_rate_hz = SCAN_RATE_DEFAULT_HZ,
};

system_status_t g_status = {0};

// =============================================================================
// WiFi AP (fallback when no Ethernet)
// =============================================================================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
            ESP_LOGI(TAG, "Client connected to AP (AID=%d)", event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
            ESP_LOGW(TAG, "Client disconnected from AP (AID=%d, reason=%d)", event->aid, event->reason);
        }
    }
}

static void wifi_ap_start(void) {
    // WiFi init + config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.ampdu_rx_enable = 1;
    cfg.ampdu_tx_enable = 1;
    cfg.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    
    esp_netif_create_default_wifi_ap();
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASS,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = WIFI_AP_MAX_CONN,
            .beacon_interval = 100,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(80);
    
    ESP_LOGI(TAG, "WiFi AP: %s ch%d 20MHz (http://192.168.4.1)", WIFI_AP_SSID, WIFI_AP_CHANNEL);
}

// =============================================================================
// Network interlock: Ethernet first, WiFi AP fallback
// =============================================================================

static bool network_init(void) {
    // Common: init netif + event loop (needed by both Ethernet and WiFi)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // --- Try Ethernet first ---
    ESP_LOGI(TAG, "--- Trying Ethernet (W5500) ---");
    
    esp_err_t ret = w5500_eth_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "W5500 init failed (%s) → WiFi AP fallback", esp_err_to_name(ret));
        wifi_ap_start();
        return false;
    }
    
    ret = w5500_eth_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "W5500 start failed (%s) → WiFi AP fallback", esp_err_to_name(ret));
        wifi_ap_start();
        return false;
    }
    
    // Wait for link up (max 3 seconds)
    ESP_LOGI(TAG, "Waiting for Ethernet link...");
    for (int i = 0; i < 30; i++) {
        if (w5500_eth_is_link_up()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (!w5500_eth_is_link_up()) {
        ESP_LOGW(TAG, "No Ethernet link → WiFi AP fallback");
        w5500_eth_stop();
        wifi_ap_start();
        return false;
    }
    
    ESP_LOGI(TAG, "Link up! Waiting for DHCP (%d ms timeout)...", ETH_DHCP_TIMEOUT_MS);
    
    // Wait for DHCP IP (max ETH_DHCP_TIMEOUT_MS)
    for (int i = 0; i < (ETH_DHCP_TIMEOUT_MS / 100); i++) {
        if (w5500_eth_has_ip()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (!w5500_eth_has_ip()) {
        ESP_LOGW(TAG, "DHCP timeout → WiFi AP fallback");
        w5500_eth_stop();
        wifi_ap_start();
        return false;
    }
    
    // Ethernet is active — do NOT start WiFi
    ESP_LOGI(TAG, "=== Ethernet mode (WiFi OFF) ===");
    return true;
}

// =============================================================================
// Tasks
// =============================================================================

static void network_task(void* arg) {
    while (1) {
        etherdream_server_loop();
    }
}

void app_main(void) {
    // Let USB-Serial/JTAG monitor connect before printing boot log
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "ILDAWaveX16 Starting...");
    ESP_LOGI(TAG, "  Buffer: %d points", FRAME_BUFFER_SIZE);
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Network interlock: Ethernet (DHCP) or WiFi AP — never both
    bool eth_mode = network_init();
    
    if (eth_mode) {
        ESP_LOGI(TAG, "Mode: ETHERNET (%.0f Mbps)", (float)w5500_eth_get_speed());
    } else {
        ESP_LOGI(TAG, "Mode: WiFi AP (%s)", WIFI_AP_SSID);
    }
    
    // Use hardware-timed DAC output (GPTimer + SPI queue)
    dac_timer_init();
    
    etherdream_server_init();
    etherdream_server_start();
    http_server_init();
    http_server_start();
    
    // Network task on Core 0
    xTaskCreatePinnedToCore(network_task, "net", 8192, NULL, TASK_PRIORITY_EDREAM, NULL, CORE_SERVICES);
    
    // Start DAC timer
    dac_timer_start();
    
    ESP_LOGI(TAG, "Ready - TCP:%d HTTP:%d", ETHERDREAM_TCP_PORT, HTTP_PORT);
    
    // Status loop
    while (1) {
        g_status.running = dac_timer_is_running();
        g_status.ed_connected = etherdream_server_is_connected();
        g_status.buffer_level = frame_buffer_level();
        g_status.current_scan_rate = dac_timer_get_scan_rate();
        g_status.ed_point_rate = etherdream_server_get_point_rate();
        g_status.free_heap = esp_get_free_heap_size();
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

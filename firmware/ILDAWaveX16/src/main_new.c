/**
 * @file main.c  
 * @brief ILDAWaveX16 - Ether Dream Laser DAC (SIMPLIFIED)
 * 
 * ARCHITECTURE:
 *   Everything on Core 0 - no cross-core overhead
 *   - GPTimer ISR: highest priority, outputs points to DAC
 *   - network_task: receives TCP data, feeds buffer
 *   - ISR preempts network_task when needed
 *   
 *   Core 1: idle (future: web server, OTA)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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

// For FreeRTOS cleanup
void vPortCleanUpTCB(void *pxTCB) { (void)pxTCB; }

// Global config
system_config_t g_config = {
    .scan_rate_hz = SCAN_RATE_DEFAULT_HZ,
    .brightness = 100,
};

system_status_t g_status = {0};

// Event groups
static EventGroupHandle_t s_net_event;
#define ETH_GOT_IP_BIT  BIT0

static bool s_eth_connected = false;

//=============================================================================
// Callbacks
//=============================================================================

static void eth_got_ip_callback(void) {
    ESP_LOGI(TAG, "Ethernet got IP");
    s_eth_connected = true;
    xEventGroupSetBits(s_net_event, ETH_GOT_IP_BIT);
}

//=============================================================================
// WiFi AP (fallback only)
//=============================================================================

static void wifi_init_ap(void) {
    ESP_LOGI(TAG, "Starting WiFi AP as fallback...");
    
    esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASS,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 2,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    ESP_LOGI(TAG, "WiFi AP: %s @ 192.168.4.1", WIFI_AP_SSID);
}

//=============================================================================
// Network Task - runs on Core 0
//=============================================================================

static void network_task(void* arg) {
    ESP_LOGI(TAG, "Network task on Core %d", xPortGetCoreID());
    
    while (1) {
        // This uses select() internally - yields CPU properly
        etherdream_server_loop();
        
        // Print DAC stats periodically
        dac_timer_print_stats();
    }
}

//=============================================================================
// Main
//=============================================================================

void app_main(void) {
    ESP_LOGI(TAG, "ILDAWaveX16 Starting (simplified architecture)");
    ESP_LOGI(TAG, "  Buffer: %d points", FRAME_BUFFER_SIZE);
    
    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    s_net_event = xEventGroupCreate();
    
    //=========================================================================
    // NETWORK: Ethernet first, WiFi AP as fallback
    //=========================================================================
    
    w5500_eth_set_got_ip_callback(eth_got_ip_callback);
    
    ret = w5500_eth_init();
    if (ret == ESP_OK) {
        w5500_eth_start();
        ESP_LOGI(TAG, "Waiting for Ethernet DHCP (10s)...");
        
        EventBits_t bits = xEventGroupWaitBits(s_net_event, ETH_GOT_IP_BIT,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        
        if (bits & ETH_GOT_IP_BIT) {
            ESP_LOGI(TAG, "Ethernet connected - WiFi disabled");
        } else {
            ESP_LOGW(TAG, "No Ethernet IP - starting WiFi AP");
            wifi_init_ap();
        }
    } else {
        ESP_LOGW(TAG, "Ethernet init failed - WiFi AP only");
        wifi_init_ap();
    }
    
    //=========================================================================
    // DAC TIMER: Init but don't start yet
    //=========================================================================
    
    ret = dac_timer_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DAC timer init failed!");
        return;
    }
    
    //=========================================================================
    // ETHER DREAM SERVER
    //=========================================================================
    
    etherdream_server_init();
    etherdream_server_start();
    
    //=========================================================================
    // HTTP SERVER (on Core 1 - non-critical)
    //=========================================================================
    
    http_server_init();
    http_server_start();
    
    //=========================================================================
    // START DAC OUTPUT
    //=========================================================================
    
    dac_timer_start();
    
    //=========================================================================
    // NETWORK TASK on Core 0 (same as ISR - no cross-core sync!)
    //=========================================================================
    
    xTaskCreatePinnedToCore(
        network_task,
        "network",
        8192,
        NULL,
        5,      // Medium priority - ISR will preempt when needed
        NULL,
        0       // Core 0 - same as GPTimer ISR!
    );
    
    ESP_LOGI(TAG, "Ready - TCP port %d", ETHERDREAM_TCP_PORT);
    
    //=========================================================================
    // MAIN LOOP - just status updates
    //=========================================================================
    
    while (1) {
        g_status.running = dac_timer_is_running();
        g_status.ed_connected = etherdream_server_is_connected();
        g_status.buffer_level = frame_buffer_level();
        g_status.current_scan_rate = dac_timer_get_scan_rate();
        g_status.ed_point_rate = etherdream_server_get_point_rate();
        g_status.free_heap = esp_get_free_heap_size();
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

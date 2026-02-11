/**
 * @file main.c
 * @brief ILDAWaveX16 - Simple Ether Dream to Laser DAC
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

void vPortCleanUpTCB(void *pxTCB) { (void)pxTCB; }

#include "config.h"
#include "core/dac_engine.h"
#include "hal/dac_timer.h"
#include "hal/w5500_eth.h"
#include "core/frame_buffer.h"
#include "input/etherdream_server.h"
#include "web/http_server.h"

static const char* TAG = "MAIN";

system_config_t g_config = {
    .scan_rate_hz = SCAN_RATE_DEFAULT_HZ,
    .brightness = 100,
    .color_invert = false,
    .xy_swap = false,
    .x_invert = false,
    .y_invert = false,
};

system_status_t g_status = {0};

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_network_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define ETH_GOT_IP_BIT     BIT0

static bool s_sta_connected = false;
static bool s_eth_connected = false;
static esp_netif_t* s_ap_netif = NULL;  // For dynamic AP

// Called by W5500 driver when Ethernet gets IP via DHCP
static void eth_got_ip_callback(void) {
    ESP_LOGI(TAG, "ETH Got IP via DHCP");
    s_eth_connected = true;
    if (s_network_event_group) {
        xEventGroupSetBits(s_network_event_group, ETH_GOT_IP_BIT);
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "WiFi STA started");
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_sta_connected = false;
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            ESP_LOGW(TAG, "WiFi disconnected");
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(TAG, "Client connected to AP");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Stop AP when connected
        if (s_ap_netif) {
            ESP_LOGI(TAG, "Stopping AP (STA connected)");
            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_netif_destroy(s_ap_netif);
            s_ap_netif = NULL;
        }
    }
}

bool wifi_save_credentials(const char* ssid, const char* password) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS");
        return false;
    }
    
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "pass", password);
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Saved WiFi: %s", ssid);
    return true;
}

bool wifi_connect_to(const char* ssid, const char* password) {
    ESP_LOGI(TAG, "Connecting to: %s", ssid);
    
    wifi_config_t sta_config = {0};
    strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char*)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();
    
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    
    if (bits & WIFI_CONNECTED_BIT) {
        wifi_save_credentials(ssid, password);
        ESP_LOGI(TAG, "WiFi connected successfully");
        return true;
    }
    
    ESP_LOGW(TAG, "WiFi connection failed");
    return false;
}

bool wifi_is_connected(void) {
    return s_sta_connected;
}

static void wifi_init(void) {
    ESP_LOGI(TAG, "Starting WiFi AP as fallback...");
    
    s_wifi_event_group = xEventGroupCreate();
    
    // netif and event loop already initialized in app_main
    esp_netif_create_default_wifi_sta();
    
    // Ottimizzazioni per bassa latenza
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.ampdu_rx_enable = 0;        // Disabilita AMPDU RX - riduce latenza
    cfg.ampdu_tx_enable = 0;        // Disabilita AMPDU TX - riduce latenza
    cfg.nvs_enable = 0;             // Non salvare config in NVS (più veloce)
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    
    // AP-only mode for best streaming performance
    s_ap_netif = esp_netif_create_default_wifi_ap();
    
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
    
    // Disabilita power saving
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(80);
    
    ESP_LOGI(TAG, "WiFi AP: %s (http://192.168.4.1)", WIFI_AP_SSID);
}

static void network_task(void* arg) {
    while (1) {
        etherdream_server_loop();
        // Must use vTaskDelay to feed watchdog (IDLE task needs to run)
        // 1 tick = 1ms at 1000Hz tick rate - acceptable latency for ACK
        vTaskDelay(1);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "ILDAWaveX16 Starting...");
    ESP_LOGI(TAG, "  Buffer: %d points", FRAME_BUFFER_SIZE);
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize network stack (required before Ethernet or WiFi)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Event group for Ethernet IP wait
    s_network_event_group = xEventGroupCreate();
    
    // ==========================================================================
    // ETHERNET FIRST - WiFi only as fallback
    // ==========================================================================
    
    // Set callback BEFORE init so we catch the IP event
    w5500_eth_set_got_ip_callback(eth_got_ip_callback);
    
    ret = w5500_eth_init();
    if (ret == ESP_OK) {
        w5500_eth_start();
        ESP_LOGI(TAG, "Ethernet initialized - waiting for DHCP (10s timeout)...");
        
        // Wait up to 10 seconds for Ethernet to get IP
        EventBits_t bits = xEventGroupWaitBits(
            s_network_event_group,
            ETH_GOT_IP_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(10000)
        );
        
        if (bits & ETH_GOT_IP_BIT) {
            ESP_LOGI(TAG, "Ethernet connected - WiFi disabled for low latency");
            // Do NOT initialize WiFi - Ethernet is primary
        } else {
            ESP_LOGW(TAG, "Ethernet no IP after 10s - starting WiFi AP as fallback");
            wifi_init();
        }
    } else {
        ESP_LOGW(TAG, "Ethernet init failed (0x%x) - WiFi AP only", ret);
        wifi_init();
    }
    
    // ==========================================================================
    // Application services
    // ==========================================================================
    
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
    
    // Show available interfaces
    if (s_eth_connected) {
        ESP_LOGI(TAG, "Ready - ETH (DHCP), TCP: %d", ETHERDREAM_TCP_PORT);
    } else {
        ESP_LOGI(TAG, "Ready - AP: %s, TCP: %d", WIFI_AP_SSID, ETHERDREAM_TCP_PORT);
    }
    
    // Simple status loop
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

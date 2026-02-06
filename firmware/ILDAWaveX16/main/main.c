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
#include "core/frame_buffer.h"
#include "input/etherdream_server.h"
#include "web/http_server.h"

static const char* TAG = "MAIN";

system_config_t g_config = {
    .scan_rate_hz = SCAN_RATE_DEFAULT_HZ,
    .brightness = 100,
    .repeat_last_frame = true,
    .color_invert = false,
    .xy_swap = false,
    .x_invert = false,
    .y_invert = false,
};

system_status_t g_status = {0};

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    
    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASS,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = WIFI_AP_MAX_CONN,
        },
    };
    
    wifi_config_t sta_config = {0};
    bool sta_configured = false;
    
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(sta_config.sta.ssid);
        if (nvs_get_str(nvs, "ssid", (char*)sta_config.sta.ssid, &len) == ESP_OK &&
            strlen((char*)sta_config.sta.ssid) > 0) {
            len = sizeof(sta_config.sta.password);
            nvs_get_str(nvs, "pass", (char*)sta_config.sta.password, &len);
            sta_configured = true;
        }
        nvs_close(nvs);
    }
    
    if (!sta_configured && strlen(WIFI_STA_SSID) > 0) {
        strncpy((char*)sta_config.sta.ssid, WIFI_STA_SSID, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char*)sta_config.sta.password, WIFI_STA_PASS, sizeof(sta_config.sta.password) - 1);
        sta_configured = true;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    
    if (sta_configured) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }
    
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    ESP_LOGI(TAG, "WiFi AP: %s", WIFI_AP_SSID);
}

static void network_task(void* arg) {
    while (1) {
        etherdream_server_loop();
        vTaskDelay(pdMS_TO_TICKS(1));
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
    
    wifi_init();
    dac_engine_init();
    etherdream_server_init();
    etherdream_server_start();
    http_server_init();
    http_server_start();
    
    // Network task on Core 0
    xTaskCreatePinnedToCore(network_task, "net", 8192, NULL, TASK_PRIORITY_EDREAM, NULL, CORE_SERVICES);
    
    // Start DAC
    dac_engine_start();
    
    ESP_LOGI(TAG, "Ready - AP: %s, TCP: %d", WIFI_AP_SSID, ETHERDREAM_TCP_PORT);
    
    // Simple status loop
    while (1) {
        g_status.running = dac_engine_is_running();
        g_status.ed_connected = etherdream_server_is_connected();
        g_status.buffer_level = frame_buffer_level();
        g_status.current_scan_rate = dac_engine_get_scan_rate();
        g_status.ed_point_rate = etherdream_server_get_point_rate();
        g_status.free_heap = esp_get_free_heap_size();
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

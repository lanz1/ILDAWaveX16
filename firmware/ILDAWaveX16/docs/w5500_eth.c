/**
 * @file w5500_eth.c
 * @brief W5500 Ethernet - HIGH PERFORMANCE / LOW LATENCY
 * 
 * Optimized for real-time laser streaming:
 *   - SPI @ 33MHz (W5500 max stable)
 *   - 1ms polling for minimum latency  
 *   - No interrupt pin needed
 */

#include "w5500_eth.h"
#include "config.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_phy.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <string.h>

static const char* TAG = "ETH";

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t* s_eth_netif = NULL;
static volatile bool s_link_up = false;
static volatile bool s_got_ip = false;
static uint32_t s_link_speed = 0;

// Callback to notify main when Ethernet gets IP
static eth_got_ip_cb_t s_got_ip_callback = NULL;

void w5500_eth_set_got_ip_callback(eth_got_ip_cb_t cb) {
    s_got_ip_callback = cb;
}

// Event handler for Ethernet events
static void eth_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED: {
            s_link_up = true;
            eth_speed_t speed;
            eth_duplex_t duplex;
            if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed) == ESP_OK &&
                esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex) == ESP_OK) {
                s_link_speed = (speed == ETH_SPEED_100M) ? 100 : 10;
                ESP_LOGI(TAG, "Link Up - %luMbps %s", 
                         (unsigned long)s_link_speed,
                         duplex == ETH_DUPLEX_FULL ? "Full" : "Half");
            }
            break;
        }
            
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Link Down");
            s_link_up = false;
            s_got_ip = false;
            s_link_speed = 0;
            break;
            
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Started");
            break;
            
        case ETHERNET_EVENT_STOP:
            s_link_up = false;
            s_got_ip = false;
            break;
            
        default:
            break;
    }
}

// Event handler for IP events  
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_got_ip = true;
        
        ESP_LOGI(TAG, "=== Ethernet Ready ===");
        ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "  GW: " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "  Speed: %lu Mbps", (unsigned long)s_link_speed);
        
        uint8_t mac[6];
        if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK) {
            ESP_LOGI(TAG, "  MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        ESP_LOGI(TAG, "======================");
        
        // Notify main
        if (s_got_ip_callback) {
            s_got_ip_callback();
        }
    }
}

esp_err_t w5500_eth_init(void)
{
    ESP_LOGI(TAG, "Init SPI@%dMHz CS=%d MOSI=%d MISO=%d CLK=%d", 
             ETH_SPI_FREQ_HZ / 1000000,
             PIN_ETH_CS, PIN_ETH_MOSI, PIN_ETH_MISO, PIN_ETH_SCK);
    
    // Initialize SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_ETH_MOSI,
        .miso_io_num = PIN_ETH_MISO,
        .sclk_io_num = PIN_ETH_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1600,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    
    esp_err_t ret = spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return ret;
    }
    
    // Brief delay for W5500 power-on
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Software reset via SPI (no hardware RESET pin)
    {
        spi_device_handle_t test_spi;
        spi_device_interface_config_t test_cfg = {
            .mode = 0,
            .clock_speed_hz = 1000000,
            .spics_io_num = PIN_ETH_CS,
            .queue_size = 1,
        };
        
        if (spi_bus_add_device(ETH_SPI_HOST, &test_cfg, &test_spi) == ESP_OK) {
            // Write RST bit to Mode Register
            uint8_t reset_cmd[4] = {0x00, 0x00, 0x04, 0x80};
            spi_transaction_t t = { .length = 32, .tx_buffer = reset_cmd };
            spi_device_transmit(test_spi, &t);
            
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // Verify W5500 responds
            uint8_t ver_cmd[4] = {0x00, 0x39, 0x00, 0x00};
            uint8_t ver_rx[4] = {0};
            spi_transaction_t vt = { .length = 32, .tx_buffer = ver_cmd, .rx_buffer = ver_rx };
            spi_device_transmit(test_spi, &vt);
            
            if (ver_rx[3] == 0x04) {
                ESP_LOGI(TAG, "W5500 OK (v0x%02X)", ver_rx[3]);
            } else {
                ESP_LOGW(TAG, "W5500 version: 0x%02X (expected 0x04)", ver_rx[3]);
            }
            
            spi_bus_remove_device(test_spi);
        }
    }
    
    // SPI device config for W5500 driver
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = ETH_SPI_FREQ_HZ,
        .spics_io_num = PIN_ETH_CS,
        .queue_size = 20,
    };
    
    // W5500 MAC config - REALTIME optimized
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = -1;        // No interrupt - use polling
    w5500_config.poll_period_ms = 2;       // 2ms polling (balance latency vs CPU)
    
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    mac_config.rx_task_prio = 12;          // Lower priority than DAC refill (15)
    
    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "MAC create failed");
        return ESP_FAIL;
    }
    
    // PHY config
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;
    
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "PHY create failed");
        return ESP_FAIL;
    }
    
    // Install driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Set MAC address
    uint8_t mac_addr[] = ETH_MAC_ADDR;
    esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);
    
    // Create netif with DHCP
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "Netif create failed");
        return ESP_FAIL;
    }
    
    // Attach driver to netif
    esp_eth_netif_glue_handle_t eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    esp_netif_attach(s_eth_netif, eth_glue);
    
    // Register event handlers
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ip_event_handler, NULL);
    
    ESP_LOGI(TAG, "Ready - waiting for link");
    return ESP_OK;
}

esp_err_t w5500_eth_start(void)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    return esp_eth_start(s_eth_handle);
}

esp_err_t w5500_eth_stop(void)
{
    if (!s_eth_handle) return ESP_ERR_INVALID_STATE;
    return esp_eth_stop(s_eth_handle);
}

bool w5500_eth_is_link_up(void)
{
    return s_link_up;
}

bool w5500_eth_has_ip(void)
{
    return s_got_ip;
}

esp_netif_t* w5500_eth_get_netif(void)
{
    return s_eth_netif;
}

uint32_t w5500_eth_get_speed(void)
{
    return s_link_speed;
}

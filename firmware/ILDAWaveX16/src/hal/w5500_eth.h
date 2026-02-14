#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

// Callback when Ethernet gets IP address
typedef void (*eth_got_ip_cb_t)(void);

/**
 * @brief Set callback for when Ethernet gets IP
 */
void w5500_eth_set_got_ip_callback(eth_got_ip_cb_t cb);

/**
 * @brief Initialize W5500 Ethernet
 * @return ESP_OK on success
 */
esp_err_t w5500_eth_init(void);

/**
 * @brief Start Ethernet
 * @return ESP_OK on success
 */
esp_err_t w5500_eth_start(void);

/**
 * @brief Stop Ethernet
 * @return ESP_OK on success
 */
esp_err_t w5500_eth_stop(void);

/**
 * @brief Check if link is up
 */
bool w5500_eth_is_link_up(void);

/**
 * @brief Check if we have an IP address
 */
bool w5500_eth_has_ip(void);

/**
 * @brief Get netif handle
 */
esp_netif_t* w5500_eth_get_netif(void);

/**
 * @brief Get link speed in Mbps
 */
uint32_t w5500_eth_get_speed(void);

#ifdef __cplusplus
}
#endif

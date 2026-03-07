/**
 * @file etherdream_server.h
 * @brief Ether Dream (j4cDAC) protocol server
 * 
 * Implements the Ether Dream protocol:
 * - TCP command/data streaming on port 7765
 * - UDP broadcast/discovery on port 7654
 * 
 * Based on j4cDAC reference firmware by Jacob Potter.
 */

#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Ether Dream server
 * @return ESP_OK on success
 */
esp_err_t etherdream_server_init(void);

/**
 * @brief Set network interface and mode for broadcast/tuning
 * @param netif Active network interface (WiFi AP or Ethernet)
 * @param eth_mode true if Ethernet, false if WiFi
 */
void etherdream_server_set_network(esp_netif_t* netif, bool eth_mode);

/**
 * @brief Start Ether Dream server (TCP + UDP broadcast)
 * @return ESP_OK on success
 */
esp_err_t etherdream_server_start(void);

/**
 * @brief Stop Ether Dream server
 * @return ESP_OK on success
 */
esp_err_t etherdream_server_stop(void);

/**
 * @brief Ether Dream server main loop (call from task)
 * Handles TCP accept/recv and periodic UDP broadcast
 */
void etherdream_server_loop(void);

/**
 * @brief Check if an Ether Dream client is connected and streaming
 * @return true if client connected
 */
bool etherdream_server_is_connected(void);

/**
 * @brief Get current point rate (from begin/queue commands)
 * @return Point rate in Hz, or 0 if not playing
 */
uint32_t etherdream_server_get_point_rate(void);

/**
 * @brief Get measured incoming point rate (updated every 500ms)
 * @return Actual measured points per second
 */
uint32_t etherdream_server_get_measured_pps(void);

#ifdef __cplusplus
}
#endif

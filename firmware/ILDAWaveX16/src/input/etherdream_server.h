#pragma once

#include "esp_err.h"
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

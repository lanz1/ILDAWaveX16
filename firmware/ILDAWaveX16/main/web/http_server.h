/**
 * @file http_server.h
 * @brief HTTP server with REST API and embedded web UI
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize HTTP server
 * @return ESP_OK on success
 */
esp_err_t http_server_init(void);

/**
 * @brief Start HTTP server
 * @return ESP_OK on success
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop HTTP server
 * @return ESP_OK on success
 */
esp_err_t http_server_stop(void);

#ifdef __cplusplus
}
#endif

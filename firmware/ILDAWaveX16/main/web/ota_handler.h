/**
 * @file ota_handler.h
 * @brief OTA update handler
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register OTA HTTP handlers
 * @param server HTTP server handle
 * @return ESP_OK on success
 */
esp_err_t ota_handler_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

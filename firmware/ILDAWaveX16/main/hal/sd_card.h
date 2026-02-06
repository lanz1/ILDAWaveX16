/**
 * @file sd_card.h
 * @brief SD card interface
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SD card
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sd_card_init(void);

/**
 * @brief Deinitialize SD card
 */
void sd_card_deinit(void);

/**
 * @brief Check if SD card is mounted
 * @return true if mounted
 */
bool sd_card_is_mounted(void);

/**
 * @brief Get SD card mount point
 * @return Mount point path
 */
const char* sd_card_get_mount_point(void);

#ifdef __cplusplus
}
#endif

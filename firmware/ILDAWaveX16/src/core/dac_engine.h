/**
 * @file dac_engine.h
 * @brief Simple DAC output engine
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dac_engine_init(void);
esp_err_t dac_engine_start(void);
esp_err_t dac_engine_stop(void);
bool dac_engine_is_running(void);
esp_err_t dac_engine_set_scan_rate(uint32_t rate_hz);
uint32_t dac_engine_get_scan_rate(void);

#ifdef __cplusplus
}
#endif

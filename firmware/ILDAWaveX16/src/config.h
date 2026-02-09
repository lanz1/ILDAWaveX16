/**
 * @file config.h
 * @brief ILDAWaveX16 - Minimal Ether Dream laser DAC
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Hardware Pins
// =============================================================================

#define PIN_DAC_CS          10
#define PIN_DAC_MOSI        11
#define PIN_DAC_SCK         12
#define PIN_DAC_MISO        9

#define PIN_SD_CS           13
#define PIN_SD_MOSI         14
#define PIN_SD_SCK          15
#define PIN_SD_MISO         16

// =============================================================================
// Network
// =============================================================================

#define WIFI_AP_SSID        "ILDAWaveX16"
#define WIFI_AP_PASS        "lasershow"
#define WIFI_AP_CHANNEL     1
#define WIFI_AP_MAX_CONN    4

#define ETHERDREAM_TCP_PORT 7765
#define ETHERDREAM_UDP_PORT 7654
#define HTTP_PORT           80

// =============================================================================
// DAC / Timing
// =============================================================================

#define SCAN_RATE_MIN_HZ    1000
#define SCAN_RATE_DEFAULT_HZ 10000
#define SCAN_RATE_MAX_HZ    100000

#define FRAME_BUFFER_SIZE   8192

#define DAC_SPI_HOST        SPI3_HOST
#define SD_SPI_HOST         SPI2_HOST

// =============================================================================
// FreeRTOS
// =============================================================================

#define CORE_SERVICES       0
#define TASK_PRIORITY_EDREAM 18

// =============================================================================
// Laser Point (Ether Dream format)
// =============================================================================

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint8_t user1;
    uint8_t user2;
    uint8_t flags;
} __attribute__((packed)) laser_point_t;

#define POINT_FLAG_BLANK    0x01

// =============================================================================
// Global Configuration
// =============================================================================

typedef struct {
    uint32_t scan_rate_hz;
    uint8_t brightness;
    bool color_invert;
    bool xy_swap;
    bool x_invert;
    bool y_invert;
} system_config_t;

typedef struct {
    bool running;
    bool ed_connected;
    uint32_t buffer_level;
    uint32_t current_scan_rate;
    uint64_t points_rendered;
    uint32_t ed_point_rate;
    uint32_t free_heap;
} system_status_t;

extern system_config_t g_config;
extern system_status_t g_status;

#ifdef __cplusplus
}
#endif

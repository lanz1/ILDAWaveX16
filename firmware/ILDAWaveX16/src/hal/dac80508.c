/**
 * @file dac80508.c
 * @brief DAC80508 8-channel 16-bit DAC driver
 */

#include "dac80508.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

static const char* TAG = "DAC80508";

// ============================================================================
// DAC80508 Register Map
// ============================================================================
#define DAC_REG_NOOP        0x00
#define DAC_REG_DEVID       0x01
#define DAC_REG_SYNC        0x02
#define DAC_REG_CONFIG      0x03
#define DAC_REG_GAIN        0x04
#define DAC_REG_TRIGGER     0x05
#define DAC_REG_BRDCAST     0x06
#define DAC_REG_STATUS      0x07
#define DAC_REG_DAC0        0x08
#define DAC_REG_DAC1        0x09
#define DAC_REG_DAC2        0x0A
#define DAC_REG_DAC3        0x0B
#define DAC_REG_DAC4        0x0C
#define DAC_REG_DAC5        0x0D
#define DAC_REG_DAC6        0x0E
#define DAC_REG_DAC7        0x0F

// SPI handle
static spi_device_handle_t s_spi = NULL;

// ============================================================================
// Low-level SPI Communication
// ============================================================================

esp_err_t dac_write_register(uint8_t reg, uint16_t value) {
    if (!s_spi) return ESP_ERR_INVALID_STATE;

    // DAC80508 24-bit write frame: [reg_addr] [data_hi] [data_lo]
    uint8_t tx[4] = {
        reg,
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)(value & 0xFF),
        0
    };

    spi_transaction_t trans = {
        .length = 24,
        .tx_buffer = tx,
    };

    return spi_device_transmit(s_spi, &trans);
}

esp_err_t dac_read_register(uint8_t reg, uint16_t* value) {
    if (!s_spi || !value) return ESP_ERR_INVALID_ARG;

    // First transaction: send read command
    uint8_t tx1[4] = {
        (uint8_t)(reg | 0x80),
        0x00, 0x00, 0x00
    };
    spi_transaction_t trans1 = {
        .length = 24,
        .tx_buffer = tx1,
    };
    esp_err_t ret = spi_device_transmit(s_spi, &trans1);
    if (ret != ESP_OK) return ret;

    // Second transaction: clock out the data
    uint8_t tx2[4] = {0};
    uint8_t rx2[4] = {0};
    spi_transaction_t trans2 = {
        .length = 24,
        .tx_buffer = tx2,
        .rx_buffer = rx2,
    };
    ret = spi_device_transmit(s_spi, &trans2);
    if (ret != ESP_OK) return ret;

    *value = ((uint16_t)rx2[1] << 8) | rx2[2];
    return ESP_OK;
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t dac_init(void) {
    ESP_LOGI(TAG, "Initializing DAC80508...");

    // Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_DAC_MOSI,
        .miso_io_num = PIN_DAC_MISO,
        .sclk_io_num = PIN_DAC_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };

    esp_err_t ret = spi_bus_initialize(DAC_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // SPI device config
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 50000000,
        .mode = 1,
        .spics_io_num = PIN_DAC_CS,
        .queue_size = 8,
        .flags = SPI_DEVICE_NO_DUMMY,
    };

    ret = spi_bus_add_device(DAC_SPI_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // Soft reset
    dac_write_register(DAC_REG_TRIGGER, 0x000A);
    vTaskDelay(pdMS_TO_TICKS(5));

    // Configure: 2x gain (0-5V), sync mode, internal ref
    dac_write_register(DAC_REG_GAIN, 0x00FF);
    dac_write_register(DAC_REG_SYNC, 0xFFFF);
    dac_write_register(DAC_REG_CONFIG, 0x0000);

    // Initial output: X,Y centered, colors off
    dac_write_register(DAC_REG_DAC0 + DAC_CH_X, 32768);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, 32768);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_RED, 0);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_GREEN, 0);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_BLUE, 0);
    dac_write_register(DAC_REG_TRIGGER, 0x0010);

    ESP_LOGI(TAG, "DAC80508 ready");
    return ESP_OK;
}

static inline void dac_write_fast(uint8_t reg, uint16_t value) {
    uint8_t tx[4] = { reg, (uint8_t)(value >> 8), (uint8_t)value, 0 };
    spi_transaction_t trans = { .length = 24, .tx_buffer = tx };
    spi_device_polling_transmit(s_spi, &trans);
}

void dac_output_point(const laser_point_t* point) {
    if (!point || !s_spi) return;
    
    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    
    uint16_t x = (uint16_t)((int32_t)point->x + 32768);
    uint16_t y = (uint16_t)((int32_t)point->y + 32768);
    
    bool blank = (point->flags & POINT_FLAG_BLANK);
    uint16_t r = blank ? 0 : point->r;
    uint16_t g = blank ? 0 : point->g;
    uint16_t b = blank ? 0 : point->b;
    
    if (g_config.brightness < 100) {
        r = (uint16_t)(((uint32_t)r * g_config.brightness) / 100);
        g = (uint16_t)(((uint32_t)g * g_config.brightness) / 100);
        b = (uint16_t)(((uint32_t)b * g_config.brightness) / 100);
    }
    
    if (g_config.x_invert) x = 65535 - x;
    if (g_config.y_invert) y = 65535 - y;
    if (g_config.xy_swap) {
        uint16_t tmp = x; x = y; y = tmp;
    }
    if (g_config.color_invert) {
        r = 65535 - r;
        g = 65535 - g;
        b = 65535 - b;
    }
    
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_X, x);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_Y, y);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_RED, r);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_GREEN, g);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_BLUE, b);
    dac_write_fast(DAC_REG_TRIGGER, 0x0010);
    
    spi_device_release_bus(s_spi);
}

void dac_output_batch_timed(const laser_point_t* points, size_t count, uint32_t period_us, int64_t* next_time) {
    if (!points || count == 0 || !s_spi || !next_time) return;
    
    // Acquire bus once for entire batch
    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    
    // Cache config checks outside loop
    bool do_brightness = (g_config.brightness < 100);
    uint32_t brightness = g_config.brightness;
    bool x_inv = g_config.x_invert;
    bool y_inv = g_config.y_invert;
    bool xy_swap = g_config.xy_swap;
    bool color_inv = g_config.color_invert;
    
    for (size_t i = 0; i < count; i++) {
        // Wait for precise point timing
        while (esp_timer_get_time() < *next_time) {
            // busy-wait for precise timing (critical section, bus already locked)
        }
        
        const laser_point_t* point = &points[i];
        
        // Fast conversions
        uint16_t x = (uint16_t)((int32_t)point->x + 32768);
        uint16_t y = (uint16_t)((int32_t)point->y + 32768);
        
        bool blank = (point->flags & POINT_FLAG_BLANK);
        uint16_t r = blank ? 0 : point->r;
        uint16_t g = blank ? 0 : point->g;
        uint16_t b = blank ? 0 : point->b;
        
        // Apply brightness (single check per batch)
        if (do_brightness) {
            r = (uint16_t)(((uint32_t)r * brightness) / 100);
            g = (uint16_t)(((uint32_t)g * brightness) / 100);
            b = (uint16_t)(((uint32_t)b * brightness) / 100);
        }
        
        // Apply geometry transforms
        if (x_inv) x = 65535 - x;
        if (y_inv) y = 65535 - y;
        if (xy_swap) {
            uint16_t tmp = x; x = y; y = tmp;
        }
        if (color_inv) {
            r = 65535 - r;
            g = 65535 - g;
            b = 65535 - b;
        }
        
        // 6 SPI transactions without bus release
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_X, x);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_Y, y);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_RED, r);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_GREEN, g);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_BLUE, b);
        dac_write_fast(DAC_REG_TRIGGER, 0x0010);
        
        // Update next point time
        *next_time += period_us;
    }
    
    // Release bus once
    spi_device_release_bus(s_spi);
}

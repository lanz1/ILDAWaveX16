/**
 * @file dac80508.c
 * @brief DAC80508 8-channel 16-bit DAC driver
 */

#include "dac80508.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
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
    ESP_LOGI(TAG, "  SPI pins: CS=GPIO%d, MOSI=GPIO%d, SCK=GPIO%d",
             PIN_DAC_CS, PIN_DAC_MOSI, PIN_DAC_SCK);
    ESP_LOGI(TAG, "  Channel map: X=OUT%d, Y=OUT%d, R=OUT%d, G=OUT%d, B=OUT%d",
             DAC_CH_X, DAC_CH_Y, DAC_CH_RED, DAC_CH_GREEN, DAC_CH_BLUE);

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
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
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
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Small delay for DAC power-up
    vTaskDelay(pdMS_TO_TICKS(10));

    // Soft reset
    ret = dac_write_register(DAC_REG_TRIGGER, 0x000A);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "  Soft reset failed: %s (continuing)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "  TRIGGER = 0x000A (soft reset)");
    }
    vTaskDelay(pdMS_TO_TICKS(5));  // Wait for reset

    // Verify SPI communication
    uint16_t devid = 0;
    ret = dac_read_register(DAC_REG_DEVID, &devid);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "  DEVID = 0x%04X (expected 0x0??0 for DAC80508)", devid);
    } else {
        ESP_LOGW(TAG, "  Could not read DEVID: %s (continuing anyway)", esp_err_to_name(ret));
    }

    // Configure registers
    ret = dac_write_register(DAC_REG_GAIN, 0x00FF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write GAIN: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  GAIN = 0x00FF (2x gain all channels, 0-5V range)");

    ret = dac_write_register(DAC_REG_SYNC, 0xFFFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write SYNC: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  SYNC = 0xFFFF (sync mode, batch update on LDAC trigger)");

    ret = dac_write_register(DAC_REG_CONFIG, 0x0000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write CONFIG: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  CONFIG = 0x0000 (internal ref enabled)");

    // Initial output: X,Y centered, colors off
    dac_write_register(DAC_REG_DAC0 + DAC_CH_X, 32768);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, 32768);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_RED, 0);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_GREEN, 0);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_BLUE, 0);
    dac_write_register(DAC_REG_TRIGGER, 0x0010);

    ESP_LOGI(TAG, "  Initial output: X=center, Y=center, RGB=off");

    dac_debug_dump();

    ESP_LOGI(TAG, "DAC80508 initialized successfully");
    return ESP_OK;
}

static inline void dac_write_fast(uint8_t reg, uint16_t value) {
    uint8_t tx[4] = { reg, (uint8_t)(value >> 8), (uint8_t)value, 0 };
    spi_transaction_t trans = { .length = 24, .tx_buffer = tx };
    spi_device_polling_transmit(s_spi, &trans);
}

void dac_output_point(const laser_point_t* point) {
    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (!point || !s_spi) return;

    uint16_t x = (uint16_t)((int32_t)point->x + 32768);
    uint16_t y = (uint16_t)((int32_t)point->y + 32768);

    uint16_t r = (point->flags & POINT_FLAG_BLANK) ? 0 : point->r;
    uint16_t g = (point->flags & POINT_FLAG_BLANK) ? 0 : point->g;
    uint16_t b = (point->flags & POINT_FLAG_BLANK) ? 0 : point->b;

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
        r = 65535 - r; g = 65535 - g; b = 65535 - b;
    }

    dac_write_fast(DAC_REG_DAC0 + DAC_CH_X, x);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_Y, y);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_RED, r);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_GREEN, g);
    dac_write_fast(DAC_REG_DAC0 + DAC_CH_BLUE, b);
    dac_write_fast(DAC_REG_TRIGGER, 0x0010);
    
    spi_device_release_bus(s_spi);
}

// ============================================================================
// Test Patterns
// ============================================================================

void dac_set_all(uint16_t value) {
    if (!s_spi) return;
    for (int i = 0; i < 8; i++) {
        dac_write_register(DAC_REG_DAC0 + i, value);
    }
}

void dac_test_pattern(uint8_t pattern_id) {
    if (!s_spi) return;

    ESP_LOGI(TAG, "Running test pattern %d...", pattern_id);

    switch (pattern_id) {
        case 0: {
            // Pattern 0: Static center point, all colors ON
            ESP_LOGI(TAG, "  Pattern 0: Center point, white");
            dac_write_register(DAC_REG_DAC0 + DAC_CH_X, 32768);
            dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, 32768);
            dac_write_register(DAC_REG_DAC0 + DAC_CH_RED, 65535);
            dac_write_register(DAC_REG_DAC0 + DAC_CH_GREEN, 65535);
            dac_write_register(DAC_REG_DAC0 + DAC_CH_BLUE, 65535);
            dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC trigger
            break;
        }

        case 1: {
            // Pattern 1: Animated square (red) - quick test, 20 iterations
            ESP_LOGI(TAG, "  Pattern 1: Square (red), 20 iterations");
            const uint16_t corners_x[] = {16384, 49152, 49152, 16384};
            const uint16_t corners_y[] = {16384, 16384, 49152, 49152};
            const int steps_per_edge = 50;

            for (int iter = 0; iter < 20; iter++) {
                for (int edge = 0; edge < 4; edge++) {
                    int next = (edge + 1) % 4;
                    for (int s = 0; s < steps_per_edge; s++) {
                        uint16_t x = corners_x[edge] + 
                            (int32_t)(corners_x[next] - corners_x[edge]) * s / steps_per_edge;
                        uint16_t y = corners_y[edge] + 
                            (int32_t)(corners_y[next] - corners_y[edge]) * s / steps_per_edge;
                        dac_write_register(DAC_REG_DAC0 + DAC_CH_X, x);
                        dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, y);
                        dac_write_register(DAC_REG_DAC0 + DAC_CH_RED, 65535);
                        dac_write_register(DAC_REG_DAC0 + DAC_CH_GREEN, 0);
                        dac_write_register(DAC_REG_DAC0 + DAC_CH_BLUE, 0);
                        dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
                    }
                }
                taskYIELD();  // Feed watchdog
            }
            ESP_LOGI(TAG, "  Pattern 1 done");
            break;
        }

        case 2: {
            // Pattern 2: Animated circle (green) - quick test, 20 iterations
            ESP_LOGI(TAG, "  Pattern 2: Circle (green), 20 iterations");
            const int circle_points = 200;
            for (int iter = 0; iter < 20; iter++) {
                for (int i = 0; i < circle_points; i++) {
                    float angle = (2.0f * M_PI * i) / circle_points;
                    uint16_t x = (uint16_t)(32768 + 16384 * cosf(angle));
                    uint16_t y = (uint16_t)(32768 + 16384 * sinf(angle));
                    dac_write_register(DAC_REG_DAC0 + DAC_CH_X, x);
                    dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, y);
                    dac_write_register(DAC_REG_DAC0 + DAC_CH_RED, 0);
                    dac_write_register(DAC_REG_DAC0 + DAC_CH_GREEN, 65535);
                    dac_write_register(DAC_REG_DAC0 + DAC_CH_BLUE, 0);
                    dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
                }
                taskYIELD();  // Feed watchdog
            }
            ESP_LOGI(TAG, "  Pattern 2 done");
            break;
        }

        case 3: {
            // Pattern 3: Full-scale ramp on all channels
            ESP_LOGI(TAG, "  Pattern 3: Full-scale ramp 0→65535 on X,Y,R,G,B");
            for (uint32_t v = 0; v <= 65535; v += 256) {
                uint16_t val = (uint16_t)v;
                dac_write_register(DAC_REG_DAC0 + DAC_CH_X, val);
                dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, val);
                dac_write_register(DAC_REG_DAC0 + DAC_CH_RED, val);
                dac_write_register(DAC_REG_DAC0 + DAC_CH_GREEN, val);
                dac_write_register(DAC_REG_DAC0 + DAC_CH_BLUE, val);
                dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
            }
            ESP_LOGI(TAG, "  Pattern 3 done");
            break;
        }

        case 4: {
            // Pattern 4: Channel walk — one channel at a time to max, then back
            ESP_LOGI(TAG, "  Pattern 4: Channel walk (identify each output)");
            const uint8_t channels[] = {DAC_CH_X, DAC_CH_Y, DAC_CH_RED, DAC_CH_GREEN, DAC_CH_BLUE};
            const char* names[] = {"X", "Y", "RED", "GREEN", "BLUE"};

            for (int ch = 0; ch < 5; ch++) {
                ESP_LOGI(TAG, "    Testing %s (DAC ch%d)...", names[ch], channels[ch]);
                // All channels to 0 / center
                dac_set_all(0);
                dac_write_register(DAC_REG_DAC0 + DAC_CH_X, 32768);
                dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, 32768);
                dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
                vTaskDelay(pdMS_TO_TICKS(200));

                // Ramp up this channel
                for (uint32_t v = 0; v <= 65535; v += 1024) {
                    dac_write_register(DAC_REG_DAC0 + channels[ch], (uint16_t)v);
                    dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
                // Hold at max
                dac_write_register(DAC_REG_DAC0 + channels[ch], 65535);
                dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
                vTaskDelay(pdMS_TO_TICKS(500));

                // Ramp down
                for (int32_t v = 65535; v >= 0; v -= 1024) {
                    dac_write_register(DAC_REG_DAC0 + channels[ch], (uint16_t)v);
                    dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            // Return to center
            dac_set_all(0);
            dac_write_register(DAC_REG_DAC0 + DAC_CH_X, 32768);
            dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, 32768);
            dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC
            ESP_LOGI(TAG, "  Pattern 4 done");
            break;
        }

        case 5: {
            // Pattern 5: DIAGNOSTIC - Hold mid-scale (2.5V) on ALL channels
            // Use multimeter to verify each DAC output
            ESP_LOGI(TAG, "  Pattern 5: DIAGNOSTIC - ALL outputs at 2.5V (mid-scale)");
            ESP_LOGI(TAG, "  Writing 32768 to all DAC channels (expect ~2.5V on each output)");
            
            // Write mid-scale to all 8 channels
            for (int ch = 0; ch < 8; ch++) {
                dac_write_register(DAC_REG_DAC0 + ch, 32768);
            }
            dac_write_register(DAC_REG_TRIGGER, 0x0010);  // LDAC trigger
            
            ESP_LOGI(TAG, "  Holding for 30 seconds - measure outputs NOW:");
            ESP_LOGI(TAG, "    OUT0 (DAC0): expect 2.5V");
            ESP_LOGI(TAG, "    OUT1 (DAC1): expect 2.5V");
            ESP_LOGI(TAG, "    OUT2 (DAC2): expect 2.5V");
            ESP_LOGI(TAG, "    OUT3=BLUE:   expect 2.5V");
            ESP_LOGI(TAG, "    OUT4=GREEN:  expect 2.5V");
            ESP_LOGI(TAG, "    OUT5=RED:    expect 2.5V");
            ESP_LOGI(TAG, "    OUT6=X:      expect 2.5V");
            ESP_LOGI(TAG, "    OUT7=Y:      expect 2.5V");
            vTaskDelay(pdMS_TO_TICKS(30000));  // 30 seconds to measure
            
            ESP_LOGI(TAG, "  Pattern 5 done");
            break;
        }

        case 6: {
            // Pattern 6: DIAGNOSTIC - Full-scale (5V) on ALL channels
            ESP_LOGI(TAG, "  Pattern 6: DIAGNOSTIC - ALL outputs at 5V (full-scale)");
            
            for (int ch = 0; ch < 8; ch++) {
                dac_write_register(DAC_REG_DAC0 + ch, 65535);
            }
            dac_write_register(DAC_REG_TRIGGER, 0x0010);
            
            ESP_LOGI(TAG, "  Holding for 30 seconds - measure outputs NOW (expect ~5V on each)");
            vTaskDelay(pdMS_TO_TICKS(30000));
            
            ESP_LOGI(TAG, "  Pattern 6 done");
            break;
        }

        case 7: {
            // Pattern 7: DIAGNOSTIC - Zero (0V) on ALL channels
            ESP_LOGI(TAG, "  Pattern 7: DIAGNOSTIC - ALL outputs at 0V");
            
            for (int ch = 0; ch < 8; ch++) {
                dac_write_register(DAC_REG_DAC0 + ch, 0);
            }
            dac_write_register(DAC_REG_TRIGGER, 0x0010);
            
            ESP_LOGI(TAG, "  Holding for 30 seconds - measure outputs NOW (expect ~0V on each)");
            vTaskDelay(pdMS_TO_TICKS(30000));
            
            ESP_LOGI(TAG, "  Pattern 7 done");
            break;
        }

        default:
            ESP_LOGW(TAG, "  Unknown pattern %d", pattern_id);
            break;
    }
}

// ============================================================================
// Debug
// ============================================================================

void dac_debug_dump(void) {
    if (!s_spi) {
        ESP_LOGW(TAG, "DAC not initialized");
        return;
    }

    ESP_LOGI(TAG, "=== DAC80508 Register Dump ===");

    const char* reg_names[] = {
        "NOOP", "DEVID", "SYNC", "CONFIG", "GAIN", "TRIGGER", "BRDCAST", "STATUS",
        "DAC0", "DAC1", "DAC2", "DAC3", "DAC4", "DAC5", "DAC6", "DAC7"
    };

    for (int reg = 0x01; reg <= 0x0F; reg++) {
        uint16_t val = 0;
        esp_err_t ret = dac_read_register(reg, &val);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "  [0x%02X] %-8s = 0x%04X (%u)", reg, reg_names[reg], val, val);
        } else {
            ESP_LOGW(TAG, "  [0x%02X] %-8s = READ ERROR", reg, reg_names[reg]);
        }
    }

    ESP_LOGI(TAG, "=== Channel Mapping ===");
    ESP_LOGI(TAG, "  X=DAC%d  Y=DAC%d  R=DAC%d  G=DAC%d  B=DAC%d",
             DAC_CH_X, DAC_CH_Y, DAC_CH_RED, DAC_CH_GREEN, DAC_CH_BLUE);
    ESP_LOGI(TAG, "==============================");
}

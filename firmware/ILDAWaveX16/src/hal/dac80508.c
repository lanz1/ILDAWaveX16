/**
 * @file dac80508.c
 * @brief DAC80508 8-channel 16-bit SPI DAC driver
 * 
 * Uses direct SPI register access for maximum speed (~1µs per write)
 */

#include "dac80508.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "hal/spi_ll.h"
#include "soc/spi_struct.h"
#include "soc/spi_reg.h"
#include "soc/gpio_struct.h"
#include <string.h>

static const char* TAG = "DAC80508";

// DAC80508 Register Addresses
#define DAC_REG_NOOP      0x00
#define DAC_REG_DEVID     0x01
#define DAC_REG_SYNC      0x02
#define DAC_REG_CONFIG    0x03
#define DAC_REG_GAIN      0x04
#define DAC_REG_TRIGGER   0x06
#define DAC_REG_BRDCAST   0x07
#define DAC_REG_STATUS    0x07
#define DAC_REG_DAC0      0x08

// SPI configuration
#define DAC_SPI_HOST      SPI3_HOST
#define DAC_PIN_MOSI      11
#define DAC_PIN_CLK       12
#define DAC_PIN_CS        10
#define DAC_SPI_SPEED_HZ  (50 * 1000 * 1000)  // 50 MHz

static spi_device_handle_t s_spi = NULL;

// =============================================================================
// DIRECT SPI REGISTER ACCESS (bypasses ESP-IDF driver for ~1µs vs ~9µs)
// =============================================================================

// Pre-computed register addresses for fastest access
static volatile uint32_t* s_spi_cmd_reg;
static volatile uint32_t* s_spi_user_reg;
static volatile uint32_t* s_spi_ms_dlen_reg;
static volatile uint32_t* s_spi_w0_reg;
static volatile uint32_t* s_spi_clock_reg;
static volatile uint32_t* s_spi_user1_reg;
static volatile uint32_t* s_spi_user2_reg;
static volatile uint32_t* s_spi_ctrl_reg;

static bool s_direct_spi_ready = false;

static void init_direct_spi(void) {
    // Cache register addresses for fastest access
    s_spi_cmd_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x00);       // SPI_CMD_REG
    s_spi_ctrl_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x08);      // SPI_CTRL_REG
    s_spi_clock_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x0C);     // SPI_CLOCK_REG
    s_spi_user_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x10);      // SPI_USER_REG
    s_spi_user1_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x14);     // SPI_USER1_REG
    s_spi_user2_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x18);     // SPI_USER2_REG
    s_spi_ms_dlen_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x1C);   // SPI_MS_DLEN_REG
    s_spi_w0_reg = (volatile uint32_t*)(DR_REG_SPI3_BASE + 0x98);        // SPI_W0_REG
    
    // Configure SPI for direct access:
    // - Mode 1 (CPOL=0, CPHA=1): clock idle low, data sampled on falling edge
    // - MSB first
    // - Full duplex master mode
    
    // SPI_USER_REG: Enable MOSI, set byte order
    uint32_t user_val = SPI_USR_MOSI |  // Enable MOSI phase
                        SPI_CK_OUT_EDGE; // Clock edge (CPHA=1)
    *s_spi_user_reg = user_val;
    
    // SPI_CTRL_REG: Clear any special modes
    *s_spi_ctrl_reg = 0;
    
    s_direct_spi_ready = true;
    ESP_LOGI(TAG, "Direct SPI registers initialized (mode 1)");
}

// Ultra-fast 24-bit SPI write with manual CS control
// Takes ~1-2µs instead of ~9µs with driver
static inline void IRAM_ATTR dac_write_direct(uint8_t reg, uint16_t value) {
    // Pack 24 bits: [reg:8][value_hi:8][value_lo:8]
    // ESP32 SPI W0 is transmitted LSB-first from bit 0, so we need correct packing
    uint32_t data = ((uint32_t)(value & 0xFF) << 16) |    // value_lo at bits 16-23
                    ((uint32_t)(value >> 8) << 8) |        // value_hi at bits 8-15
                    ((uint32_t)reg);                       // reg at bits 0-7
    
    // CS LOW
    GPIO.out_w1tc = (1 << DAC_PIN_CS);
    
    // Small delay for CS setup time
    __asm__ __volatile__("nop; nop; nop; nop;");
    
    // Write data to FIFO
    *s_spi_w0_reg = data;
    
    // Set length: 24 bits - 1 = 23
    *s_spi_ms_dlen_reg = 23;
    
    // Start transmission
    *s_spi_cmd_reg = SPI_USR;
    
    // Wait for completion (poll SPI_USR bit)
    while (*s_spi_cmd_reg & SPI_USR) {}
    
    // Small delay for CS hold time
    __asm__ __volatile__("nop; nop; nop; nop;");
    
    // CS HIGH  
    GPIO.out_w1ts = (1 << DAC_PIN_CS);
}

// =============================================================================
// DAC INITIALIZATION
// =============================================================================

esp_err_t dac_init(void) {
    ESP_LOGI(TAG, "Initializing DAC80508");
    
    // Initialize SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = DAC_PIN_MOSI,
        .miso_io_num = -1,              // Not used
        .sclk_io_num = DAC_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    
    esp_err_t ret = spi_bus_initialize(DAC_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Add SPI device - with CS for driver operations
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = DAC_SPI_SPEED_HZ,
        .mode = 1,                      // CPOL=0, CPHA=1 per datasheet
        .spics_io_num = DAC_PIN_CS,     // Let driver control CS for normal ops
        .queue_size = 1,
        .flags = 0,
    };
    
    ret = spi_bus_add_device(DAC_SPI_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "SPI OK at %d MHz", DAC_SPI_SPEED_HZ / 1000000);
    
    // Configure DAC registers using driver (safe initialization)
    // GAIN: Set all channels to 2x gain (0-10V range from 5V ref)
    dac_write_register(DAC_REG_GAIN, 0x00FF);
    
    // SYNC: Disable sync on all channels (individual updates)
    dac_write_register(DAC_REG_SYNC, 0x0000);
    
    // CONFIG: Power on all channels, internal reference
    dac_write_register(DAC_REG_CONFIG, 0x0000);
    
    // Initialize all outputs to mid-scale
    dac_write_register(DAC_REG_DAC0 + DAC_CH_X, 32768);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_Y, 32768);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_RED, 0);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_GREEN, 0);
    dac_write_register(DAC_REG_DAC0 + DAC_CH_BLUE, 0);
    dac_write_register(DAC_REG_TRIGGER, 0x0010);
    
    // Now switch to manual CS for direct SPI access
    // First remove device from bus
    spi_bus_remove_device(s_spi);
    
    // Re-add with manual CS
    dev_cfg.spics_io_num = -1;  // Manual CS control for direct access
    ret = spi_bus_add_device(DAC_SPI_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device re-add failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure CS pin for manual control
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << DAC_PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(DAC_PIN_CS, 1);  // CS high (inactive)
    
    // Initialize direct SPI registers
    init_direct_spi();

    ESP_LOGI(TAG, "DAC80508 ready");
    return ESP_OK;
}

esp_err_t dac_write_register(uint8_t reg, uint16_t value) {
    if (!s_spi) return ESP_ERR_INVALID_STATE;
    
    uint8_t tx[4] = {
        reg,                         // Register address
        (uint8_t)(value >> 8),       // Data high byte
        (uint8_t)(value & 0xFF),     // Data low byte
        0                            // Padding (not sent, but needed for alignment)
    };
    
    spi_transaction_t trans = {
        .length = 24,                // 24 bits
        .tx_buffer = tx,
    };
    
    return spi_device_polling_transmit(s_spi, &trans);
}

esp_err_t dac_read_register(uint8_t reg, uint16_t* value) {
    if (!s_spi || !value) return ESP_ERR_INVALID_ARG;
    
    // DAC80508 read: send reg with R/W bit set, then read back
    uint8_t tx[4] = { (uint8_t)(reg | 0x80), 0, 0, 0 };
    uint8_t rx[4] = {0};
    
    spi_transaction_t trans = {
        .length = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    
    esp_err_t ret = spi_device_polling_transmit(s_spi, &trans);
    if (ret == ESP_OK) {
        *value = ((uint16_t)rx[1] << 8) | rx[2];
    }
    return ret;
}

static inline void dac_write_fast(uint8_t reg, uint16_t value) {
    uint8_t tx[4] = { reg, (uint8_t)(value >> 8), (uint8_t)value, 0 };
    spi_transaction_t trans = { .length = 24, .tx_buffer = tx };
    spi_device_polling_transmit(s_spi, &trans);
}

void dac_output_point(const laser_point_t* point) {
    if (!point || !s_spi) return;
    
    // Conversione signed→unsigned (no altre trasformazioni per max performance)
    uint16_t x = (uint16_t)((int32_t)point->x + 32768);
    uint16_t y = (uint16_t)((int32_t)point->y + 32768);
    
    // Blanking
    bool blank = (point->flags & POINT_FLAG_BLANK);
    uint16_t r = blank ? 0 : point->r;
    uint16_t g = blank ? 0 : point->g;
    uint16_t b = blank ? 0 : point->b;
    
    // Try direct register access if ready, else fall back to driver
    if (s_direct_spi_ready) {
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_X, x);
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_Y, y);
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_RED, r);
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_GREEN, g);
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_BLUE, b);
        dac_write_direct(DAC_REG_TRIGGER, 0x0010);
    } else {
        spi_device_acquire_bus(s_spi, portMAX_DELAY);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_X, x);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_Y, y);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_RED, r);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_GREEN, g);
        dac_write_fast(DAC_REG_DAC0 + DAC_CH_BLUE, b);
        dac_write_fast(DAC_REG_TRIGGER, 0x0010);
        spi_device_release_bus(s_spi);
    }
}

void dac_output_batch_timed(const laser_point_t* points, size_t count, uint32_t period_us, int64_t* next_time) {
    if (!points || count == 0 || !s_spi || !next_time) return;
    
    // Acquire bus once for entire batch when using driver
    bool use_direct = s_direct_spi_ready;
    if (!use_direct) {
        spi_device_acquire_bus(s_spi, portMAX_DELAY);
    }
    
    for (size_t i = 0; i < count; i++) {
        // Wait for precise point timing
        while (esp_timer_get_time() < *next_time) {
            // busy-wait for precise timing
        }
        
        const laser_point_t* point = &points[i];
        
        // Conversione signed→unsigned (no altre trasformazioni per max performance)
        uint16_t x = (uint16_t)((int32_t)point->x + 32768);
        uint16_t y = (uint16_t)((int32_t)point->y + 32768);
        
        // Blanking
        bool blank = (point->flags & POINT_FLAG_BLANK);
        uint16_t r = blank ? 0 : point->r;
        uint16_t g = blank ? 0 : point->g;
        uint16_t b = blank ? 0 : point->b;
        
        if (use_direct) {
            // Direct SPI writes (~1µs each = ~6µs total)
            dac_write_direct(DAC_REG_DAC0 + DAC_CH_X, x);
            dac_write_direct(DAC_REG_DAC0 + DAC_CH_Y, y);
            dac_write_direct(DAC_REG_DAC0 + DAC_CH_RED, r);
            dac_write_direct(DAC_REG_DAC0 + DAC_CH_GREEN, g);
            dac_write_direct(DAC_REG_DAC0 + DAC_CH_BLUE, b);
            dac_write_direct(DAC_REG_TRIGGER, 0x0010);
        } else {
            // Driver SPI writes (~4-5µs each with bus held)
            dac_write_fast(DAC_REG_DAC0 + DAC_CH_X, x);
            dac_write_fast(DAC_REG_DAC0 + DAC_CH_Y, y);
            dac_write_fast(DAC_REG_DAC0 + DAC_CH_RED, r);
            dac_write_fast(DAC_REG_DAC0 + DAC_CH_GREEN, g);
            dac_write_fast(DAC_REG_DAC0 + DAC_CH_BLUE, b);
            dac_write_fast(DAC_REG_TRIGGER, 0x0010);
        }
        
        // Update next point time
        *next_time += period_us;
    }
    
    if (!use_direct) {
        spi_device_release_bus(s_spi);
    }
}

/**
 * @file led_status.c
 * @brief Status LED control using WS2812 via RMT
 */

#include "led_status.h"
#include "led_strip_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

static const char* TAG = "LED_STATUS";

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static uint8_t s_led_rgb[3] = {0, 0, 0};

static const uint8_t LED_COLORS[][3] = {
    [LED_OFF]       = {0, 0, 0},
    [LED_BOOT]      = {0, 0, 64},
    [LED_READY]     = {64, 0, 0},
    [LED_STREAMING] = {64, 0, 64},
    [LED_PLAYING]   = {64, 64, 0},
    [LED_ERROR]     = {0, 64, 0},
};

esp_err_t led_status_init(void) {
    ESP_LOGI(TAG, "Initializing status LED on GPIO %d", PIN_LED_STATUS);
    
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = PIN_LED_STATUS,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    
    esp_err_t ret = rmt_new_tx_channel(&tx_config, &s_led_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    led_strip_encoder_config_t encoder_config = {
        .resolution = 10000000,
    };
    
    ret = rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED encoder: %s", esp_err_to_name(ret));
        rmt_del_channel(s_led_chan);
        return ret;
    }
    
    ret = rmt_enable(s_led_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Status LED initialized");
    return ESP_OK;
}

static void led_update(void) {
    if (!s_led_chan || !s_led_encoder) return;
    
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    
    rmt_transmit(s_led_chan, s_led_encoder, s_led_rgb, sizeof(s_led_rgb), &tx_config);
    rmt_tx_wait_all_done(s_led_chan, 100);
}

void led_status_set(led_status_t status) {
    if (status >= sizeof(LED_COLORS) / sizeof(LED_COLORS[0])) {
        status = LED_OFF;
    }
    
    s_led_rgb[0] = LED_COLORS[status][0];
    s_led_rgb[1] = LED_COLORS[status][1];
    s_led_rgb[2] = LED_COLORS[status][2];
    
    led_update();
}

void led_status_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    s_led_rgb[0] = g;
    s_led_rgb[1] = r;
    s_led_rgb[2] = b;
    
    led_update();
}

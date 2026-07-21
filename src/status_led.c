#include "status_led.h"
#include "app_config.h"
#include "config.h"
#include "mic_health.h"
#include "audio_pipeline.h"

#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "status_led";

// ---------------------------------------------------------------------------
// WS2812B RMT encoder
// ---------------------------------------------------------------------------
// Resolution: 10 MHz → 1 tick = 100 ns
// T0H = 3 ticks (300 ns), T0L = 9 ticks (900 ns)
// T1H = 9 ticks (900 ns), T1L = 3 ticks (300 ns)
// Reset: line low ≥ 50 µs; we achieve this by delaying 1 ms between calls.

static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_bytes_enc = NULL;

static esp_err_t ws2812_encoder_init(void)
{
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {
            .level0    = 1, .duration0 = 3,  // T0H 300 ns
            .level1    = 0, .duration1 = 9,  // T0L 900 ns
        },
        .bit1 = {
            .level0    = 1, .duration0 = 9,  // T1H 900 ns
            .level1    = 0, .duration1 = 3,  // T1L 300 ns
        },
        .flags.msb_first = 1,
    };
    return rmt_new_bytes_encoder(&enc_cfg, &s_bytes_enc);
}

// Write a single pixel. Color bytes are in GRB order (WS2812B order).
static void ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    rmt_transmit(s_rmt_chan, s_bytes_enc, grb, sizeof(grb), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_chan, pdMS_TO_TICKS(10));
    // WS2812B reset: hold data line low ≥ 50 µs.
    // The task delay between calls is always >> 50 µs, so no extra wait needed.
}

static void ws2812_off(void) { ws2812_set(0, 0, 0); }

// ---------------------------------------------------------------------------
// Scale a channel by the configured brightness (0-255)
// ---------------------------------------------------------------------------
static inline uint8_t dim(uint8_t v)
{
    return (uint8_t)((uint32_t)v * g_config.led_brightness / 255);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
static volatile atomic_int s_state = LED_CONNECTING;
static volatile atomic_int s_flash_ticks = 0;

void status_led_set(led_state_t state)
{
    atomic_store(&s_state, (int)state);
}

led_state_t status_led_get(void)
{
    return (led_state_t)atomic_load(&s_state);
}

void status_led_flash(void)
{
    atomic_store(&s_flash_ticks, 6);  // 6 × 50 ms = 300 ms of rapid white flashing
}

static void led_task(void *arg)
{
    bool toggle = false;
    int  tick   = 0;   // counts 50 ms ticks

    for (;;) {
        int flash = atomic_load(&s_flash_ticks);
        if (flash > 0) {
            // Brief acknowledgment flash overrides normal state rendering,
            // then falls back to it automatically once the counter runs out.
            ws2812_set(dim(255), dim(255), dim(255));
            vTaskDelay(pdMS_TO_TICKS(25));
            ws2812_off();
            vTaskDelay(pdMS_TO_TICKS(25));
            atomic_fetch_sub(&s_flash_ticks, 1);
            continue;
        }

        led_state_t state = (led_state_t)atomic_load(&s_state);

        // A dead or missing mic overrides the two "all is well" states with
        // a magenta blink; WiFi/setup problems are more urgent and keep
        // their patterns.
        if ((state == LED_CONNECTED || state == LED_STREAMING) &&
            (!mic_health_ok() || !audio_pipeline_is_active())) {
            if (tick % 10 == 0) {
                toggle = !toggle;
                if (toggle) ws2812_set(dim(255), 0, dim(255));
                else        ws2812_off();
            }
            tick++;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        switch (state) {
        case LED_CONNECTING:
            // Blue, slow blink: toggle every 500 ms (10 ticks × 50 ms)
            if (tick % 10 == 0) {
                toggle = !toggle;
                if (toggle) ws2812_set(0, 0, dim(255));
                else        ws2812_off();
            }
            break;

        case LED_CONNECTED:
            // Solid green
            ws2812_set(0, dim(255), 0);
            break;

        case LED_STREAMING:
            // Green, blink: toggle every 500 ms (10 ticks × 50 ms)
            if (tick % 10 == 0) {
                toggle = !toggle;
                if (toggle) ws2812_set(0, dim(255), 0);
                else        ws2812_off();
            }
            break;

        case LED_ERROR:
            // Orange, blink: toggle every 500 ms (10 ticks × 50 ms)
            if (tick % 10 == 0) {
                toggle = !toggle;
                if (toggle) ws2812_set(dim(255), dim(80), 0);
                else        ws2812_off();
            }
            break;

        case LED_SETUP:
            // Red, slow blink: toggle every 500 ms (10 ticks × 50 ms).
            // Distinguishable from LED_ERROR by color (red vs orange).
            if (tick % 10 == 0) {
                toggle = !toggle;
                if (toggle) ws2812_set(dim(255), 0, 0);
                else        ws2812_off();
            }
            break;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
esp_err_t status_led_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num            = NEOPIXEL_GPIO,
        .clk_src             = RMT_CLK_SRC_DEFAULT,
        .resolution_hz       = 10 * 1000 * 1000,  // 10 MHz
        .mem_block_symbols   = 64,
        .trans_queue_depth   = 4,
    };
    esp_err_t ret = rmt_new_tx_channel(&chan_cfg, &s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ws2812_encoder_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ws2812_encoder_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_enable(s_rmt_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    xTaskCreatePinnedToCore(led_task, "led_task", 2048, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "NeoPixel on GPIO %d, brightness %d", NEOPIXEL_GPIO, g_config.led_brightness);
    return ESP_OK;
}

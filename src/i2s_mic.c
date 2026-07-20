#include "i2s_mic.h"
#include "app_config.h"
#include "config.h"
#include "audio_dsp.h"

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "i2s_mic";

static i2s_chan_handle_t s_rx_chan = NULL;
static audio_dsp_state_t s_dsp;

esp_err_t i2s_mic_init(void)
{
    // Create a new RX channel on I2S_PORT
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Standard I2S mode — INMP441 uses Philips/I2S standard
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(g_config.sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_SCK,
            .ws   = I2S_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_PIN_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    audio_dsp_init(&s_dsp);

    ESP_LOGI(TAG, "INMP441 ready — %lu Hz mono, WS=%d SCK=%d SD=%d",
             (unsigned long)g_config.sample_rate, I2S_PIN_WS, I2S_PIN_SCK, I2S_PIN_SD);
    return ESP_OK;
}

size_t i2s_mic_read(int16_t *buf, size_t count)
{
    // Philips MONO mode on ESP-IDF 6 returns interleaved stereo frames
    // [L0, R0, L1, R1, …] despite the MONO config. Request 2× bytes so
    // we read `chunk` full stereo frames, then keep only every other
    // sample to get back to the correct 1× playback speed.
    static int32_t raw[I2S_DMA_BUF_LEN * 2];

    size_t to_read = count;
    size_t out_idx = 0;

    while (to_read > 0) {
        size_t chunk = (to_read < I2S_DMA_BUF_LEN) ? to_read : I2S_DMA_BUF_LEN;
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(s_rx_chan, raw,
                                         chunk * 2 * sizeof(int32_t),
                                         &bytes_read, pdMS_TO_TICKS(200));
        if (ret != ESP_OK || bytes_read == 0) break;

        size_t raw_samples = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < raw_samples; i += 2) {
            buf[out_idx++] = (int16_t)(raw[i + 1] >> g_config.gain_shift);
        }
        to_read -= raw_samples / 2;
    }

    audio_dsp_process(&s_dsp, buf, out_idx);
    return out_idx;
}

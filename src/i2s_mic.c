#include "i2s_mic.h"
#include "app_config.h"
#include "config.h"
#include "audio_dsp.h"

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include <stdatomic.h>

static const char *TAG = "i2s_mic";

static i2s_chan_handle_t s_rx_chan = NULL;
static audio_dsp_state_t s_dsp;
static atomic_uint       s_dma_overflows = 0;
// Which of the two duplicated slots i2s_channel_read() returns per frame
// (see the comment in i2s_mic_read()) actually carries mic data. Empirically
// verified via raw DMA dump: Philips format (bit_shift=true, INMP441) puts
// it in the second slot; MSB format (bit_shift=false, SPH0645) puts it in
// the first — the two slot configs differ in more than just bit timing.
static size_t s_data_slot = 1;

static bool IRAM_ATTR on_recv_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    atomic_fetch_add(&s_dma_overflows, 1);
    return false;
}

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

    // INMP441 follows the Philips I2S standard (data delayed one BCLK after
    // the WS edge). The SPH0645 has a timing quirk — it clocks data out one
    // BCLK early — which lines up with the MSB (left-justified) slot format
    // instead. Same pins, same wiring (L/R / SEL to GND) for both.
    bool sph = g_config.mic_model == MIC_MODEL_SPH0645;
    s_data_slot = sph ? 0 : 1;
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(g_config.sample_rate),
        .slot_cfg = sph ? (i2s_std_slot_config_t)
                          I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                          I2S_SLOT_MODE_MONO)
                        : (i2s_std_slot_config_t)
                          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
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

    i2s_event_callbacks_t cbs = { .on_recv_q_ovf = on_recv_q_ovf };
    ret = i2s_channel_register_event_callback(s_rx_chan, &cbs, NULL);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "i2s_channel_register_event_callback failed: %s", esp_err_to_name(ret));

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    audio_dsp_init(&s_dsp);

    ESP_LOGI(TAG, "%s ready — %lu Hz mono, WS=%d SCK=%d SD=%d",
             sph ? "SPH0645" : "INMP441",
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
            buf[out_idx++] = (int16_t)(raw[i + s_data_slot] >> g_config.gain_shift);
        }
        to_read -= raw_samples / 2;
    }

    audio_dsp_process(&s_dsp, buf, out_idx);
    return out_idx;
}

uint32_t i2s_mic_get_dma_overflows(void)
{
    return atomic_load(&s_dma_overflows);
}

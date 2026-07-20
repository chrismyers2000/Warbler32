#include "audio_pipeline.h"
#include "i2s_mic.h"
#include "usb_mic.h"
#include "app_config.h"
#include "config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <arpa/inet.h>

static const char *TAG = "pipeline";

static RingbufHandle_t s_ringbuf  = NULL;
static volatile int16_t s_peak   = 0;
static size_t (*s_mic_read)(int16_t *buf, size_t count) = i2s_mic_read;

static void i2s_reader_task(void *arg)
{
    static int16_t pcm[RTP_SAMPLES_PER_PACKET];

    ESP_LOGI(TAG, "audio reader task started");

    for (;;) {
        size_t got = s_mic_read(pcm, RTP_SAMPLES_PER_PACKET);
        if (got == 0) continue;

        // Track peak amplitude for the level monitor
        int16_t pk = 0;
        for (size_t i = 0; i < got; i++) {
            int16_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > pk) pk = a;
        }
        s_peak = pk;

        // Warn on clipping, rate-limited to once per second
        if (pk >= 32767) {
            static uint32_t s_clip_logged_ms = 0;
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms - s_clip_logged_ms >= 1000) {
                ESP_LOGW(TAG, "audio clipping — reduce gain_shift or gain_mult");
                s_clip_logged_ms = now_ms;
            }
        }

        // Convert to big-endian (RTP L16 requires network byte order)
        for (size_t i = 0; i < got; i++)
            pcm[i] = (int16_t)htons((uint16_t)pcm[i]);

        // Send to ring buffer; drop oldest data if full (non-blocking send)
        size_t bytes = got * sizeof(int16_t);
        BaseType_t ok = xRingbufferSend(s_ringbuf, pcm, bytes, 0);
        if (ok != pdTRUE) {
            // Ring buffer full — evict oldest item, then retry
            size_t dummy_size;
            void *dummy = xRingbufferReceive(s_ringbuf, &dummy_size, 0);
            if (dummy) vRingbufferReturnItem(s_ringbuf, dummy);
            ok = xRingbufferSend(s_ringbuf, pcm, bytes, 0);
            if (ok != pdTRUE)
                ESP_LOGW(TAG, "ring buffer full, audio dropped");
        }
    }
}

esp_err_t audio_pipeline_start(void)
{
    // Allocate ring buffer in PSRAM
    s_ringbuf = xRingbufferCreateWithCaps(
        PIPELINE_BUF_BYTES,
        RINGBUF_TYPE_BYTEBUF,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_ringbuf == NULL) {
        ESP_LOGW(TAG, "PSRAM ring buffer failed, falling back to internal RAM");
        s_ringbuf = xRingbufferCreate(PIPELINE_BUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    }

    if (s_ringbuf == NULL) {
        ESP_LOGE(TAG, "failed to allocate ring buffer");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ring buffer: %u bytes", (unsigned)PIPELINE_BUF_BYTES);

    esp_err_t ret;
    if (g_config.audio_source == AUDIO_SOURCE_USB) {
        s_mic_read = usb_mic_read;
        ret = usb_mic_init();
    } else {
        s_mic_read = i2s_mic_read;
        ret = i2s_mic_init();
    }
    if (ret != ESP_OK) return ret;

    BaseType_t ok = xTaskCreatePinnedToCore(
        i2s_reader_task, "i2s_reader",
        TASK_I2S_STACK, NULL,
        TASK_I2S_PRIORITY, NULL,
        TASK_I2S_CORE);

    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void audio_pipeline_flush(void)
{
    size_t size;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_ringbuf, &size, 0, PIPELINE_BUF_BYTES)) != NULL)
        vRingbufferReturnItem(s_ringbuf, item);
}

int audio_pipeline_get_peak_pct(void)
{
    int16_t pk = s_peak;
    return (pk * 100) / 32767;
}

size_t audio_pipeline_read(uint8_t *buf, size_t bytes, uint32_t timeout_ms)
{
    size_t received = 0;
    uint8_t *dst = buf;
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while (received < bytes) {
        size_t chunk_size;
        void *chunk = xRingbufferReceiveUpTo(s_ringbuf,
                                             &chunk_size,
                                             timeout,
                                             bytes - received);
        if (chunk == NULL) break;

        memcpy(dst, chunk, chunk_size);
        vRingbufferReturnItem(s_ringbuf, chunk);
        dst      += chunk_size;
        received += chunk_size;
        timeout   = 0;  // subsequent grabs are non-blocking
    }

    return received;
}

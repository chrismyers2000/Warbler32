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
#include "freertos/semphr.h"
#include <arpa/inet.h>
#include <stdatomic.h>

static const char *TAG = "pipeline";

// One ring buffer per subscribed reader (i.e. per streaming RTSP client).
// The reader task fans captured audio out to every active buffer, so each
// client gets the full stream instead of stealing samples from the other.
typedef struct {
    RingbufHandle_t rb;
    bool            active;
    bool            count_overruns;
} pipeline_reader_t;

static pipeline_reader_t s_readers[PIPELINE_MAX_READERS];
static SemaphoreHandle_t s_readers_mtx = NULL;

static volatile int16_t s_peak = 0;
static atomic_uint      s_overruns = 0;
static atomic_uint      s_chunk_count = 0;
static atomic_bool      s_active = false;
static size_t (*s_mic_read)(int16_t *buf, size_t count) = i2s_mic_read;

static RingbufHandle_t create_ringbuf(void)
{
    RingbufHandle_t rb = xRingbufferCreateWithCaps(
        PIPELINE_BUF_BYTES,
        RINGBUF_TYPE_BYTEBUF,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rb == NULL) {
        ESP_LOGW(TAG, "PSRAM ring buffer failed, falling back to internal RAM");
        rb = xRingbufferCreate(PIPELINE_BUF_BYTES, RINGBUF_TYPE_BYTEBUF);
    }
    return rb;
}

static void i2s_reader_task(void *arg)
{
    static int16_t pcm[RTP_SAMPLES_PER_PACKET];

    ESP_LOGI(TAG, "audio reader task started");

    for (;;) {
        size_t got = s_mic_read(pcm, RTP_SAMPLES_PER_PACKET);
        if (got == 0) {
            // A backend returning 0 with no internal blocking (e.g. USB
            // mic disconnected mid-stream — usb_mic_read() checks a null
            // handle and returns instantly) would otherwise busy-spin this
            // pinned task at full priority, starving the idle task on its
            // core and tripping the watchdog. I2S never hits this path
            // (its read call always blocks internally), but this guards
            // any backend that can return 0 without blocking.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        atomic_fetch_add(&s_chunk_count, 1);

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

        size_t bytes = got * sizeof(int16_t);

        xSemaphoreTake(s_readers_mtx, portMAX_DELAY);
        for (int r = 0; r < PIPELINE_MAX_READERS; r++) {
            if (!s_readers[r].active) continue;

            // Non-blocking send; a slow client only loses its own audio.
            // On full, evict the oldest chunk and retry once.
            if (xRingbufferSend(s_readers[r].rb, pcm, bytes, 0) != pdTRUE) {
                size_t dummy_size;
                void *dummy = xRingbufferReceiveUpTo(s_readers[r].rb, &dummy_size,
                                                     0, bytes);
                if (dummy) vRingbufferReturnItem(s_readers[r].rb, dummy);
                if (xRingbufferSend(s_readers[r].rb, pcm, bytes, 0) != pdTRUE)
                    ESP_LOGW(TAG, "reader %d buffer full, audio dropped", r);
                if (s_readers[r].count_overruns)
                    atomic_fetch_add(&s_overruns, 1);
            }
        }
        xSemaphoreGive(s_readers_mtx);
    }
}

// Inits the configured mic and spawns the reader task. Returns
// ESP_ERR_NOT_FOUND/ESP_ERR_NOT_SUPPORTED for hardware-absent/mismatched
// conditions (no device, or no alt setting matches the configured sample
// rate) — those are retried by the caller rather than treated as fatal.
static esp_err_t mic_start_and_spawn_reader(void)
{
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
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    atomic_store(&s_active, true);
    return ESP_OK;
}

static void mic_retry_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        esp_err_t ret = mic_start_and_spawn_reader();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "microphone found, audio pipeline started");
            vTaskDelete(NULL);
        } else if (ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "mic retry failed: %s", esp_err_to_name(ret));
        }
    }
}

esp_err_t audio_pipeline_start(void)
{
    s_readers_mtx = xSemaphoreCreateMutex();
    if (s_readers_mtx == NULL) return ESP_ERR_NO_MEM;

    esp_err_t ret = mic_start_and_spawn_reader();
    if (ret == ESP_OK) return ESP_OK;

    if (ret == ESP_ERR_NOT_FOUND || ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "no usable mic found (%s) — WiFi/web UI stay up, retrying "
                      "in the background", esp_err_to_name(ret));
        xTaskCreatePinnedToCore(
            mic_retry_task, "mic_retry",
            TASK_MIC_RETRY_STACK, NULL,
            TASK_MIC_RETRY_PRIORITY, NULL,
            TASK_MIC_RETRY_CORE);
        return ESP_OK;
    }

    return ret;
}

bool audio_pipeline_is_active(void)
{
    return atomic_load(&s_active);
}

void audio_pipeline_mark_inactive(void)
{
    atomic_store(&s_active, false);
}

int audio_pipeline_subscribe(bool count_overruns)
{
    int slot = -1;

    // Pipeline never starts in setup-AP mode, but HTTP endpoints that
    // subscribe (browser preview) are still reachable there.
    if (s_readers_mtx == NULL) return -1;

    xSemaphoreTake(s_readers_mtx, portMAX_DELAY);
    for (int r = 0; r < PIPELINE_MAX_READERS; r++) {
        if (s_readers[r].active) continue;
        if (s_readers[r].rb == NULL)
            s_readers[r].rb = create_ringbuf();
        if (s_readers[r].rb != NULL) {
            s_readers[r].count_overruns = count_overruns;
            s_readers[r].active = true;
            slot = r;
        }
        break;
    }
    xSemaphoreGive(s_readers_mtx);

    if (slot < 0) ESP_LOGW(TAG, "no free reader slot");
    else          ESP_LOGI(TAG, "reader %d subscribed", slot);
    return slot;
}

void audio_pipeline_unsubscribe(int reader)
{
    if (reader < 0 || reader >= PIPELINE_MAX_READERS || s_readers_mtx == NULL) return;

    xSemaphoreTake(s_readers_mtx, portMAX_DELAY);
    s_readers[reader].active = false;
    // Keep the ring buffer allocated for reuse, but drain it so the next
    // subscriber starts with fresh audio instead of stale samples.
    size_t size;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_readers[reader].rb, &size, 0,
                                          PIPELINE_BUF_BYTES)) != NULL)
        vRingbufferReturnItem(s_readers[reader].rb, item);
    xSemaphoreGive(s_readers_mtx);

    ESP_LOGI(TAG, "reader %d unsubscribed", reader);
}

int audio_pipeline_get_peak_pct(void)
{
    int16_t pk = s_peak;
    return (pk * 100) / 32767;
}

uint32_t audio_pipeline_get_overruns(void)
{
    return atomic_load(&s_overruns);
}

uint32_t audio_pipeline_get_chunk_count(void)
{
    return atomic_load(&s_chunk_count);
}

size_t audio_pipeline_read(int reader, uint8_t *buf, size_t bytes, uint32_t timeout_ms)
{
    if (reader < 0 || reader >= PIPELINE_MAX_READERS) return 0;
    RingbufHandle_t rb = s_readers[reader].rb;
    if (rb == NULL) return 0;

    size_t received = 0;
    uint8_t *dst = buf;
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while (received < bytes) {
        size_t chunk_size;
        void *chunk = xRingbufferReceiveUpTo(rb, &chunk_size, timeout,
                                             bytes - received);
        if (chunk == NULL) break;

        memcpy(dst, chunk, chunk_size);
        vRingbufferReturnItem(rb, chunk);
        dst      += chunk_size;
        received += chunk_size;
        timeout   = 0;  // subsequent grabs are non-blocking
    }

    return received;
}

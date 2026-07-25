#include "log_stream.h"
#include "config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

// True circular overwrite buffer, not a FreeRTOS RingbufHandle_t: those are
// bounded FIFOs that drop/block on full rather than age out old data, and
// only exist once something has subscribed — wrong shape for "always be
// capturing a rolling window of recent output, ready the moment someone
// looks." s_write_pos is a monotonically increasing total-bytes-ever-
// written counter; each reader tracks its own read_pos into that same
// space and both indices are reduced mod LOG_STREAM_BUF_BYTES to address
// s_buf. Capturing runs unconditionally from log_stream_init() onward,
// independent of whether any reader is currently subscribed, so a fresh
// subscriber immediately sees however much recent history is still within
// the window instead of starting from a blank buffer.
typedef struct {
    bool     active;
    uint32_t read_pos;
} log_reader_t;

static char              *s_buf = NULL;
static uint32_t           s_write_pos = 0;
static log_reader_t       s_readers[LOG_STREAM_MAX_READERS];
static SemaphoreHandle_t  s_mutex = NULL;
static vprintf_like_t     s_prev_vprintf = NULL;

// The installed vprintf hook — called from whatever task context logged the
// line, for every single ESP_LOG* call in the whole firmware (and ESP-IDF's
// own internals: WiFi, mbedTLS, etc). Must never block the caller and must
// never itself call ESP_LOG* (the format-and-dispatch that invokes this
// function isn't safe to re-enter from inside itself).
static int log_stream_vprintf(const char *fmt, va_list args)
{
    // Preserve real console/UART output exactly as before this hook existed.
    va_list copy;
    va_copy(copy, args);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);

    char line[256];
    int len = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (len <= 0 || s_buf == NULL) return ret;
    if (len >= (int)sizeof(line)) len = sizeof(line) - 1;  // truncated, still useful

    // Best-effort, non-blocking — a log call must never stall waiting on a
    // mutex. Dropping an occasional line under contention is fine for a
    // live diagnostic tail.
    if (xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        for (int i = 0; i < len; i++)
            s_buf[(s_write_pos + (uint32_t)i) % LOG_STREAM_BUF_BYTES] = line[i];
        s_write_pos += (uint32_t)len;
        xSemaphoreGive(s_mutex);
    }

    return ret;
}

esp_err_t log_stream_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    s_buf = heap_caps_malloc(LOG_STREAM_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        s_buf = heap_caps_malloc(LOG_STREAM_BUF_BYTES, MALLOC_CAP_8BIT);
        if (s_buf == NULL) return ESP_ERR_NO_MEM;
    }

    s_prev_vprintf = esp_log_set_vprintf(log_stream_vprintf);
    return ESP_OK;
}

int log_stream_subscribe(void)
{
    int slot = -1;
    if (s_mutex == NULL) return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int r = 0; r < LOG_STREAM_MAX_READERS; r++) {
        if (s_readers[r].active) continue;
        s_readers[r].active = true;
        // Start from however much history is still in the window (or the
        // very beginning, on a device that hasn't logged 8KB yet) rather
        // than only showing lines from this exact moment forward.
        s_readers[r].read_pos = (s_write_pos > LOG_STREAM_BUF_BYTES)
            ? s_write_pos - LOG_STREAM_BUF_BYTES : 0;
        slot = r;
        break;
    }
    xSemaphoreGive(s_mutex);

    return slot;
}

void log_stream_unsubscribe(int reader)
{
    if (reader < 0 || reader >= LOG_STREAM_MAX_READERS || s_mutex == NULL) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_readers[reader].active = false;
    xSemaphoreGive(s_mutex);
}

size_t log_stream_read(int reader, char *buf, size_t max_len, uint32_t timeout_ms)
{
    if (reader < 0 || reader >= LOG_STREAM_MAX_READERS) return 0;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        size_t n = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        uint32_t avail = s_write_pos - s_readers[reader].read_pos;
        // Fell behind more than a full window (producer wrapped around
        // before we caught up) — jump to the oldest data still actually
        // present rather than replaying stale/overwritten bytes.
        if (avail > LOG_STREAM_BUF_BYTES) {
            s_readers[reader].read_pos = s_write_pos - LOG_STREAM_BUF_BYTES;
            avail = LOG_STREAM_BUF_BYTES;
        }
        n = avail < max_len ? avail : max_len;
        for (size_t i = 0; i < n; i++)
            buf[i] = s_buf[(s_readers[reader].read_pos + (uint32_t)i) % LOG_STREAM_BUF_BYTES];
        s_readers[reader].read_pos += (uint32_t)n;
        xSemaphoreGive(s_mutex);

        if (n > 0) return n;
        if (xTaskGetTickCount() >= deadline) return 0;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

size_t log_stream_dump(char *buf, size_t max_len)
{
    if (s_buf == NULL) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t oldest = (s_write_pos > LOG_STREAM_BUF_BYTES) ? s_write_pos - LOG_STREAM_BUF_BYTES : 0;
    uint32_t avail  = s_write_pos - oldest;
    size_t   n      = avail < max_len ? avail : max_len;
    // If the caller's buffer is smaller than what's available, keep the
    // most recent n bytes rather than the oldest.
    uint32_t start  = s_write_pos - n;
    for (size_t i = 0; i < n; i++)
        buf[i] = s_buf[(start + (uint32_t)i) % LOG_STREAM_BUF_BYTES];
    xSemaphoreGive(s_mutex);

    return n;
}

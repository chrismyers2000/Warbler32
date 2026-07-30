#include "battery_history.h"
#include "config.h"

#include "app_config.h"
#include "battery_monitor.h"
#include "mppt_monitor.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "batthist";

typedef struct {
    uint32_t t_sec;
    uint8_t  pct;
    uint8_t  charging;
} sample_t;

static sample_t         *s_buf;
static size_t            s_head;   // next write index
static size_t            s_count;  // valid entries, capped at BATT_HISTORY_MAX_SAMPLES
static SemaphoreHandle_t s_mutex;

static void record(uint8_t pct, bool charging)
{
    uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_buf[s_head] = (sample_t){ .t_sec = now_sec, .pct = pct, .charging = charging ? 1 : 0 };
    s_head = (s_head + 1) % BATT_HISTORY_MAX_SAMPLES;
    if (s_count < BATT_HISTORY_MAX_SAMPLES) s_count++;
    xSemaphoreGive(s_mutex);
}

static void history_task(void *arg)
{
    for (;;) {
        bool    present  = false;
        uint8_t pct      = 0;
        bool    charging = false;

        if (mppt_monitor_present()) {
            present  = true;
            pct      = mppt_monitor_percent();
            charging = mppt_monitor_charging();
        } else if (battery_monitor_present()) {
            present = true;
            uint16_t mv = battery_monitor_voltage_mv();
            int span = (int)g_config.batt_full_mv - (int)g_config.batt_low_mv;
            int p = 0;
            if (span > 0) {
                p = ((int)mv - (int)g_config.batt_low_mv) * 100 / span;
                if (p < 0)   p = 0;
                if (p > 100) p = 100;
            }
            pct = (uint8_t)p;
        }

        if (present) record(pct, charging);

        vTaskDelay(pdMS_TO_TICKS(BATT_HISTORY_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t battery_history_init(void)
{
    s_buf = heap_caps_malloc(sizeof(sample_t) * BATT_HISTORY_MAX_SAMPLES, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for %d samples", BATT_HISTORY_MAX_SAMPLES);
        return ESP_ERR_NO_MEM;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        heap_caps_free(s_buf);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        history_task, "batthist",
        TASK_BATT_HISTORY_STACK, NULL,
        TASK_BATT_HISTORY_PRIORITY, NULL,
        TASK_BATT_HISTORY_CORE);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

int battery_history_copy(battery_history_point_t *out, int max_count)
{
    uint32_t now_sec = (uint32_t)(esp_timer_get_time() / 1000000);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = (int)s_count;
    if (n > max_count) n = max_count;
    // Oldest sample is s_count-many slots behind s_head (wrapping); when the
    // buffer isn't full yet, that's simply index 0.
    size_t oldest = (s_count < BATT_HISTORY_MAX_SAMPLES)
                        ? 0
                        : s_head; // s_head is the next-write slot == oldest once full
    // Only the most recent max_count of them, so skip forward if n was capped.
    size_t start = (oldest + (s_count - n)) % BATT_HISTORY_MAX_SAMPLES;
    for (int i = 0; i < n; i++) {
        sample_t *s = &s_buf[(start + i) % BATT_HISTORY_MAX_SAMPLES];
        out[i].age_sec = (now_sec >= s->t_sec) ? (now_sec - s->t_sec) : 0;
        out[i].pct      = s->pct;
        out[i].charging = s->charging != 0;
    }
    xSemaphoreGive(s_mutex);

    return n;
}

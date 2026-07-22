#include "pipeline_watchdog.h"
#include "audio_pipeline.h"
#include "app_config.h"
#include "config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdbool.h>

static const char *TAG = "pl_watchdog";

// 0 = not currently stalled; otherwise the esp_timer_get_time() at which the
// current stall streak began (for pipeline_watchdog_stall_secs() only — the
// task below tracks its own pass/fail state independently via stalled_checks).
static _Atomic int64_t s_stall_since_us = 0;

static void watchdog_task(void *arg)
{
    uint32_t last_count = 0;
    int stalled_checks = 0;
    bool baselined = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PIPELINE_WATCHDOG_CHECK_INTERVAL_MS));

        if (!g_config.watchdog_enabled || !audio_pipeline_is_active()) {
            stalled_checks = 0;
            baselined = false;
            atomic_store(&s_stall_since_us, 0);
            continue;
        }

        uint32_t count = audio_pipeline_get_chunk_count();
        if (!baselined) {
            // First check after (re)becoming active: just record where the
            // counter starts, don't judge progress over a window that began
            // before we were watching.
            last_count = count;
            baselined = true;
            continue;
        }

        if (count == last_count) {
            if (stalled_checks == 0)
                atomic_store(&s_stall_since_us, esp_timer_get_time());
            stalled_checks++;
            ESP_LOGW(TAG, "no audio chunks produced (%d/%d checks)",
                     stalled_checks, PIPELINE_WATCHDOG_STALL_CHECKS);
            if (stalled_checks >= PIPELINE_WATCHDOG_STALL_CHECKS) {
                ESP_LOGE(TAG, "audio pipeline stalled for ~%d s — rebooting to recover",
                         (PIPELINE_WATCHDOG_CHECK_INTERVAL_MS * PIPELINE_WATCHDOG_STALL_CHECKS) / 1000);
                esp_restart();
            }
        } else {
            stalled_checks = 0;
            atomic_store(&s_stall_since_us, 0);
        }
        last_count = count;
    }
}

esp_err_t pipeline_watchdog_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        watchdog_task, "pl_watchdog",
        TASK_WATCHDOG_STACK, NULL,
        TASK_WATCHDOG_PRIORITY, NULL,
        TASK_WATCHDOG_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

uint32_t pipeline_watchdog_stall_secs(void)
{
    int64_t since = atomic_load(&s_stall_since_us);
    if (since == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - since) / 1000000);
}

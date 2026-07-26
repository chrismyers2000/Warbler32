#include "log_persist.h"
#include "log_stream.h"
#include "app_config.h"
#include "config.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>

static const char *TAG = "log_persist";

#define SPIFFS_PARTITION_LABEL "spiffs"
#define CURRENT_LOG_PATH       "/spiffs/current.log"

static bool              s_active         = false;
static FILE              *s_fp            = NULL;
static int                s_reader        = -1;
static size_t             s_written       = 0;
static volatile bool      s_stop_requested = false;
// Signaled by flush_task right before it self-deletes, so
// log_persist_set_enabled(false) can wait for teardown to actually finish
// (file closed, SPIFFS unmounted) instead of racing a fresh enable against
// cleanup still in flight. Created lazily on first use.
static SemaphoreHandle_t  s_stopped_sem   = NULL;

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_SW:         return "software (esp_restart)";
    case ESP_RST_PANIC:      return "PANIC / exception";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "other watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep sleep wake";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "efuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup (double exception)";
    default:                 return "unknown";
    }
}

// Drains new log content into current.log until asked to stop. Self-deletes
// rather than being killed externally via vTaskDelete() from another task —
// an external delete could catch this task mid-critical-section inside
// log_stream_read() (holding its mutex) and leave that mutex permanently
// held, deadlocking every future log_stream call. A short read timeout
// keeps shutdown latency low without sacrificing the batching that avoids
// hitting flash on every single log line.
static void flush_task(void *arg)
{
    char buf[512];
    while (!s_stop_requested) {
        size_t got = log_stream_read(s_reader, buf, sizeof(buf), 500);
        if (got == 0 || s_written >= LOG_PERSIST_MAX_BYTES) continue;
        fwrite(buf, 1, got, s_fp);
        fflush(s_fp);
        s_written += got;
    }

    log_stream_unsubscribe(s_reader);
    s_reader = -1;
    fclose(s_fp);
    s_fp = NULL;
    esp_vfs_spiffs_unregister(SPIFFS_PARTITION_LABEL);

    xSemaphoreGive(s_stopped_sem);
    vTaskDelete(NULL);
}

esp_err_t log_persist_set_enabled(bool enabled)
{
    if (enabled == s_active) return ESP_OK;

    if (enabled) {
        esp_vfs_spiffs_conf_t conf = {
            .base_path              = "/spiffs",
            .partition_label        = SPIFFS_PARTITION_LABEL,
            .max_files              = 3,
            .format_if_mount_failed = true,
        };
        esp_err_t ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
            return ret;
        }

        // Rotate: whatever was current.log from the last time this was
        // enabled becomes previous.log, so there's always exactly one
        // prior session's log available regardless of how this one ends.
        remove("/spiffs/previous.log");
        rename(CURRENT_LOG_PATH, "/spiffs/previous.log");

        s_fp = fopen(CURRENT_LOG_PATH, "w");
        if (!s_fp) {
            ESP_LOGW(TAG, "could not open %s", CURRENT_LOG_PATH);
            esp_vfs_spiffs_unregister(SPIFFS_PARTITION_LABEL);
            return ESP_FAIL;
        }
        s_written = 0;

        s_reader = log_stream_subscribe();
        if (s_reader < 0) {
            ESP_LOGW(TAG, "no free log_stream reader slot");
            fclose(s_fp);
            s_fp = NULL;
            esp_vfs_spiffs_unregister(SPIFFS_PARTITION_LABEL);
            return ESP_ERR_NO_MEM;
        }

        if (s_stopped_sem == NULL) s_stopped_sem = xSemaphoreCreateBinary();
        s_stop_requested = false;

        // Answers "did it crash or was this a clean reboot" without
        // needing to guess from log content alone — logged through the
        // normal ESP_LOG path, so it lands in current.log itself as soon
        // as the flush task picks it up.
        ESP_LOGW(TAG, "log persistence enabled — last reset reason: %s", reset_reason_str());

        if (xTaskCreatePinnedToCore(flush_task, "log_persist",
                TASK_LOG_PERSIST_STACK, NULL,
                TASK_LOG_PERSIST_PRIORITY, NULL, TASK_LOG_PERSIST_CORE) != pdPASS) {
            log_stream_unsubscribe(s_reader);
            s_reader = -1;
            fclose(s_fp);
            s_fp = NULL;
            esp_vfs_spiffs_unregister(SPIFFS_PARTITION_LABEL);
            return ESP_ERR_NO_MEM;
        }

        s_active = true;
    } else {
        s_stop_requested = true;
        xSemaphoreTake(s_stopped_sem, portMAX_DELAY);  // wait for flush_task's own cleanup + self-delete
        s_active = false;
        ESP_LOGI(TAG, "log persistence disabled");
    }

    g_config.log_persist_enabled = enabled ? 1 : 0;
    app_config_save();
    return ESP_OK;
}

esp_err_t log_persist_init(void)
{
    if (!g_config.log_persist_enabled) return ESP_OK;

    esp_err_t ret = log_persist_set_enabled(true);
    if (ret != ESP_OK) {
        // Don't let a mount failure re-arm itself on every future boot —
        // fall back to disabled so the user has to explicitly retry.
        g_config.log_persist_enabled = 0;
        app_config_save();
    }
    return ESP_OK;
}

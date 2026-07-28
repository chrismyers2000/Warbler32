#include "auto_reboot.h"
#include "app_config.h"
#include "config.h"
#include "time_sync.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "auto_reboot";

static void auto_reboot_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AUTO_REBOOT_CHECK_INTERVAL_MS));

        if (!g_config.auto_reboot_enabled) continue;

        struct tm now;
        // Never fire before the first successful NTP sync — pre-sync local
        // time is meaningless (Jan 1 1970 + whatever offset), and would
        // otherwise coincidentally match the target minute at some point.
        if (!time_sync_get_local_tm(&now)) continue;

        int now_min = now.tm_hour * 60 + now.tm_min;
        if (now_min == (int)g_config.auto_reboot_time_min) {
            ESP_LOGW(TAG, "scheduled reboot at configured local time %02u:%02u",
                     (unsigned)(g_config.auto_reboot_time_min / 60),
                     (unsigned)(g_config.auto_reboot_time_min % 60));
            esp_restart();
        }
    }
}

esp_err_t auto_reboot_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        auto_reboot_task, "auto_reboot",
        TASK_AUTO_REBOOT_STACK, NULL,
        TASK_AUTO_REBOOT_PRIORITY, NULL,
        TASK_AUTO_REBOOT_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

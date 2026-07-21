#include "mic_health.h"
#include "config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <stdatomic.h>

static const char *TAG = "mic_health";

static atomic_bool     s_started;       // first chunk seen
static _Atomic int64_t s_last_live_us;  // when the signal last moved
static atomic_bool     s_flagged;       // warning logged for current outage

void mic_health_report(const int16_t *buf, size_t count)
{
    if (count == 0) return;

    int16_t mn = buf[0], mx = buf[0];
    for (size_t i = 1; i < count; i++) {
        if (buf[i] < mn) mn = buf[i];
        if (buf[i] > mx) mx = buf[i];
    }

    int64_t now = esp_timer_get_time();
    if (!atomic_load(&s_started)) {
        atomic_store(&s_last_live_us, now);
        atomic_store(&s_started, true);
    }

    if ((int32_t)mx - (int32_t)mn >= MIC_HEALTH_MIN_P2P) {
        atomic_store(&s_last_live_us, now);
        if (atomic_exchange(&s_flagged, false))
            ESP_LOGI(TAG, "mic signal restored");
    } else if (now - atomic_load(&s_last_live_us) >=
               (int64_t)MIC_HEALTH_TIMEOUT_MS * 1000) {
        if (!atomic_exchange(&s_flagged, true))
            ESP_LOGW(TAG, "mic flatlined for %d s — check mic wiring/power",
                     MIC_HEALTH_TIMEOUT_MS / 1000);
    }
}

bool mic_health_ok(void)
{
    if (!atomic_load(&s_started)) return true;
    return esp_timer_get_time() - atomic_load(&s_last_live_us)
           < (int64_t)MIC_HEALTH_TIMEOUT_MS * 1000;
}

uint32_t mic_health_silent_secs(void)
{
    if (!atomic_load(&s_started)) return 0;
    int64_t d = esp_timer_get_time() - atomic_load(&s_last_live_us);
    if (d < (int64_t)MIC_HEALTH_TIMEOUT_MS * 1000) return 0;
    return (uint32_t)(d / 1000000);
}

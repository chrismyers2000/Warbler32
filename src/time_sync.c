#include "time_sync.h"
#include "app_config.h"
#include "config.h"

#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "time_sync";

static bool year_is_sane(time_t t)
{
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    return (tm_utc.tm_year + 1900) >= TIME_SYNC_SANE_YEAR;
}

esp_err_t time_sync_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(g_config.ntp_server);
    // Needed so time_sync_now() below can esp_netif_sntp_sync_wait() on a
    // later manual resync — doesn't make this call itself block.
    cfg.wait_for_sync = true;
    esp_err_t ret = esp_netif_sntp_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "syncing time from %s", g_config.ntp_server);
    return ESP_OK;
}

esp_err_t time_sync_now(TickType_t tout)
{
    esp_err_t ret = esp_netif_sntp_start();
    if (ret != ESP_OK) return ret;
    return esp_netif_sntp_sync_wait(tout);
}

bool time_sync_is_synced(void)
{
    return year_is_sane(time(NULL));
}

bool time_sync_get_local_tm(struct tm *tm_out)
{
    time_t now = time(NULL);
    if (!year_is_sane(now)) return false;
    time_t local = now + (time_t)g_config.utc_offset_min * 60;
    gmtime_r(&local, tm_out);
    return true;
}

void time_sync_format_local(char *out, size_t out_len)
{
    struct tm tm_local;
    if (!time_sync_get_local_tm(&tm_local)) {
        strlcpy(out, "not synced", out_len);
        return;
    }
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tm_local);
}

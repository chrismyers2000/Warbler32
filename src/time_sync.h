#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts the SNTP client against g_config.ntp_server. Call once, after WiFi
// is up — syncs in the background from here on, including periodic re-sync,
// with no further calls needed. Safe to call again (e.g. after the NTP
// server field changes in the web UI) to pick up the new server.
esp_err_t time_sync_start(void);

// Forces an immediate resync attempt, blocking up to `tout` ticks for it to
// land. For the Diagnostics tab's "Sync Now" button.
esp_err_t time_sync_now(TickType_t tout);

// True once the system clock holds a real SNTP-derived time, as opposed to
// the Jan 1 1970 epoch every boot starts at before the first sync completes.
bool time_sync_is_synced(void);

// Local time (system UTC clock + g_config.utc_offset_min, no DST) as broken-
// down fields. Returns false (tm_out untouched) if not yet synced.
bool time_sync_get_local_tm(struct tm *tm_out);

// Local time formatted "YYYY-MM-DD HH:MM:SS", or "not synced" if
// time_sync_is_synced() is false. Always null-terminates within out_len.
void time_sync_format_local(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

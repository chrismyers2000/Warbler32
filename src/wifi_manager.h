#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_start(void);

// True whenever an AP is currently being broadcast — either the original
// "no saved network" setup AP, or the backup AP described below.
bool wifi_manager_is_ap_mode(void);

// True specifically for the backup AP case (WiFi was joined successfully at
// some point, then became unreachable) — as opposed to the original "no
// saved network yet" setup AP. Both broadcast the same AP; this is only for
// telling the two situations apart in the web UI/logs. Concurrent with STA,
// which keeps retrying the saved network the whole time.
bool wifi_manager_is_fallback_mode(void);

// Manually advances the currently-broadcasting AP (setup or backup) to the
// next channel in {1, 6, 11}. No-op (returns 0) unless an AP is up.
uint8_t wifi_manager_cycle_ap_channel(void);

// BOOT-button double-tap handler: brings up the backup AP if WiFi is
// currently connected or retrying (leaving STA untouched), tears it back
// down if it's already up because of a prior timeout or toggle, or is a
// no-op if the only AP up is the original "no saved network" setup AP —
// turning that one off would strand the device with no network to fall
// back to.
void wifi_manager_toggle_fallback_ap(void);

// Pushes g_config.wifi_tx_power_dbm to the radio. Safe to call any time
// after wifi_manager_start() — used both at startup and to apply a change
// from the web UI live, without a reboot.
void wifi_manager_apply_tx_power(void);

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} wifi_scan_result_t;

// Blocking scan for nearby networks, deduped by SSID (strongest RSSI kept)
// and sorted strongest-first. Works whenever the STA interface is present,
// which is now always (see WIFI_MODE_APSTA note in wifi_manager.c) — idle,
// connected, or mid-fallback. Returns the number of results written to
// `out` (capped at max_results), or -1 if the scan itself failed (e.g. STA
// caught mid-connect) — the caller should surface that to the user rather
// than silently falling back to anything.
int wifi_manager_scan(wifi_scan_result_t *out, int max_results);

#ifdef __cplusplus
}
#endif

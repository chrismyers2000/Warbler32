#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_start(void);

// True if the device couldn't join a real network and fell back to
// broadcasting its own setup AP (see WIFI_AP_SSID/WIFI_AP_PASSWORD).
bool wifi_manager_is_ap_mode(void);

// Manually advances the setup AP to the next channel in {1, 6, 11}. No-op
// (returns 0) unless currently broadcasting the setup AP.
uint8_t wifi_manager_cycle_ap_channel(void);

#ifdef __cplusplus
}
#endif

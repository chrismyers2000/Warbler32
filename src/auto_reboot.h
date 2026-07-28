#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Optional scheduled reboot, independent of device health — see
// AUTO_REBOOT_* in config.h. Spawns a background task; toggling
// auto_reboot_enabled or auto_reboot_interval_hours in the web UI takes
// effect on the next check without a reboot of its own.
esp_err_t auto_reboot_start(void);

#ifdef __cplusplus
}
#endif

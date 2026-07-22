#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Liveness watchdog for the audio reader task. See the block comment above
// PIPELINE_WATCHDOG_* in config.h for how this differs from mic_health.h.
// Spawns a background task; no-op reads of app_config's watchdog_enabled
// happen every check interval, so toggling it in the web UI takes effect on
// the next check without a reboot.
esp_err_t pipeline_watchdog_start(void);

// Seconds since the pipeline last made forward progress, or 0 if healthy,
// disabled, or not yet active. For /status display only — the task itself
// tracks stall state independently.
uint32_t pipeline_watchdog_stall_secs(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors the live log (see log_stream.h) to flash so it survives a
// reboot — call once at boot, after log_stream_init(). Off by default
// (app_config.h's log_persist_enabled); if it was left enabled last
// session, this starts it back up via log_persist_set_enabled(true). Never
// fails boot: a SPIFFS mount problem just leaves persistence unavailable
// this session rather than panicking.
esp_err_t log_persist_init(void);

// The live toggle, callable any time (boot or from the web UI's
// Diagnostics tab). Mounts/rotates/opens on enable, fully tears down
// (task, file, SPIFFS mount) on disable — this subsystem touches nothing
// at all while disabled. Persists the new state via app_config_save() so
// leaving it on survives exactly the crash/reboot it's meant to catch.
// Returns an error if enabling fails (e.g. SPIFFS won't mount); disabling
// never fails.
esp_err_t log_persist_set_enabled(bool enabled);

// Path to the previous session's persisted log, for the web UI's
// "Download Previous Boot Log" button. The file may not exist (never
// enabled, or this is the very first boot) — callers should check.
#define LOG_PERSIST_PREVIOUS_PATH "/spiffs/previous.log"

#ifdef __cplusplus
}
#endif

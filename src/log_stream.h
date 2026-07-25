#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Captures ESP_LOG output (normally only visible over USB serial) into a
// ring buffer so the web UI can tail it live. Call as the very first thing
// in app_main() — before nvs_flash_init() — so boot-time logs are captured
// too; installing the hook has no dependencies of its own (PSRAM and the
// FreeRTOS scheduler are already up by the time app_main() runs).
esp_err_t log_stream_init(void);

// Single-reader, mirroring audio_pipeline.c's subscribe/read/unsubscribe
// shape: one browser tab tailing the log at a time. Returns a reader handle
// (always 0) or -1 if already in use.
int    log_stream_subscribe(void);
void   log_stream_unsubscribe(int reader);
size_t log_stream_read(int reader, char *buf, size_t max_len, uint32_t timeout_ms);

// One-shot snapshot of whatever's currently in the buffer (oldest first,
// most recent max_len bytes if there's more than that), for a "download the
// log" button. Independent of subscribe/read/unsubscribe above — doesn't
// touch reader state, so it works whether or not a live tail is already
// open, and doesn't consume the single reader slot. Returns bytes copied.
size_t log_stream_dump(char *buf, size_t max_len);

#ifdef __cplusplus
}
#endif

#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Board variant this firmware was built for ("quad" or "oct" PSRAM) —
// selects which GitHub release asset fits this hardware.
const char *ota_board_variant(void);

// ---------------------------------------------------------------------------
// OTA write engine — shared by the manual .bin upload and the GitHub
// installer. Feed it the image bytes in order; it buffers the image prefix,
// validates magic / chip id / project name BEFORE any flash is erased, then
// streams the rest into the inactive OTA slot. Single-instance: begin()
// fails while another OTA is in progress.
//
// On any error, *err_msg is set to a short user-displayable string and the
// engine cleans up after itself (no explicit abort needed unless the caller
// stops feeding mid-image).
// ---------------------------------------------------------------------------
esp_err_t ota_engine_begin(size_t total_size, const char **err_msg);
esp_err_t ota_engine_feed(const uint8_t *data, size_t len, const char **err_msg);
esp_err_t ota_engine_finish(const char **err_msg);   // verify + set boot partition
void      ota_engine_abort(void);                    // safe to call in any state

// ---------------------------------------------------------------------------
// GitHub release updater
// ---------------------------------------------------------------------------
typedef struct {
    bool   available;        // latest tag differs from the running version
    char   current[32];      // running firmware version
    char   latest[32];       // latest release tag
    size_t size;             // asset size in bytes
} ota_check_result_t;

// Query GitHub for the latest release and the asset matching this board's
// variant. Synchronous (a few seconds of TLS+HTTP). On failure returns an
// error and sets *err_msg. Caches the asset URL for ota_github_install().
esp_err_t ota_github_check(ota_check_result_t *out, const char **err_msg);

// Start installing the update found by the last successful check. Runs in a
// background task; poll ota_github_progress(). Fails if no check found an
// update or an install is already running.
esp_err_t ota_github_install(const char **err_msg);

typedef struct {
    const char *state;   // "idle" | "downloading" | "verifying" | "rebooting" | "error"
    int         pct;     // download progress 0-100
    char        msg[96]; // error detail when state == "error"
} ota_progress_t;

void ota_github_progress(ota_progress_t *out);

#ifdef __cplusplus
}
#endif

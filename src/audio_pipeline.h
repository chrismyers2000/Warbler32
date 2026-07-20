#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_pipeline_start(void);

// Each streaming client subscribes to get its own ring buffer (starts
// empty), reads from it, and unsubscribes when done. Returns a reader
// handle >= 0, or -1 if all PIPELINE_MAX_READERS slots are taken.
int    audio_pipeline_subscribe(void);
void   audio_pipeline_unsubscribe(int reader);
size_t audio_pipeline_read(int reader, uint8_t *buf, size_t bytes, uint32_t timeout_ms);

int      audio_pipeline_get_peak_pct(void);
uint32_t audio_pipeline_get_overruns(void);   // total packets dropped on full buffers

#ifdef __cplusplus
}
#endif

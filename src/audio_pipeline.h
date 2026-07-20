#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_pipeline_start(void);
size_t    audio_pipeline_read(uint8_t *buf, size_t bytes, uint32_t timeout_ms);
void      audio_pipeline_flush(void);
int       audio_pipeline_get_peak_pct(void);

#ifdef __cplusplus
}
#endif

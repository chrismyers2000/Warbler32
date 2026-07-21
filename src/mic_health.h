#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Dead-mic detector. A failed INMP441 (broken wire, dead board) delivers a
// flatline — all zeros or a stuck DC value — while a healthy mic's raw
// signal always carries at least its own self-noise. The detector watches
// the raw pre-DSP samples (gain and filtering could flatten quiet audio and
// cause false alarms) and declares the mic silent after MIC_HEALTH_TIMEOUT_MS
// of continuous flatline. Also trips if samples stop arriving entirely (e.g. a
// USB mic that dropped off the bus).

// Feed one chunk of raw samples; called from audio_dsp_process().
void mic_health_report(const int16_t *buf, size_t count);

// True until the mic has been flat/absent for MIC_HEALTH_TIMEOUT_MS.
bool mic_health_ok(void);

// Seconds since the last live audio, or 0 while the mic is healthy.
uint32_t mic_health_silent_secs(void);

#ifdef __cplusplus
}
#endif

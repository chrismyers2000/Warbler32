#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float hpf_alpha;
    float hpf_in;
    float hpf_out;
} audio_dsp_state_t;

// Precompute HPF alpha from g_config.hpf_freq / g_config.sample_rate and reset filter state.
void audio_dsp_init(audio_dsp_state_t *st);

// Apply gain_mult, high-pass filter, clamping, and noise gate in place.
// Input samples must already be scaled to roughly int16 range (e.g. after
// any source-specific bit-depth extraction such as the I2S 32-bit frame shift).
void audio_dsp_process(audio_dsp_state_t *st, int16_t *buf, size_t count);

#ifdef __cplusplus
}
#endif

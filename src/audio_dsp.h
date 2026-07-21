#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_HPF_MAX_STAGES 4   // 4 cascaded 1st-order stages = 24 dB/octave

typedef struct {
    float    hpf_alpha;
    float    shelf_k;    // dry-mix gain 10^(-depth/20); 0 = full cut, 1 = bypass
    int      stages;     // active cascade length (1..AUDIO_HPF_MAX_STAGES)
    float    in[AUDIO_HPF_MAX_STAGES];
    float    out[AUDIO_HPF_MAX_STAGES];
    // g_config values the coefficients were derived from — all three are
    // web-configurable without a reboot, so process() re-inits on change
    uint16_t hpf_freq_applied;
    uint8_t  hpf_slope_applied;
    uint8_t  hpf_depth_applied;
} audio_dsp_state_t;

// Precompute HPF coefficients from g_config (hpf_freq/hpf_slope/hpf_depth
// and sample_rate) and reset filter state.
void audio_dsp_init(audio_dsp_state_t *st);

// Apply gain_mult, high-pass filter (cascaded shelf), clamping, and noise
// gate in place. Input samples must already be scaled to roughly int16 range
// (e.g. after any source-specific bit-depth extraction such as the I2S
// 32-bit frame shift).
void audio_dsp_process(audio_dsp_state_t *st, int16_t *buf, size_t count);

#ifdef __cplusplus
}
#endif

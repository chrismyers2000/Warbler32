#include "audio_dsp.h"
#include "app_config.h"

#include <math.h>

void audio_dsp_init(audio_dsp_state_t *st)
{
    // α = 1 − 2π×fc/fs  (1st-order IIR, valid for fc << fs)
    if (g_config.hpf_freq > 0) {
        st->hpf_alpha = 1.0f - (2.0f * 3.14159265f * (float)g_config.hpf_freq)
                               / (float)g_config.sample_rate;
        if (st->hpf_alpha < 0.0f) st->hpf_alpha = 0.0f;
    } else {
        st->hpf_alpha = 0.0f;
    }

    st->stages = g_config.hpf_slope;
    if (st->stages < 1) st->stages = 1;
    if (st->stages > AUDIO_HPF_MAX_STAGES) st->stages = AUDIO_HPF_MAX_STAGES;

    // Shelf depth: mix `shelf_k` of the dry signal back in, so lows are
    // pushed down by `hpf_depth` dB instead of removed outright. 60 dB is
    // treated as a full cut.
    st->shelf_k = (g_config.hpf_depth >= 60)
                      ? 0.0f
                      : powf(10.0f, -(float)g_config.hpf_depth / 20.0f);

    for (int i = 0; i < AUDIO_HPF_MAX_STAGES; i++) {
        st->in[i]  = 0.0f;
        st->out[i] = 0.0f;
    }

    st->hpf_freq_applied  = g_config.hpf_freq;
    st->hpf_slope_applied = g_config.hpf_slope;
    st->hpf_depth_applied = g_config.hpf_depth;
}

void audio_dsp_process(audio_dsp_state_t *st, int16_t *buf, size_t count)
{
    // All HPF parameters are web-configurable without a reboot; re-derive
    // coefficients when any of them change (gain_mult/noise_gate below read
    // g_config directly, so they need no equivalent).
    if (st->hpf_freq_applied  != g_config.hpf_freq  ||
        st->hpf_slope_applied != g_config.hpf_slope ||
        st->hpf_depth_applied != g_config.hpf_depth)
        audio_dsp_init(st);

    for (size_t i = 0; i < count; i++) {
        int32_t s = (int32_t)buf[i] * g_config.gain_mult;

        // High-pass: cascaded 1st-order stages (6 dB/oct each), each
        // y[n] = α*(y[n-1] + x[n] - x[n-1]), then shelf-blend the dry
        // signal back in for a partial (attenuating) cut.
        if (st->hpf_alpha > 0.0f) {
            float x = (float)s;
            float y = x;
            for (int stage = 0; stage < st->stages; stage++) {
                float out = st->hpf_alpha * (st->out[stage] + y - st->in[stage]);
                st->in[stage]  = y;
                st->out[stage] = out;
                y = out;
            }
            s = (int32_t)(y + st->shelf_k * (x - y));
        }

        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        // Noise gate: mute samples below threshold. Compute the absolute
        // value in int32_t (s's own type) before comparing — narrowing to
        // int16_t first overflows when s is exactly -32768 (a full-scale
        // clipped sample), wrapping the result negative and causing the
        // gate to fire on every clipped sample regardless of threshold.
        if (g_config.noise_gate > 0) {
            int32_t a = s < 0 ? -s : s;
            if (a < (int32_t)g_config.noise_gate) s = 0;
        }

        buf[i] = (int16_t)s;
    }
}

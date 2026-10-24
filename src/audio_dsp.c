#include "audio_dsp.h"
#include "app_config.h"
#include "mic_health.h"

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

void audio_dsp_process(audio_dsp_state_t *st, const int32_t *in, int16_t *out,
                       size_t count, unsigned shift)
{
    // Dead-mic detection must see the raw signal, before gain and filtering
    // can flatten or reshape it.
    mic_health_report(in, count);

    // All HPF parameters are web-configurable without a reboot; re-derive
    // coefficients when any of them change (gain_mult below reads g_config
    // directly, so it needs no equivalent).
    if (st->hpf_freq_applied  != g_config.hpf_freq  ||
        st->hpf_slope_applied != g_config.hpf_slope ||
        st->hpf_depth_applied != g_config.hpf_depth)
        audio_dsp_init(st);

    const float gain = (float)g_config.gain_mult / (float)(1ULL << shift);

    for (size_t i = 0; i < count; i++) {
        float x = (float)in[i] * gain;
        float y = x;

        // High-pass: cascaded 1st-order stages (6 dB/oct each), each
        // y[n] = α*(y[n-1] + x[n] - x[n-1]), then shelf-blend the dry
        // signal back in for a partial (attenuating) cut.
        if (st->hpf_alpha > 0.0f) {
            for (int stage = 0; stage < st->stages; stage++) {
                float o = st->hpf_alpha * (st->out[stage] + y - st->in[stage]);
                st->in[stage]  = y;
                st->out[stage] = o;
                y = o;
            }
            y += st->shelf_k * (x - y);
        }

        if (y >  32767.0f) y =  32767.0f;
        if (y < -32768.0f) y = -32768.0f;

        out[i] = (int16_t)lroundf(y);
    }
}

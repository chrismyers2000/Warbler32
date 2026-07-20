#include "audio_dsp.h"
#include "app_config.h"

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
    st->hpf_in  = 0.0f;
    st->hpf_out = 0.0f;
}

void audio_dsp_process(audio_dsp_state_t *st, int16_t *buf, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        int32_t s = (int32_t)buf[i] * g_config.gain_mult;

        // High-pass filter: y[n] = α*(y[n-1] + x[n] - x[n-1])
        if (st->hpf_alpha > 0.0f) {
            float in  = (float)s;
            float out = st->hpf_alpha * (st->hpf_out + in - st->hpf_in);
            st->hpf_in  = in;
            st->hpf_out = out;
            s = (int32_t)out;
        }

        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        // Noise gate: mute samples below threshold
        if (g_config.noise_gate > 0) {
            int16_t a = (int16_t)(s < 0 ? -s : s);
            if (a < (int16_t)g_config.noise_gate) s = 0;
        }

        buf[i] = (int16_t)s;
    }
}

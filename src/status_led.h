#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_CONNECTING,   // blue blink        — trying to join WiFi
    LED_CONNECTED,    // solid green       — WiFi up, no RTSP client
    LED_STREAMING,    // green blink       — RTSP client is playing
    LED_ERROR,        // orange blink      — WiFi failed
    LED_SETUP,        // red blink         — broadcasting the setup AP
} led_state_t;
// Note: CONNECTED/STREAMING render as a magenta blink instead while the
// mic-health detector reports a flatlined mic (see mic_health.h), or while
// no mic has been found yet (see audio_pipeline_is_active() in
// audio_pipeline.h).

// Initialise RMT + WS2812B and start the blink task. Call once from app_main.
esp_err_t status_led_init(void);

// Thread-safe: can be called from any task or ISR-deferred context.
void status_led_set(led_state_t state);
led_state_t status_led_get(void);

// Briefly overrides the current state with a rapid white flash (~300ms),
// then automatically resumes rendering whatever status_led_set() last set.
void status_led_flash(void);

#ifdef __cplusplus
}
#endif

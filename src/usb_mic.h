#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Initialise USB Host mode and wait for a UAC 1.0 microphone to connect,
// negotiating a 16-bit format matching g_config.sample_rate.
esp_err_t usb_mic_init(void);

// Read 16-bit mono PCM samples into buf (downmixed if the device is stereo).
// Returns the number of samples actually read (may be less than count).
size_t usb_mic_read(int16_t *buf, size_t count);

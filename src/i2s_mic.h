#pragma once

#include "esp_err.h"
#include <stddef.h>

// Initialise the I2S peripheral for the INMP441 microphone.
esp_err_t i2s_mic_init(void);

// Read 16-bit mono PCM samples into buf.
// Returns the number of samples actually read (may be less than count).
size_t i2s_mic_read(int16_t *buf, size_t count);

#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// Initialise the I2S peripheral for the INMP441 microphone.
esp_err_t i2s_mic_init(void);

// Read 16-bit mono PCM samples into buf.
// Returns the number of samples actually read (may be less than count).
size_t i2s_mic_read(int16_t *buf, size_t count);

// Count of DMA receive queue overflows since init — the driver ISR
// finished a DMA buffer before i2s_mic_read() drained the previous one,
// so samples were silently overwritten below the driver's read() API.
// Distinct from audio_pipeline's overrun counter, which only sees drops
// at the app-level ring buffer.
uint32_t i2s_mic_get_dma_overflows(void);

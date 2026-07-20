#pragma once

// =============================================================================
// WiFi
// =============================================================================
// Left blank so a fresh build has no network configured — set up over WiFi
// via the device's own setup AP (see WIFI_AP_* below) instead of hardcoding
// credentials here.
#define WIFI_SSID        ""
#define WIFI_PASSWORD    ""
#define WIFI_MAX_RETRIES  10

// Fallback setup AP the device broadcasts when it can't join a saved network
// (blank credentials, wrong password, router out of range, etc). Connect to
// this network and browse to 192.168.4.1 to configure the real WiFi network.
#define WIFI_AP_SSID      "Warbler32-Setup"
#define WIFI_AP_PASSWORD  "warbler32"

// mDNS hostname once connected to a real network — reachable at
// http://<name>.local/. User-configurable via the web UI; this is just the
// fallback for a fresh device or an emptied-out name field.
#define DEVICE_NAME_DEFAULT  "warbler32"

// =============================================================================
// BOOT button (GPIO0) gestures — checked only while the app is already
// running, never at power-on: GPIO0's bootloader-strapping role would
// otherwise put the chip in USB download mode instead of booting normally.
// =============================================================================
// Hold this long to wipe saved settings and drop back into setup mode.
#define FACTORY_RESET_GPIO     0
#define FACTORY_RESET_HOLD_MS  5000
// Two taps within this long (while broadcasting the setup AP) cycles the
// AP's WiFi channel through 1/6/11.
#define DOUBLE_TAP_WINDOW_MS   500

// =============================================================================
// I2S / INMP441 pins (change to match your wiring)
// =============================================================================
#define I2S_PORT         I2S_NUM_0
#define I2S_PIN_WS       42      // Word Select (LRCLK)
#define I2S_PIN_SCK      41      // Bit Clock  (BCLK)
#define I2S_PIN_SD       40      // Serial Data (DOUT from mic)

// =============================================================================
// Audio input source
// =============================================================================
#define AUDIO_SOURCE_I2S      0   // INMP441 over I2S
#define AUDIO_SOURCE_USB      1   // USB Audio Class (UAC 1.0) microphone, USB Host mode
#define AUDIO_SOURCE_DEFAULT  AUDIO_SOURCE_I2S

// =============================================================================
// Audio parameters
// =============================================================================
#define AUDIO_SAMPLE_RATE     48000   // Hz
#define AUDIO_CHANNELS        1       // mono
#define AUDIO_BITS_PER_SAMPLE 16      // bits sent over RTSP (after 32→16 shift)

// I2S DMA: number of buffers * buffer length (in samples) must hold ~10 ms
#define I2S_DMA_BUF_COUNT    8
#define I2S_DMA_BUF_LEN      1024    // samples per DMA buffer

// =============================================================================
// Audio gain  (INMP441 outputs 24-bit left-justified in a 32-bit I2S frame)
// =============================================================================
// AUDIO_GAIN_SHIFT — right-shift to extract 16-bit value from 32-bit frame.
//   16 = unity (top 16 of 24 bits), 14 = +6 dB, 12 = +12 dB, 10 = +18 dB
#define AUDIO_GAIN_SHIFT  12

// AUDIO_GAIN_MULT — integer multiplier applied after shift, with saturation.
//   1 = no extra gain, 2 = +6 dB, 4 = +12 dB, 8 = +18 dB
// Together SHIFT=12 + MULT=2 gives ~+18 dB over unity.
// If audio clips/distorts: raise SHIFT by 2 or halve MULT.
#define AUDIO_GAIN_MULT   2

// High-pass filter cutoff frequency in Hz (0 = off)
// 80-200 Hz recommended outdoors; higher values cut more low-frequency noise
#define AUDIO_HPF_FREQ    0

// Noise gate: silence output when peak is below this threshold (int16 units)
// 0 = disabled, 50-500 = typical useful range
#define AUDIO_NOISE_GATE  0

// =============================================================================
// RTSP / RTP
// =============================================================================
#define RTSP_PORT     554
#define RTP_PAYLOAD   11           // PT 11 = L16 in RFC 3551
// RTP packet carries this many samples (20 ms at 48 kHz = 960 samples; 320 at 16 kHz)
#define RTP_SAMPLES_PER_PACKET  960

// =============================================================================
// Audio pipeline ring buffer (allocated in PSRAM)
// Holds ~500 ms of audio to absorb network jitter
// =============================================================================
#define PIPELINE_BUF_BYTES  (AUDIO_SAMPLE_RATE * AUDIO_BITS_PER_SAMPLE / 8 / 2)

// =============================================================================
// NeoPixel status LED (WS2812B on GPIO 48 = onboard RGB LED)
// =============================================================================
#define NEOPIXEL_GPIO       48
#define NEOPIXEL_BRIGHTNESS 30   // 0-255; keep low to avoid glare indoors

// =============================================================================
// FreeRTOS task settings
// =============================================================================
#define TASK_I2S_STACK      4096
#define TASK_I2S_PRIORITY   5
#define TASK_I2S_CORE       1

#define TASK_RTP_STACK      4096
#define TASK_RTP_PRIORITY   4
#define TASK_RTP_CORE       0

#define TASK_RTSP_STACK     6144
#define TASK_RTSP_PRIORITY  3
#define TASK_RTSP_CORE      0

// USB Host library event-pump task (only spawned when AUDIO_SOURCE_USB is active)
#define TASK_USB_STACK      4096
#define TASK_USB_PRIORITY   5
#define TASK_USB_CORE       1

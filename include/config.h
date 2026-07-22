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

// Max WiFi TX power, in dBm (converted to esp_wifi_set_max_tx_power()'s
// quarter-dBm units at the call site). Applied on every boot and live on
// /save — no reboot needed. User-configurable via the web UI; 20 dBm is
// effectively "don't attenuate" (matches the chip's own ceiling). Lowering
// this is a useful diagnostic/mitigation for RF noise coupling from WiFi TX
// bursts into nearby analog mic wiring/power rails — worth trying if audio
// is noisy despite a stable link.
#define WIFI_TX_POWER_DBM_DEFAULT  20
#define WIFI_TX_POWER_DBM_MIN      8
#define WIFI_TX_POWER_DBM_MAX      20

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
#define AUDIO_SOURCE_I2S      0   // I2S MEMS mic (see MIC_MODEL_*)
#define AUDIO_SOURCE_USB      1   // USB Audio Class (UAC 1.0) microphone, USB Host mode
#define AUDIO_SOURCE_DEFAULT  AUDIO_SOURCE_I2S

// Which I2S mic is wired up. Both use the same pins and wiring (L/R / SEL
// tied to GND), but the SPH0645 clocks data out one BCLK early relative to
// the Philips I2S standard, so it needs the MSB (left-justified) slot format.
#define MIC_MODEL_INMP441     0
#define MIC_MODEL_SPH0645     1
#define MIC_MODEL_DEFAULT     MIC_MODEL_INMP441

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

// HPF slope: number of cascaded 1st-order stages (1-4 = 6/12/18/24 dB per octave)
#define AUDIO_HPF_SLOPE   1
// HPF attenuation depth in dB (low-shelf): how far lows below the cutoff are
// pushed down. 60 = effectively a full cut (matches the pre-shelf behavior);
// 0 = filter bypassed.
#define AUDIO_HPF_DEPTH   60

// Mic-health detector: the mic is declared silent (web status + magenta LED)
// when the raw pre-DSP signal shows less than MIN_P2P peak-to-peak movement
// for TIMEOUT_MS. A healthy INMP441's self-noise alone exceeds 4 counts, so
// even a dead-quiet room never trips this — only a true flatline does.
#define MIC_HEALTH_MIN_P2P     4
#define MIC_HEALTH_TIMEOUT_MS  20000

// Pipeline stall watchdog: distinct from mic-health above. Mic-health
// watches *content* of the samples the reader task delivers (catches a
// flatlined mic while the task itself is fine). This watches whether the
// reader task is delivering samples *at all* — catches a wedged task/driver
// (e.g. an I2S read call that never returns) that mic-health can't see,
// since a hung task never calls mic_health_report() again either. A stall
// this severe generally isn't recoverable in-place, so the response is a
// full reboot: samples the pipeline's chunk counter every CHECK_INTERVAL_MS
// and reboots once it's seen zero forward progress for STALL_CHECKS
// consecutive samples (~CHECK_INTERVAL_MS * STALL_CHECKS of total silence).
// User-toggleable via the web UI (app_config.h watchdog_enabled).
#define PIPELINE_WATCHDOG_DEFAULT_ENABLED  1
#define PIPELINE_WATCHDOG_CHECK_INTERVAL_MS  15000
#define PIPELINE_WATCHDOG_STALL_CHECKS       3

// GitHub OTA download: retry transient network failures (mesh WiFi hiccups)
#define OTA_GH_ATTEMPTS        3
#define OTA_GH_RETRY_DELAY_MS  3000

// =============================================================================
// RTSP / RTP
// =============================================================================
#define RTSP_PORT         554
#define RTSP_MAX_CLIENTS  2   // simultaneous RTSP clients (each takes a pipeline reader)
#define RTP_PAYLOAD   11           // PT 11 = L16 in RFC 3551
// RTP packet carries this many samples (20 ms at 48 kHz = 960 samples; 320 at 16 kHz)
#define RTP_SAMPLES_PER_PACKET  960

// =============================================================================
// OTA updates from GitHub releases
// Release assets must be named warbler32-quad.bin / warbler32-oct.bin
// (published by scripts/release.sh)
// =============================================================================
#define OTA_GITHUB_REPO "chrismyers2000/Warbler32"

// =============================================================================
// Audio pipeline ring buffers (allocated in PSRAM)
// Each subscriber (one per streaming RTSP client) gets its own buffer
// holding ~2s of audio to absorb network jitter. Sized to comfortably
// exceed send_all()'s own 1.5s max-stall retry budget in rtsp_server.c —
// at the previous ~500ms buffer, any TCP stall past 500ms (weak WiFi,
// congestion) started dropping whole audio chunks well before send_all()
// gave up on the connection, which is audible as clicking/crackling.
// Negligible PSRAM cost either way (a few hundred KB across all readers).
// =============================================================================
#define PIPELINE_BUF_BYTES   (AUDIO_SAMPLE_RATE * AUDIO_BITS_PER_SAMPLE / 8 * 2)
#define PIPELINE_MAX_READERS 3   // RTSP_MAX_CLIENTS + 1 browser preview stream

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

// Background mic-retry task (only spawned if no mic is found at boot)
#define TASK_MIC_RETRY_STACK    3072
#define TASK_MIC_RETRY_PRIORITY 2
#define TASK_MIC_RETRY_CORE     1

#define TASK_BATTERY_STACK      3072
#define TASK_BATTERY_PRIORITY   2
#define TASK_BATTERY_CORE       1

// Background pipeline-stall watchdog task (see PIPELINE_WATCHDOG_* above)
#define TASK_WATCHDOG_STACK     3072
#define TASK_WATCHDOG_PRIORITY  2
#define TASK_WATCHDOG_CORE      1

// =============================================================================
// Battery monitor (INA219 I2C voltage sensor) — optional. If no INA219 is
// found on the bus, /status just reports it absent; nothing else is
// affected (no blocking boot, no error spam beyond one log line per
// present/absent transition).
// =============================================================================
#define BATTERY_I2C_PORT        I2C_NUM_0
#define BATTERY_I2C_SDA_GPIO    8     // free on ESP32-S3-DevKitC-1: not a
#define BATTERY_I2C_SCL_GPIO    9     // strapping pin, not USB D+/D-, not PSRAM
#define BATTERY_I2C_FREQ_HZ     100000

#define INA219_I2C_ADDR         0x40  // default addr, all ADR pins unstrapped
#define INA219_REG_BUS_VOLTAGE  0x02  // 13-bit, 4 mV/LSB — no calibration needed

#define BATTERY_POLL_INTERVAL_MS  10000

// Battery profile defaults (1S Li-ion/LiPo) — user-configurable via the web
// UI from here on; see app_config.h's batt_* fields.
#define BATTERY_DEFAULT_CHEMISTRY    0   // 0=Li-ion/LiPo, 1=LiFePO4, 2=Custom
#define BATTERY_DEFAULT_CELLS        1
#define BATTERY_DEFAULT_LOW_MV       3300
#define BATTERY_DEFAULT_NOMINAL_MV   3700
#define BATTERY_DEFAULT_FULL_MV      4200

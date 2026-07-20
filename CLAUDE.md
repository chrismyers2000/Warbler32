# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ESP32-S3 (N8R2: 8MB flash, 2MB PSRAM) firmware that captures audio from either an
INMP441 I2S microphone or a USB Audio Class (UAC 1.0) microphone and streams it
live over RTSP (PCM L16). Bare ESP-IDF (no Arduino), built via PlatformIO. No git
repository in this working tree.

## Commands

```bash
pio run                  # build
pio run -t upload        # build + flash (board is at /dev/ttyACM0)
```

`pio run monitor` needs a real TTY, which doesn't work here — read serial via:

```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False); time.sleep(0.1)
end = time.time() + 10
while time.time() < end:
    l = s.readline()
    if l: print(l.decode('utf-8', errors='replace').rstrip())
s.close()
"
```

Changing `src/idf_component.yml` or `sdkconfig.defaults` requires regenerating
the config before the next build: `rm -f sdkconfig.esp32s3 && pio run`.

Stream: `rtsp://<device-ip>/audio` (port 554). Config/monitor web UI: port 80.

## Architecture

**Boot** (`src/main.cpp`): NVS → `app_config_load()` → `status_led_init()` →
`wifi_manager_start()` → `web_server_start()` → `audio_pipeline_start()` →
`rtsp_server_start()`, each wrapped in `ESP_ERROR_CHECK`. Any subsystem failing
to init panics/reboots the whole device (no partial-degradation mode) — e.g.
selecting the USB mic with none plugged in reboot-loops. The web server starts
before the audio pipeline, so it's briefly reachable each boot to fix a bad
config before a later failure panics.

**Runtime config**: one global `app_config_t g_config` (`src/app_config.h/.c`),
loaded from NVS over `include/config.h` compile-time defaults. `src/web_server.c`
serves the whole config page as one embedded `snprintf`-templated HTML string
(no filesystem/JS framework) and `POST /save` updates `g_config`, persists it,
and unconditionally reboots — there's no live-reinit path for any setting.

**Audio input** is pluggable: `src/i2s_mic.c` (INMP441) and `src/usb_mic.c`
(UAC 1.0 mic via USB Host, `espressif/usb_host_uac`) both expose the same
`init()`/`read()` shape, and `src/audio_pipeline.c` picks one via a function
pointer based on `g_config.audio_source`. Shared gain/HPF/noise-gate DSP lives
in `src/audio_dsp.c`, applied after either backend produces 16-bit mono PCM.
`audio_pipeline.c` also owns the PSRAM ring buffer that feeds the RTSP server.

**RTSP/RTP**: `src/rtsp_server.c` is a minimal hand-rolled RTSP/RTP server on
raw sockets (no external library) — parses OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN
and streams RTP either as UDP or, for BirdNET-Go, TCP-interleaved framing.
TCP sends must go through the full-write retry loop (`send_all`) — a short
write permanently desyncs the interleaved framing for the rest of the session.

**Task layout**: most subsystems run as their own FreeRTOS task, with
stack/priority/core set via named constants in `config.h` (`TASK_*`).

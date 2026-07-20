# Warbler32

ESP32-S3 firmware that captures audio from a microphone and streams it live over
RTSP, for listening to (or running BirdNET-Go against) birds outside your window.

Supports two microphone options:
- **INMP441** — a cheap I2S digital microphone breakout, wired directly to the board.
- **USB microphone** — any standard USB Audio Class (UAC 1.0) mic/headset, plugged
  into the board's native USB port.

## Hardware

- ESP32-S3-DevKitC-1 (N8R2: 8MB flash, 2MB PSRAM)
- Either an INMP441 breakout, or a UAC-class USB microphone

### INMP441 wiring

| INMP441 pin | ESP32-S3 GPIO |
|---|---|
| WS  | 42 |
| SCK | 41 |
| SD  | 40 |
| L/R | GND |

### USB microphone

Plug the mic into the board's native **"USB"** port — not the **"UART"** port
(that one's only for flashing/serial and can't act as a USB host). If the mic
never gets detected, check whether your board has a **"USB-OTG"** solder
jumper/pad that needs bridging — some DevKitC-1 revisions don't supply 5V to
the native USB port unless it's bridged.

## Flashing

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -t upload
```

## First-time WiFi setup

The device ships with no WiFi configured. On first boot (and any time it can't
join a saved network) it broadcasts its own setup network:

1. On your phone or laptop, connect to the WiFi network **`Warbler32-Setup`**
   (password: **`warbler32`**).
2. Browse to **`http://192.168.4.1/`**.
3. Enter your home WiFi's SSID and password, then **Save & Reboot**.

The status LED blinks red (slowly) while in setup mode.

The setup AP automatically picks a quiet 2.4GHz channel (6 or 11) by
scanning for nearby networks first. If devices still won't join it reliably,
double-tap the board's BOOT button (two quick presses) to manually cycle the
AP to the next channel in `1 → 6 → 11 → ...` — the LED flashes white to
confirm. Only does anything while the device is broadcasting the setup AP.

## Using it

The config page has a **Device Name** field (default `warbler32`) — set it
to whatever you like (letters, numbers, hyphens), useful for telling multiple
devices apart. Once connected to your WiFi, the device is reachable at:

- **Config page**: `http://<name>.local/` (e.g. `http://warbler32.local/`)
- **Stream**: `rtsp://<name>.local/audio`

(If your network doesn't support mDNS/`.local` names, check your router's
client list for the device's IP instead.)

Open the stream in VLC, or point [BirdNET-Go](https://github.com/tphakala/birdnet-go)
at the RTSP URL for live bird ID.

The config page also lets you switch between the INMP441 and USB microphone,
adjust sample rate/gain/filtering, and watch a live audio level meter.

## Factory reset

Two ways to wipe saved WiFi/audio settings and drop back into setup mode:

- **Web UI**: click **Factory Reset** at the bottom of the config page (asks
  for confirmation first).
- **BOOT button**: once the device is already running (not while powering
  on — GPIO0 is a strapping pin, so holding it during power-on puts the chip
  in USB download mode instead), press and hold the board's BOOT button for
  5 seconds. The status LED blinks orange while held; release early to
  cancel. Useful if the device is unreachable on its saved network.

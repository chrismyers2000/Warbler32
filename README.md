# Warbler32

Warbler32 is a device for streaming RTSP audio to a bird identification server such as [BirdNET-Go](https://github.com/tphakala/birdnet-go). This is a low power device that can be solar and/or battery powered and placed further away (within WiFi range) from your house than you would if running the microphone on the BirdNET server itself (usually a raspberry pi). As an example, I'm running BirdNET-Go on a proxmox server and have 3 audio streams running to it at the same time. These 3 devices are located  about 600ft from each-other on my property so I can capture a wider range of birds.

Supports these microphone options:
- **INMP441** — a cheap I2S digital microphone breakout, wired directly to the board.
- **SPH0645** — Adafruit's I2S MEMS microphone breakout, same wiring as the INMP441
  (This one is tested to have much cleaner audio than the INMP441).
- **USB microphone (Cleanest audio if using a good Mic)** — most standard USB Audio Class (UAC 1.0) mic/headset, plugged
  into the board's native USB port.

No programming experience is required to use this — just follow the steps below.

## What you'll need

- An **ESP32-S3-DevKitC-1** board with PSRAM — N8R2, N16R8, or similar
  variants with two USB ports (native USB + UART) all work. The exact
  flash/PSRAM configuration is auto-detected when you flash it, so you don't
  need to figure out which one you have.
- A USB cable to connect it to your computer
- Either an **INMP441** or **SPH0645** microphone breakout, or a **UAC-class USB microphone**
- (Optional) An **INA219** breakout board, if you want battery-voltage
  monitoring on the Status card — most boards default to I2C address
  `0x40`, which this firmware expects.
- A computer running Windows, macOS, or Linux

## Flashing the firmware

## [Click Here to use the WEB FLASHER](https://chrismyers2000.github.io/Warbler32/) Easiest method for all operating systems

### ESPtool (for Linux)
The next best way to get the firmware onto the board — Just one command:

```bash
curl -fsSL https://raw.githubusercontent.com/chrismyers2000/Warbler32/master/scripts/install.sh | bash
```

> No `curl`? (it isn't installed by default on every system, e.g. some
> minimal/server installs) — use `wget` instead:
> `wget -qO- https://raw.githubusercontent.com/chrismyers2000/Warbler32/master/scripts/install.sh | bash`

This detects your connected board's PSRAM variant automatically, downloads
the matching `warbler32-<variant>-factory.bin` from the
[latest release](https://github.com/chrismyers2000/Warbler32/releases/latest),
and flashes it with [esptool](https://github.com/espressif/esptool)
(installed automatically via `pip` if it isn't already present). It'll ask
for confirmation before actually flashing.

> **This wipes any saved settings** (WiFi, audio config — everything),
> same as a factory reset, because the factory image spans the whole
> region of flash where settings are stored. Perfect for a brand-new board;
> **don't** point this at an already-configured device unless you want to
> reconfigure it from scratch. After flashing, connect to the device's own
> `Warbler32-Setup` WiFi network and browse to `192.168.4.1` — see
> [First-time WiFi setup](#first-time-wifi-setup) below.

**On Windows, or don't want to touch a terminal at all?** Use the
[browser-based flasher](https://chrismyers2000.github.io/Warbler32/)
instead — pick your board from a dropdown and click Connect. No install of
anything, not even esptool. Needs Chrome or Edge (Safari doesn't support
the Web Serial API this relies on).

<details>
<summary>Prefer to do it manually, or already know your variant?</summary>

Every release publishes the same `warbler32-<variant>-factory.bin` files as
plain downloadable assets — grab the one matching your board and flash it
yourself:

```bash
python3 -m pip install --user esptool
esptool.py --chip esp32s3 write_flash 0x0 warbler32-quad-factory.bin
```

Use the `oct` variant instead of `quad` if you have an N16R8 (or other
Octal-PSRAM) board — see [What you'll need](#what-youll-need) above; if
you're not sure which you have, `quad` is the safer default and the
firmware will tell you at boot (over serial) if it's wrong.

> On Debian/Ubuntu 12+/23.04+ (including Raspberry Pi OS Trixie), a plain
> `pip install` is blocked by default (PEP 668). If you see an
> "externally-managed-environment" error, add `--break-system-packages`:
> `python3 -m pip install --user --break-system-packages esptool`. If
> `esptool.py` isn't found right after installing it, open a new terminal
> (it needs a fresh shell to pick up the updated PATH).

</details>

## Building from source (VS Code + PlatformIO)

Only needed if you want to modify the firmware yourself, or prefer building
locally instead of using the released `.bin` above. This project uses
**PlatformIO** (via **VS Code**, a free code editor) to build and flash the
firmware. Pick your operating system below — Linux is shown by default;
click **Windows** or **macOS** to expand those instead.

### Linux

1. **Get the code:**
   ```bash
   git clone https://github.com/chrismyers2000/Warbler32.git
   cd Warbler32
   ```
2. **Install VS Code and the PlatformIO extension** — a couple of commands,
   no downloading an installer or clicking through menus. The exact commands
   depend on your distro:

   **Raspberry Pi OS:**
   ```bash
   sudo apt update && sudo apt install -y code
   code --install-extension platformio.platformio-ide
   ```
   > Raspberry Pi OS's own apt repo already includes VS Code, so this is
   > all it takes — no snap or extra setup needed. **This project is
   > developed and tested on Raspberry Pi OS Trixie**, so if that's what
   > you're running, this is the best-tested path.

   **Ubuntu:**
   ```bash
   sudo snap install code --classic
   code --install-extension platformio.platformio-ide
   ```

   **Debian:** (plain Debian doesn't include Raspberry Pi OS's VS Code
   package or Ubuntu's preinstalled `snap` command, so install `snapd` first)
   ```bash
   sudo apt update && sudo apt install -y snapd
   sudo snap install core
   sudo snap install code --classic
   code --install-extension platformio.platformio-ide
   ```
   > If `snap install` doesn't work right after installing `snapd`, log out
   > and back in (or reboot) first — it needs a fresh shell session to pick
   > up its PATH changes.
3. **Open the project** — this launches VS Code with the folder already open,
   no File menu needed:
   ```bash
   code .
   ```
   Give PlatformIO a minute the first time — it downloads the ESP32-S3
   toolchain in the background (progress shows in the bottom status bar).
4. **Connect and flash:** plug the board into your computer via the port
   labeled **"UART"** (not "USB" — that one's only used later, for a USB
   microphone), click the PlatformIO alien-head icon in the sidebar, then under
   **PROJECT TASKS → esp32s3 → General** click **Upload**. This takes a minute
   or two the first time; you'll see `[SUCCESS]` when it's done.
5. **If the board isn't found:** you likely need permission on the serial
   port — add your user to the `dialout` group and log out/back in:
   ```bash
   sudo usermod -aG dialout $USER
   ```
   Still nothing? Try a different USB cable (some are power-only) or port.

<details>
<summary><strong>Windows</strong></summary>

1. **Get the code:** on this repository's GitHub page, click the green
   **`<> Code`** button → **Download ZIP**. Find the downloaded ZIP (usually in
   `Downloads`) and extract it — you'll get a folder named `Warbler32` (or
   `Warbler32-main`).
2. **Install the tools:**
   1. Install [Visual Studio Code](https://code.visualstudio.com/) (run the
      installer, accepting the defaults).
   2. Open VS Code, click the **Extensions** icon in the left sidebar (or press
      `Ctrl+Shift+X`), search for **"PlatformIO IDE"**, and click **Install**.
   3. Wait for the install to finish — this can take a few minutes. Let VS Code
      reload/restart if it asks to.
3. **Open the project:** **File → Open Folder...** and select the extracted
   `Warbler32` folder (the one containing `platformio.ini`). Give PlatformIO a
   minute the first time — it downloads the ESP32-S3 toolchain in the
   background (progress shows in the bottom status bar).
4. **Connect and flash:** plug the board into your computer via the port
   labeled **"UART"** (not "USB" — that one's only used later, for a USB
   microphone), click the PlatformIO alien-head icon in the sidebar, then under
   **PROJECT TASKS → esp32s3 → General** click **Upload**. This takes a minute
   or two the first time; you'll see `[SUCCESS]` when it's done.
5. **If the board isn't found:** install the
   [CP210x or CH340 USB driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
   (check the sticker/silkscreen near the USB port for which chip your board
   uses) so Windows recognizes it as a serial device. Still nothing? Try a
   different USB cable (some are power-only) or port.

</details>

<details>
<summary><strong>macOS</strong></summary>

1. **Get the code:** on this repository's GitHub page, click the green
   **`<> Code`** button → **Download ZIP**. Find the downloaded ZIP (usually in
   `Downloads`) and extract it — you'll get a folder named `Warbler32` (or
   `Warbler32-main`).
2. **Install the tools:**
   1. Install [Visual Studio Code](https://code.visualstudio.com/) (drag it
      into Applications).
   2. Open VS Code, click the **Extensions** icon in the left sidebar (or press
      `Cmd+Shift+X`), search for **"PlatformIO IDE"**, and click **Install**.
   3. Wait for the install to finish — this can take a few minutes. Let VS Code
      reload/restart if it asks to.
3. **Open the project:** **File → Open Folder...** and select the extracted
   `Warbler32` folder (the one containing `platformio.ini`). Give PlatformIO a
   minute the first time — it downloads the ESP32-S3 toolchain in the
   background (progress shows in the bottom status bar).
4. **Connect and flash:** plug the board into your computer via the port
   labeled **"UART"** (not "USB" — that one's only used later, for a USB
   microphone), click the PlatformIO alien-head icon in the sidebar, then under
   **PROJECT TASKS → esp32s3 → General** click **Upload**. This takes a minute
   or two the first time; you'll see `[SUCCESS]` when it's done.
5. **If the board isn't found:** install the
   [CP210x or CH340 USB driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
   (check the sticker/silkscreen near the USB port for which chip your board
   uses) so macOS recognizes it as a serial device. Still nothing? Try a
   different USB cable (some are power-only) or port.

</details>

## Wiring (I2S mic only — skip if using a USB microphone)

### INMP441

| INMP441 pin | ESP32-S3 GPIO |
|---|---|
| VDD | 3.3V |
| GND | GND |
| WS | 42 |
| SCK | 41 |
| SD | 40 |
| L/R | GND |

Tying **L/R** to GND selects the left channel — the wiring this firmware
expects.

### Adafruit SPH0645

The Adafruit SPH0645 breakout ([product page](https://www.adafruit.com/product/3421))
silkscreens its pins differently from the INMP441, which is a common source of
mis-wiring — use these exact labels, not the INMP441 names above:

| SPH0645 (Adafruit) pin | ESP32-S3 GPIO |
|---|---|
| 3V | 3.3V |
| GND | GND |
| BCLK | 41 |
| DOUT | 40 |
| LRCL | 42 |
| SEL | GND |

Tying **SEL** to GND selects the left channel (the default) — the wiring this
firmware expects.

Both mics need different I2S bus timing internally, so after setup pick your
model under **Audio → I2S Mic Model** on the config page (default is
INMP441). For the SPH0645, also enable the high-pass filter (e.g. 100 Hz) —
that mic has a built-in DC offset the filter removes.

## Wiring (USB microphone only — skip if using an I2S mic)

Plug the mic into the board's native **"USB"** port — not the **"UART"** port
used for flashing. If the mic never gets detected, check whether your board has
a **"USB-OTG"** solder jumper/pad that needs bridging — some DevKitC-1 revisions
don't supply 5V to the native USB port unless it's bridged.

**CM108-based USB sound card dongles** (common, cheap, widely sold for PC
headsets/ham radio interfaces) enumerate fine as a generic UAC 1.0 device,
but **may produce audible crackling** on this firmware — testing found its
adaptive-clock audio endpoint isn't well handled by the ESP32 USB Audio
driver used here. It works cleanly on other hosts (e.g. a Raspberry Pi);
on this board, treat it as untested/may-not-work rather than a supported
option.

## Wiring (INA219 battery monitor — optional, skip if not using one)

| INA219 pin | ESP32-S3 GPIO |
|---|---|
| SDA | 8 |
| SCL | 9 |
| VCC | 3.3V |
| GND | GND |

Wire **VIN+/VIN-** across whatever you want to measure the voltage of —
typically directly across the battery terminals, before any regulator. The
INA219 measures up to 26V, plenty for common 1S-4S Li-ion/LiPo or LiFePO4
packs. If no INA219 is detected on the bus, the Status card just shows "–"
for Battery and nothing else in the firmware is affected.

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
to whatever you like (letters, numbers, hyphens). If you run more than one
Warbler32, give each a unique name: two devices with the same name fight
over the same `.local` address and you won't know which one you're talking
to. Once connected to your WiFi, the device is reachable at:

- **Config page**: `http://<name>.local/` (e.g. `http://warbler32.local/`)
- **Stream**: `rtsp://<name>.local/audio`

(If your network doesn't support mDNS/`.local` names, check your router's
client list for the device's IP instead.)

Open the stream in VLC, or point [BirdNET-Go](https://github.com/tphakala/birdnet-go)
at the RTSP URL for live bird ID. Up to **two clients** can stream at once —
handy for spot-checking in VLC while BirdNET-Go stays connected; a third
connection is politely refused.

The config page also lets you switch between the I2S and USB microphone
(and pick the I2S mic model), adjust sample rate/gain/filtering, and watch
a live audio level meter — plus
a **Listen** button that plays the live mic audio right in your browser
(sub-second latency), so you can position the mic and tune it by ear from a
phone without opening VLC. Audio
and LED tweaks (gain, high-pass filter, brightness) apply the
moment you hit Save — no reboot, no stream interruption. Changing the name,
WiFi, input source, or sample rate still reboots the device.

A **Status** card at the top shows live diagnostics — uptime, WiFi signal,
free memory, connected RTSP clients, dropped-audio count, mic health, and
battery voltage (if an INA219 is wired up). The same data is available as
JSON at `http://<name>.local/status` if you want to script your own
monitoring.

### Mic-health detection

A bird box that quietly dies shouldn't sit silent for days. The device
watches the raw microphone signal (before any filtering or noise gating)
and if it flatlines for 20 seconds — all zeros or a stuck value, as from a
broken wire or a dead mic — the Status card shows **SILENT** with how long
it's been out, `/status` reports `"mic":0`, and the status LED switches to
a **magenta blink** (instead of green) so you can see it from across the
yard. A healthy mic's own self-noise keeps the detector happy even in a
completely quiet room, so there are no false alarms at night. Everything
recovers automatically the moment real signal returns.

### Battery voltage monitoring

If an INA219 is wired up (see Wiring above), the config page's **Battery**
card lets you pick a chemistry (Li-ion/LiPo or LiFePO4) and cell count
(1S-4S) — the Low/Nominal/Full voltage fields auto-fill to sensible
presets, but stay directly editable if you want to fine-tune them, or pick
**Custom** to enter your own pack voltages from scratch. The Status card
shows the live voltage and an estimated percentage (a simple linear
estimate between Low and Full — not lab-grade, but good enough at a
glance), turning red with a **LOW** label once voltage drops to or below
the configured Low threshold. No INA219 wired up? The card just shows "–"
and nothing else in the firmware is affected. Battery settings apply
instantly, no reboot needed.

## Updating the firmware over WiFi

After the first USB flash, updates no longer need a cable — or even a
computer. On the config page's **Firmware** card, click **Check for
Updates**: the device asks GitHub for the latest release, automatically
picks the build matching its own hardware (Quad or Octal PSRAM variant),
and if it's newer, one click downloads and installs it with a progress
bar. Transient network hiccups are retried automatically (three attempts).
The device reboots into the new firmware; WiFi and audio settings
are kept.

There's also a manual fallback under "Manual update": build locally with
`pio run` and upload `.pio/build/esp32s3/firmware.bin` directly.

It's safe against bad updates: images that aren't a valid Warbler32
ESP32-S3 build are rejected before anything is written, and if a new
firmware crashes during startup the bootloader automatically boots the
previous version again on the next reset.

### Publishing a release (maintainers)

```bash
scripts/release.sh 1.0.0
```

Tags `v1.0.0`, builds both PSRAM variants, and publishes
`warbler32-quad.bin` + `warbler32-oct.bin` as GitHub release assets —
which is exactly what deployed devices look for.

## Factory reset

Two ways to wipe saved WiFi/audio settings and drop back into setup mode:

- **Web UI**: click **Factory Reset** at the bottom of the config page (asks
  for confirmation first).
- **BOOT button**: once the device is already running (not while powering
  on — GPIO0 is a strapping pin, so holding it during power-on puts the chip
  in USB download mode instead), press and hold the board's BOOT button for
  5 seconds. The status LED blinks orange while held; release early to
  cancel. Useful if the device is unreachable on its saved network.

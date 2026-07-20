# Warbler32

Warbler32 is a device for streaming RTSP audio to a bird identification server such as [BirdNET-Go](https://github.com/tphakala/birdnet-go). This is a low power device that can be solar and/or battery powered and placed further away (within WiFi range) from your house than you would if running the microphone on the BirdNET server itself (usually a raspberry pi). As an example, I'm running BirdNET-Go on a proxmox server and have 3 audio streams running to it at the same time. These 3 devices are located  about 600ft from each-other on my property so I can capture a wider range of birds.

Supports two microphone options:
- **INMP441** — a cheap I2S digital microphone breakout, wired directly to the board.
- **USB microphone (Cleaner Audio)** — any standard USB Audio Class (UAC 1.0) mic/headset, plugged
  into the board's native USB port.

No programming experience is required to use this — just follow the steps below.

## What you'll need

- An **ESP32-S3-DevKitC-1** board with PSRAM — N8R2, N16R8, or similar
  variants with two USB ports (native USB + UART) all work. The exact
  flash/PSRAM configuration is auto-detected when you flash it, so you don't
  need to figure out which one you have.
- A USB cable to connect it to your computer
- Either an **INMP441** microphone breakout, or a **UAC-class USB microphone**
- A computer running Windows, macOS, or Linux

## 1. Get the code

If you've never used GitHub before, the easiest way is to download a ZIP file —
no extra software required:

1. On this repository's GitHub page, click the green **`<> Code`** button (near
   the top right).
2. Click **Download ZIP**.
3. Find the downloaded ZIP file (usually in your `Downloads` folder) and
   extract/unzip it. You should end up with a folder named `warbler32` (or
   `warbler32-main`).

<details>
<summary>Prefer using Git instead?</summary>

If you have [Git](https://git-scm.com/downloads) installed, you can clone the
repository from a terminal instead:

```bash
git clone https://github.com/chrismyers2000/Warbler32.git
cd Warbler32
```
</details>

## 2. Install the tools

This project uses **PlatformIO**, a free tool for building and flashing ESP32
firmware. The easiest way to use it is through **VS Code**, a free code editor.

1. Install [Visual Studio Code](https://code.visualstudio.com/) (download the
   installer for your operating system and run it, accepting the defaults).
2. Open VS Code.
3. Click the **Extensions** icon in the left-hand sidebar (it looks like four
   small squares), or press `Ctrl+Shift+X` (`Cmd+Shift+X` on macOS).
4. Search for **"PlatformIO IDE"** and click **Install**.
5. Wait for the install to finish — this can take a few minutes. VS Code may
   ask to reload/restart itself; let it.
6. Once installed, a new alien-head icon will appear in the left sidebar —
   that's PlatformIO.

## 3. Open the project

1. In VS Code, go to **File → Open Folder...**
2. Select the `warbler32` folder from step 1 (the one containing `platformio.ini`).
3. PlatformIO will automatically detect the project. Give it a minute — the
   first time you open it, PlatformIO downloads the ESP32-S3 toolchain and
   dependencies in the background (you'll see progress in the bottom status bar).

## 4. Connect and flash the board

1. Connect the ESP32-S3-DevKitC-1 to your computer with a USB cable, using the
   port labeled **"UART"** (not "USB" — that port is only used later, for a USB
   microphone).
2. Click the PlatformIO alien-head icon in the sidebar.
3. Under **PROJECT TASKS → esp32s3 → General**, click **Upload**.
   - (If you prefer the command line, you can instead run `pio run -t upload`
     from a terminal opened in the project folder.)
4. PlatformIO will compile the firmware and flash it to the board — this takes
   a minute or two the first time. When it finishes, you'll see
   `[SUCCESS]` in the terminal output at the bottom of VS Code.

### Troubleshooting the upload

- **Windows**: if the board isn't detected, you may need to install the
  [CP210x or CH340 USB driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
  (whichever chip your board uses — check the sticker/silkscreen near the USB
  port) so your computer recognizes it as a serial device.
- **macOS**: similarly, install the CP210x/CH340 driver if the board doesn't
  show up.
- **Linux**: if you get a "Permission denied" error on the serial port, add
  your user to the `dialout` group (`sudo usermod -aG dialout $USER`), then
  log out and back in.
- If PlatformIO can't find the board at all, try a different USB cable (some
  are power-only and don't carry data) or a different USB port.

## Wiring (INMP441 only — skip if using a USB microphone)

| INMP441 pin | ESP32-S3 GPIO |
|---|---|
| WS  | 42 |
| SCK | 41 |
| SD  | 40 |
| L/R | GND |

## Wiring (USB microphone only — skip if using an INMP441)

Plug the mic into the board's native **"USB"** port — not the **"UART"** port
used for flashing. If the mic never gets detected, check whether your board has
a **"USB-OTG"** solder jumper/pad that needs bridging — some DevKitC-1 revisions
don't supply 5V to the native USB port unless it's bridged.

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

## Updating the firmware over WiFi

After the first USB flash, updates no longer need a cable. Build the new
firmware (`pio run`), then on the config page's **Firmware** card choose
`.pio/build/esp32s3/firmware.bin` and click **Upload & Install**. The device
verifies the image, flashes it to a spare slot, and reboots into it —
about 10 seconds total. Your WiFi and audio settings are kept.

It's safe against bad updates: uploads that aren't a valid Warbler32
ESP32-S3 image are rejected before anything is written, and if a new build
crashes during startup the bootloader automatically boots the previous
firmware again on the next reset.

## Factory reset

Two ways to wipe saved WiFi/audio settings and drop back into setup mode:

- **Web UI**: click **Factory Reset** at the bottom of the config page (asks
  for confirmation first).
- **BOOT button**: once the device is already running (not while powering
  on — GPIO0 is a strapping pin, so holding it during power-on puts the chip
  in USB download mode instead), press and hold the board's BOOT button for
  5 seconds. The status LED blinks orange while held; release early to
  cancel. Useful if the device is unreachable on its saved network.

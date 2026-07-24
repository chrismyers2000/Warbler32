#!/usr/bin/env bash
# One-command Warbler32 installer: detects the connected board's PSRAM
# variant (Quad/N8R2 vs Octal/N16R8), downloads the matching factory image
# from the latest GitHub release, and flashes it — no PlatformIO/VS Code,
# no cloning the repo.
#
#   curl -fsSL https://raw.githubusercontent.com/chrismyers2000/Warbler32/master/scripts/install.sh | bash
#
# No curl? (not guaranteed on a fresh minimal/server install, unlike wget):
#
#   wget -qO- https://raw.githubusercontent.com/chrismyers2000/Warbler32/master/scripts/install.sh | bash
#
# or, if you already have the repo cloned:
#
#   bash scripts/install.sh
#
# Requires Python 3 + pip (to install esptool if it isn't already present;
# this script installs pip itself too, via apt, if it's missing) and the
# board connected via its "UART" USB port.
#
# WARNING: this wipes any saved settings on the board (WiFi, audio config —
# everything), the same as a factory reset — the factory image spans the
# whole region of flash where settings live. Only meant for first-time
# setup or intentionally starting over; don't point this at an
# already-configured device unless that's what you want.
set -euo pipefail

REPO="chrismyers2000/Warbler32"

# curl is only Debian "optional" priority — not guaranteed on a fresh
# minimal/server install, unlike wget ("standard" priority). Since this
# script itself is normally fetched with curl, don't also assume curl is
# what's available for its own HTTP calls further down.
fetch_stdout() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- "$1"
    else
        echo "error: need curl or wget, and neither is installed." >&2
        echo "  sudo apt install -y curl" >&2
        exit 1
    fi
}
fetch_file() {
    if command -v curl >/dev/null 2>&1; then
        curl -fL -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$2" "$1"
    else
        echo "error: need curl or wget, and neither is installed." >&2
        echo "  sudo apt install -y curl" >&2
        exit 1
    fi
}

echo "==> Checking for esptool..."
if ! command -v esptool.py >/dev/null 2>&1; then
    if ! python3 -m pip --version >/dev/null 2>&1; then
        # Raspberry Pi OS ships python3-pip out of the box; plain
        # Debian/Ubuntu (especially server/minimal installs) often don't.
        if command -v apt-get >/dev/null 2>&1; then
            echo "==> pip isn't installed; installing it (sudo apt-get install python3-pip)..."
            sudo apt-get update && sudo apt-get install -y python3-pip
        else
            echo "error: Python 3's pip module isn't available, and this isn't an" >&2
            echo "apt-based system, so it can't be installed automatically." >&2
            echo "Install pip for Python 3 yourself, then re-run this script." >&2
            exit 1
        fi
    fi
    echo "==> Installing esptool (python3 -m pip install --user esptool)..."
    PIP_OUT="$(python3 -m pip install --user -U esptool 2>&1)" || {
        if echo "$PIP_OUT" | grep -q "externally-managed-environment"; then
            # Debian/Ubuntu 12+/23.04+ (PEP 668) block a plain `pip install`
            # outside a venv. esptool is a small, dependency-light CLI tool,
            # so --break-system-packages is the pragmatic fix here rather
            # than standing up a whole venv for one command.
            echo "==> This system blocks plain pip installs (PEP 668);"
            echo "    retrying with --break-system-packages..."
            python3 -m pip install --user --break-system-packages -U esptool
        else
            echo "$PIP_OUT" >&2
            exit 1
        fi
    }
    export PATH="$HOME/.local/bin:$PATH"
fi
if ! command -v esptool.py >/dev/null 2>&1; then
    echo "error: esptool.py still not found on PATH after installing it." >&2
    echo "Open a new terminal and try again, or run this manually:" >&2
    echo "  python3 -m pip install --user esptool" >&2
    exit 1
fi

echo "==> Detecting connected board (this reads its PSRAM size to pick the right build)..."
DETECT_OUT="$(esptool.py --chip esp32s3 chip_id 2>&1)" || {
    echo "$DETECT_OUT" >&2
    echo "" >&2
    echo "error: couldn't talk to a board." >&2
    echo "- Is it plugged in via the port labeled \"UART\" (not \"USB\")?" >&2
    echo "- On Linux, a \"Permission denied\" on the serial port usually means" >&2
    echo "  you need: sudo usermod -aG dialout \$USER   (then log out/back in)" >&2
    exit 1
}
echo "$DETECT_OUT"

if echo "$DETECT_OUT" | grep -q "Embedded PSRAM 2MB"; then
    VARIANT="quad"
elif echo "$DETECT_OUT" | grep -qE "Embedded PSRAM (8|16|4)MB"; then
    VARIANT="oct"
else
    echo "" >&2
    echo "error: couldn't determine the PSRAM variant from the board output above." >&2
    echo "Flash manually instead — see the README's esptool instructions." >&2
    exit 1
fi
echo "==> Detected variant: $VARIANT"

ASSET="warbler32-${VARIANT}-factory.bin"
echo "==> Looking up the latest release's $ASSET..."
ASSET_URL="$(fetch_stdout "https://api.github.com/repos/${REPO}/releases/latest" \
    | python3 -c "
import json, sys
data = json.load(sys.stdin)
for asset in data['assets']:
    if asset['name'] == '$ASSET':
        print(asset['browser_download_url'])
        break
")"
if [ -z "$ASSET_URL" ]; then
    echo "error: latest release has no $ASSET asset." >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "==> Downloading $ASSET..."
fetch_file "$ASSET_URL" "$TMP/$ASSET"

echo ""
echo "About to flash $ASSET — this ERASES any saved WiFi/audio settings"
echo "on the board (same as a factory reset)."
read -r -p "Continue? [y/N] " REPLY < /dev/tty
case "$REPLY" in
    [yY]|[yY][eE][sS]) ;;
    *) echo "Cancelled."; exit 1 ;;
esac

echo "==> Flashing..."
esptool.py --chip esp32s3 write_flash 0x0 "$TMP/$ASSET"

echo ""
echo "Done! Connect to the device's own \"Warbler32-Setup\" WiFi network"
echo "(password: warbler32) and browse to http://192.168.4.1/ to finish setup."

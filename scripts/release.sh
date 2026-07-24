#!/usr/bin/env bash
# Publish a Warbler32 release to GitHub with per-variant firmware assets.
#
#   scripts/release.sh 1.0.0 [notes_file]
#
# Tags the current commit v1.0.0, builds both PSRAM variants (quad + oct),
# and publishes two kinds of asset per variant:
#   - warbler32-<variant>-ota.bin      — app-partition-only image. Devices
#     find this via the web UI's "Check for Updates" button (OTA), which
#     writes it into the *other* OTA app partition — existing settings and
#     the currently-running partition are untouched either way.
#   - warbler32-<variant>-factory.bin  — bootloader + partition table + app
#     merged into one image starting at flash offset 0x0, for a first-time
#     flash with esptool.py (or any other raw flasher) and no PlatformIO/
#     VS Code install. Flashing this WIPES saved settings (WiFi, audio,
#     everything in NVS) same as a factory reset, because its byte range
#     covers the whole low flash region including the NVS partition — that's
#     expected for a blank board, but never point an already-configured
#     device at it.
#
# The "-ota" filename must exactly match what src/ota_update.c's
# ota_github_check() constructs (warbler32-%s-ota.bin) — devices already
# out in the field running older firmware that still expects the old
# "warbler32-<variant>.bin" name (no "-ota") will report "no build for
# this board" until manually re-flashed once onto firmware built after
# this rename.
#
# The tag is created BEFORE building so the firmware's embedded version
# (git describe) equals the tag name — that's what devices compare against.
#
# Also updates docs/ (the browser-based web flasher, served over GitHub
# Pages from master:/docs) with this release's factory images and version,
# as a separate untagged commit after the GitHub release is published. Kept
# separate from the tag-then-build flow above on purpose: the git tag
# identifies the firmware source commit, not the website.
#
# notes_file: path to a short human-readable summary of what changed, used
# as the GitHub release body. If omitted, falls back to GitHub's
# auto-generated commit-list notes.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: scripts/release.sh <version> [notes_file]   e.g. scripts/release.sh 1.0.0}"
NOTES_FILE="${2:-}"
TAG="v${VERSION#v}"
PIO="${PIO:-$HOME/.local/bin/pio}"

if [ -n "$NOTES_FILE" ] && [ ! -f "$NOTES_FILE" ]; then
    echo "error: notes file not found: $NOTES_FILE" >&2
    exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
    echo "error: working tree not clean — commit or stash first" >&2
    exit 1
fi
if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "error: tag $TAG already exists" >&2
    exit 1
fi

git tag -a "$TAG" -m "Warbler32 $TAG"
echo "tagged $TAG (if the build fails: git tag -d $TAG)"

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

for variant in quad oct; do
    echo ""
    echo "=== building $variant variant ==="
    rm -f sdkconfig.esp32s3
    WARBLER32_BOARD="$variant" "$PIO" run
    cp .pio/build/esp32s3/firmware.bin "$OUT/warbler32-$variant-ota.bin"

    # Read flash settings from the build's own flasher_args.json rather than
    # hardcoding them, so a future flash_mode/size/freq change (e.g. a new
    # board variant) doesn't silently desync the factory image from what
    # PlatformIO's own `pio run -t upload` actually flashes.
    FLASH_MODE=$(python3 -c "import json; print(json.load(open('.pio/build/esp32s3/flasher_args.json'))['flash_settings']['flash_mode'])")
    FLASH_FREQ=$(python3 -c "import json; print(json.load(open('.pio/build/esp32s3/flasher_args.json'))['flash_settings']['flash_freq'])")
    FLASH_SIZE=$(python3 -c "import json; print(json.load(open('.pio/build/esp32s3/flasher_args.json'))['flash_settings']['flash_size'])")

    "$PIO" pkg exec --package "tool-esptoolpy" -- esptool.py --chip esp32s3 merge_bin \
        -o "$OUT/warbler32-$variant-factory.bin" \
        --flash_mode "$FLASH_MODE" --flash_freq "$FLASH_FREQ" --flash_size "$FLASH_SIZE" \
        0x0     .pio/build/esp32s3/bootloader.bin \
        0x8000  .pio/build/esp32s3/partitions.bin \
        0x10000 .pio/build/esp32s3/firmware.bin
done

# Drop the forced-variant sdkconfig so the next normal build re-detects
# the connected board instead of silently keeping the last release variant.
rm -f sdkconfig.esp32s3 sdkconfig.defaults.detected

git push origin "$TAG"
ASSETS=(
    "$OUT/warbler32-quad-ota.bin"
    "$OUT/warbler32-quad-factory.bin"
    "$OUT/warbler32-oct-ota.bin"
    "$OUT/warbler32-oct-factory.bin"
)
if [ -n "$NOTES_FILE" ]; then
    gh release create "$TAG" "${ASSETS[@]}" \
        --title "Warbler32 $TAG" --notes-file "$NOTES_FILE"
else
    gh release create "$TAG" "${ASSETS[@]}" \
        --title "Warbler32 $TAG" --generate-notes
fi

echo ""
echo "released $TAG — devices will see it on their next 'Check for Updates'"

echo ""
echo "=== updating web flasher (docs/) ==="
cp "$OUT/warbler32-quad-factory.bin" docs/firmware/warbler32-quad-factory.bin
cp "$OUT/warbler32-oct-factory.bin"  docs/firmware/warbler32-oct-factory.bin
python3 -c "
import json
for variant in ('quad', 'oct'):
    path = f'docs/manifest-{variant}.json'
    with open(path) as f:
        m = json.load(f)
    m['version'] = '$VERSION'
    with open(path, 'w') as f:
        json.dump(m, f, indent=2)
        f.write('\n')
"
if [ -n "$(git status --porcelain docs/)" ]; then
    git add docs/
    git commit -m "Update web flasher to $TAG"
    git push origin master
    echo "docs/ updated and pushed"
else
    echo "docs/ already up to date, nothing to commit"
fi

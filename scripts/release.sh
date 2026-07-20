#!/usr/bin/env bash
# Publish a Warbler32 release to GitHub with per-variant firmware assets.
#
#   scripts/release.sh 1.0.0
#
# Tags the current commit v1.0.0, builds both PSRAM variants (quad + oct),
# and publishes warbler32-quad.bin / warbler32-oct.bin as release assets.
# Devices find the release via the web UI's "Check for Updates" button,
# which downloads the asset matching their own board variant.
#
# The tag is created BEFORE building so the firmware's embedded version
# (git describe) equals the tag name — that's what devices compare against.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:?usage: scripts/release.sh <version>   e.g. scripts/release.sh 1.0.0}"
TAG="v${VERSION#v}"
PIO="${PIO:-$HOME/.local/bin/pio}"

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
    cp .pio/build/esp32s3/firmware.bin "$OUT/warbler32-$variant.bin"
done

# Drop the forced-variant sdkconfig so the next normal build re-detects
# the connected board instead of silently keeping the last release variant.
rm -f sdkconfig.esp32s3 sdkconfig.defaults.detected

git push origin "$TAG"
gh release create "$TAG" \
    "$OUT/warbler32-quad.bin" \
    "$OUT/warbler32-oct.bin" \
    --title "Warbler32 $TAG" --generate-notes

echo ""
echo "released $TAG — devices will see it on their next 'Check for Updates'"

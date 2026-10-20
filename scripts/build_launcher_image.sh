#!/usr/bin/env bash
# Build DeskBar as an app for WaveShare_3.49_Launcher and merge everything the
# Launcher's SD installer needs into one image: it reads the partition table at
# 0x8000 and copies the app and the "littlefs" data partition out of the file.
# Run on the build box (auto), not on windows-box.
#   bash scripts/build_launcher_image.sh   ->  sketch/deskbar-<version>-launcher.bin
set -euo pipefail
cd "$(dirname "$0")/../sketch"

PIO="${PIO:-$HOME/.local/bin/pio}"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/penv/bin/esptool}"
ENV=deskbar-launcher
BUILD=".pio/build/$ENV"

# Filesystem first: a buildfs run can clean the build dir, which would take
# the firmware with it if it ran second.
"$PIO" run -e "$ENV" -t buildfs
"$PIO" run -e "$ENV"
for f in bootloader.bin partitions.bin firmware.bin littlefs.bin; do
    [ -f "$BUILD/$f" ] || { echo "missing $BUILD/$f" >&2; exit 1; }
done

VERSION=$(grep -oP 'define TUNEBAR_VERSION_STR\s+"\K[^"]+' include/version.h)
OUT="deskbar-${VERSION}-launcher.bin"

# Offsets from partitions/launcher_app5MB_littlefs.csv
"$ESPTOOL" --chip esp32s3 merge-bin -o "$OUT" \
    --flash-mode dio --flash-size 16MB \
    0x0 "$BUILD/bootloader.bin" \
    0x8000 "$BUILD/partitions.bin" \
    0x10000 "$BUILD/firmware.bin" \
    0x510000 "$BUILD/littlefs.bin"

ls -la "$OUT"

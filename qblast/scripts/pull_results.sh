#!/usr/bin/env bash
# Pull tuner result JSON files from the device into scripts/database/json/.
# Run this after the tuner finishes a batch of cfg_ids.
#
# Env:
#   ANDROID_DEVICE  adb serial. Defaults to OnePlus 15.

set -euo pipefail
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE="${ANDROID_DEVICE:-3C15AU002CL00000}"
DEST="$REPO_ROOT/scripts/database/json"

mkdir -p "$DEST"
echo "[pull] $DEVICE: /sdcard/qblast/results/  ->  $DEST/"
if ! adb -s "$DEVICE" pull /sdcard/qblast/results/. "$DEST/"; then
    echo "[pull] no results yet (expected on first run)"
fi

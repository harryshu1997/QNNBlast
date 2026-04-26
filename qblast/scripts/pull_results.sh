#!/usr/bin/env bash
# Pull tuner result JSON files from the device into scripts/database/json/.
# The APK writes them to its app-specific external dir, which is adb-pullable
# without any storage permission grant.
#
# Env:
#   ANDROID_DEVICE / ANDROID_SERIAL  adb serial. Defaults to OnePlus 15.

set -euo pipefail
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEVICE="${ANDROID_DEVICE:-${ANDROID_SERIAL:-3C15AU002CL00000}}"
DEST="$REPO_ROOT/scripts/database/json"
SRC="/sdcard/Android/data/com.qblast.tuner/files/results"

mkdir -p "$DEST"
echo "[pull] $DEVICE: $SRC  ->  $DEST/"
# `adb pull SRC DEST/` creates DEST/<basename of SRC>/ under DEST. We want the
# JSON files merged directly under $DEST. Pull into a tmp parent, then move up.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
if ! adb -s "$DEVICE" pull "$SRC" "$TMP/"; then
    echo "[pull] no results yet (run trigger_tune.sh first)"
    exit 0
fi
if [[ -d "$TMP/results" ]]; then
    # mv won't clobber via shell glob unless dotfiles too — JSON files have no
    # leading dot so this is fine. Use cp -f then rm to handle re-runs cleanly.
    cp -f "$TMP/results"/*.json "$DEST/" 2>/dev/null || true
fi

echo "[pull] $DEST/ now contains:"
ls -la "$DEST" | grep -E "\.json$" | head -20

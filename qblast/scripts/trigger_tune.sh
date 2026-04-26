#!/usr/bin/env bash
# Send a TUNE broadcast to the qblast-tuner APK on the device.
# Usage:
#   trigger_tune.sh --cfg-id 42 --shape 4096_4096_n1
#
# Env:
#   ANDROID_DEVICE  adb serial. Defaults to OnePlus 15.

set -euo pipefail
DEVICE="${ANDROID_DEVICE:-3C15AU002CL00000}"
CFG_ID=0
SHAPE="default"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cfg-id) CFG_ID="$2"; shift 2 ;;
        --shape)  SHAPE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

# Explicit component (-n) is required: Android 8+ blocks implicit broadcasts to
# manifest-registered receivers in unrelated apps.
adb -s "$DEVICE" shell am broadcast \
    -a com.qblast.TUNE \
    -n com.qblast.tuner/.TuneBroadcastReceiver \
    --ei cfg_id "$CFG_ID" \
    --es shape "$SHAPE"

#!/usr/bin/env bash
# Send a TUNE broadcast to the qblast-tuner APK on the device.
# Usage:
#   trigger_tune.sh --shape <shape> [--cfg-id N | --auto]
#                   [--seed S] [--warmup W] [--iters I]
#
# <shape> dispatch:
#   ping            - qblast_hello roundtrip
#   M_K_q           - gemv_w4a16 baseline, e.g. 4096_4096_64
#
# cfg_id selection:
#   --cfg-id N      explicit variant; loads libgemv_w4a16_v{N}_skel.so
#   --auto          query the tuned database (sd8e_gen5.hpp) for the
#                   winning variant for this shape; falls back to default
#                   if the shape isn't in the leaderboard
#
# Defaults: cfg-id=0, seed=1234, warmup=5, iters=50.
#
# Env:
#   ANDROID_DEVICE / ANDROID_SERIAL  adb serial. Defaults to OnePlus 15.

set -euo pipefail
DEVICE="${ANDROID_DEVICE:-${ANDROID_SERIAL:-3C15AU002CL00000}}"
CFG_ID=0
SHAPE="default"
SEED=1234
WARMUP=5
ITERS=50

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cfg-id) CFG_ID="$2"; shift 2 ;;
        --auto)   CFG_ID=-1; shift ;;
        --shape)  SHAPE="$2"; shift 2 ;;
        --seed)   SEED="$2"; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --iters)  ITERS="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

# Explicit component (-n): Android 8+ blocks implicit broadcasts to
# manifest-registered receivers in unrelated apps.
adb -s "$DEVICE" shell am broadcast \
    -a com.qblast.TUNE \
    -n com.qblast.tuner/.TuneBroadcastReceiver \
    --ei cfg_id "$CFG_ID" \
    --es shape "$SHAPE" \
    --ei seed "$SEED" \
    --ei warmup "$WARMUP" \
    --ei iters "$ITERS"

#!/usr/bin/env bash
# Build a Hexagon DSP skel and adb-push it to /data/local/tmp/ on the device.
# Usage:
#   build_dsp.sh <kernel_name> [variant]
#
# Example:
#   build_dsp.sh qblast_hello                         # Release v81
#   build_dsp.sh qblast_hello hexagon_Debug_toolv19_v81
#
# Prereq: source the Hexagon SDK env first.
#   conda activate research && unset HEXAGON_SDK_ROOT \
#     && source $HEXAGON_SDK_ROOT/setup_sdk_env.source
# (or just run the `qblast-env` zsh function which does the above.)
#
# Env:
#   ANDROID_DEVICE / ANDROID_SERIAL  adb serial. Defaults to OnePlus 15.
#   PUSH                              set to 0 to skip the adb push.

set -euo pipefail

KERNEL="${1:-}"
VARIANT="${2:-hexagon_Release_toolv19_v81}"
DEVICE="${ANDROID_DEVICE:-${ANDROID_SERIAL:-3C15AU002CL00000}}"
PUSH="${PUSH:-1}"

if [[ -z "$KERNEL" ]]; then
    sed -n '2,12p' "$0"
    exit 1
fi

if [[ -z "${HEXAGON_SDK_ROOT:-}" ]]; then
    echo "[build_dsp] ERROR: HEXAGON_SDK_ROOT not set. Run 'qblast-env' first." >&2
    exit 1
fi
if [[ -z "${SDK_SETUP_ENV:-}" ]]; then
    echo "[build_dsp] ERROR: SDK_SETUP_ENV not set — Hexagon SDK env not fully sourced." >&2
    exit 1
fi

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_DIR="$REPO_ROOT/src/kernels/hexagon/$KERNEL"

if [[ ! -d "$KERNEL_DIR" ]]; then
    echo "[build_dsp] ERROR: $KERNEL_DIR does not exist" >&2
    exit 1
fi

cd "$KERNEL_DIR"
echo "[build_dsp] cd $KERNEL_DIR && make V=$VARIANT tree"
make V="$VARIANT" tree

SKEL="$KERNEL_DIR/$VARIANT/ship/lib${KERNEL}_skel.so"
if [[ ! -f "$SKEL" ]]; then
    # Some variants drop the .so directly in $VARIANT/ rather than ship/.
    ALT="$KERNEL_DIR/$VARIANT/lib${KERNEL}_skel.so"
    if [[ -f "$ALT" ]]; then
        SKEL="$ALT"
    else
        echo "[build_dsp] ERROR: skel not found under $KERNEL_DIR/$VARIANT/" >&2
        find "$KERNEL_DIR/$VARIANT/" -name "*.so" >&2 || true
        exit 1
    fi
fi

echo "[build_dsp] built $SKEL"

if [[ "$PUSH" == "1" ]]; then
    echo "[build_dsp] adb -s $DEVICE push $SKEL /data/local/tmp/"
    adb -s "$DEVICE" push "$SKEL" /data/local/tmp/
fi

#!/usr/bin/env bash
# Best-effort device prep before APK install / tuning. No root assumed.
# - Disables Play Protect ADB install verifier (avoids INSTALL_FAILED_VERIFICATION_FAILURE).
# - Prints SoC / governor / DSP info as a sanity check.
#
# Env:
#   ANDROID_DEVICE  adb serial. Defaults to OnePlus 15.

set -euo pipefail
DEVICE="${ANDROID_DEVICE:-3C15AU002CL00000}"

run() { adb -s "$DEVICE" "$@"; }

echo "[prep] device: $DEVICE"
run shell getprop ro.product.model
run shell getprop ro.product.cpu.abi
run shell getprop ro.soc.model || true
run shell getprop ro.build.version.release

echo "[prep] disabling Play Protect ADB install verifier"
run shell settings put global verifier_verify_adb_installs 0
run shell settings put global package_verifier_enable 0

echo "[prep] CPU governors (for visibility — locking requires root or vendor API):"
run shell 'for i in 0 1 2 3 4 5 6 7; do
    g=$(cat /sys/devices/system/cpu/cpu$i/cpufreq/scaling_governor 2>/dev/null || echo no-access)
    echo "  cpu$i: $g"
done'

echo "[prep] /sdcard/qblast/results dir"
run shell mkdir -p /sdcard/qblast/results

echo "[prep] done"

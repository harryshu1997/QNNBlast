#!/usr/bin/env bash
# Best-effort device prep before APK install + tuning. No root assumed.
# - Confirms adb sees the device.
# - Disables Play Protect's ADB-install verifier (otherwise blocks first
#   install with a "scanning for security threats…" dialog).
# - Prints SoC + DSP + storage info as sanity output.
#
# Env:
#   ANDROID_SERIAL / ANDROID_DEVICE  adb serial. Defaults to OnePlus 15.

set -euo pipefail
DEVICE="${ANDROID_SERIAL:-${ANDROID_DEVICE:-3C15AU002CL00000}}"

run() { adb -s "$DEVICE" "$@"; }

# 1. Device reachable?
state=$(adb -s "$DEVICE" get-state 2>/dev/null || echo offline)
if [[ "$state" != "device" ]]; then
    echo "[prep] ERROR: adb -s $DEVICE state is '$state'; expected 'device'." >&2
    echo "       Run 'adb devices' to confirm; replug if 'unauthorized'." >&2
    exit 1
fi

echo "[prep] device $DEVICE state=$state"
run shell getprop ro.product.model
run shell getprop ro.product.cpu.abi
run shell getprop ro.soc.model || true
run shell getprop ro.build.version.release

# 2. Verifier off (idempotent — settings persist across reboots on Android 16).
echo "[prep] disabling Play Protect ADB install verifier"
run shell settings put global verifier_verify_adb_installs 0
run shell settings put global package_verifier_enable 0

# 3. DSP-side sanity: cdsprpc device node should exist (vendor partition).
echo "[prep] cdsprpc node:"
run shell 'ls -l /dev/fastrpc-cdsp 2>/dev/null || echo "  WARN: /dev/fastrpc-cdsp missing"'

# 4. Skel directory is /data/local/tmp; APK looks for libgemv_w4a16_v*_skel.so there.
echo "[prep] /data/local/tmp gemv skels:"
run shell 'ls -1 /data/local/tmp/libgemv_w4a16*.so 2>/dev/null || echo "  (none yet — push via scripts/push_skels.sh)"'

# 5. CPU governor visibility (locking requires root or vendor API).
echo "[prep] CPU governors (visibility only; locking needs root):"
run shell 'for i in 0 1 2 3 4 5 6 7; do
    g=$(cat /sys/devices/system/cpu/cpu$i/cpufreq/scaling_governor 2>/dev/null || echo no-access)
    echo "  cpu$i: $g"
done'

# 6. App-specific external dir for tuner output JSONs.
echo "[prep] APK results dir:"
run shell 'mkdir -p /sdcard/Android/data/com.qblast.tuner/files/results && \
           ls -ld /sdcard/Android/data/com.qblast.tuner/files/results 2>/dev/null || \
           echo "  (will be created by APK on first run)"'

echo "[prep] done"

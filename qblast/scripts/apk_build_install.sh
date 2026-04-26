#!/usr/bin/env bash
# Build the qblast-tuner APK and install it on the device.
#
# This script does NOT build DSP skels — run scripts/build_dsp.sh <kernel> first
# (which also adb-pushes the .so to /data/local/tmp/). The split is intentional
# so you can rebuild only the changed side (DSP vs. APK) when iterating.
#
# Requires:
#   - Android SDK (cmdline-tools or Android Studio); ANDROID_HOME or
#     ANDROID_SDK_ROOT set, OR android/local.properties pointing to it.
#   - JDK 17 reachable (qblast/android/gradle.properties pins
#     org.gradle.java.home).
#   - Gradle wrapper inside qblast/android/.
#
# Env:
#   ANDROID_DEVICE / ANDROID_SERIAL  adb serial. Defaults to OnePlus 15.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$REPO_ROOT/android"
DEVICE="${ANDROID_DEVICE:-${ANDROID_SERIAL:-3C15AU002CL00000}}"

cd "$ANDROID_DIR"

if [[ ! -x ./gradlew ]]; then
    echo "[apk] ERROR: $ANDROID_DIR/gradlew missing." >&2
    echo "       Bootstrap once with:" >&2
    echo "         (cd $ANDROID_DIR && /tmp/gradle-7.4/bin/gradle wrapper --gradle-version 7.4)" >&2
    exit 1
fi

echo "[apk] assembleDebug"
./gradlew --no-daemon assembleDebug

APK="$ANDROID_DIR/app/build/outputs/apk/debug/app-debug.apk"
[[ -f "$APK" ]] || { echo "[apk] APK not produced at $APK" >&2; exit 1; }

echo "[apk] installing $APK on $DEVICE"
adb -s "$DEVICE" install -r -t -g "$APK"
echo "[apk] done"

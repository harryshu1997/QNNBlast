#!/usr/bin/env bash
# Build the qblast-tuner APK and install it on the OnePlus 15.
# Requires:
#   - Android SDK (cmdline-tools or Android Studio); ANDROID_HOME or ANDROID_SDK_ROOT set,
#     OR an android/local.properties file pointing to it.
#   - JDK 11+ on PATH (java -version).
#   - Gradle wrapper inside qblast/android/. If gradlew is missing, run once:
#         cd qblast/android && gradle wrapper --gradle-version 7.4
#     (needs a system 'gradle' >= 7.x on PATH the first time only.)
#
# Env:
#   ANDROID_DEVICE  adb device serial. Defaults to OnePlus 15 (3C15AU002CL00000).

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$REPO_ROOT/android"
DEVICE="${ANDROID_DEVICE:-3C15AU002CL00000}"

cd "$ANDROID_DIR"

if [[ ! -x ./gradlew ]]; then
    echo "[apk] ERROR: $ANDROID_DIR/gradlew is missing or not executable." >&2
    echo "       Run once with a system gradle on PATH:" >&2
    echo "         (cd $ANDROID_DIR && gradle wrapper --gradle-version 7.4)" >&2
    exit 1
fi

echo "[apk] assembleDebug"
./gradlew --no-daemon assembleDebug

APK="$ANDROID_DIR/app/build/outputs/apk/debug/app-debug.apk"
[[ -f "$APK" ]] || { echo "[apk] APK not produced at $APK" >&2; exit 1; }

echo "[apk] installing $APK on $DEVICE"
adb -s "$DEVICE" install -r -t -g "$APK"
echo "[apk] done"

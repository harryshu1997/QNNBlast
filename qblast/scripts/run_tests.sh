#!/usr/bin/env bash
# All-host-side qblast tests in one go. Doesn't touch the device.
#
# Runs:
#   1. Codegen round-trip test (scripts/database/test_codegen.py)
#   2. Database lookup test (src/database/test_database.cpp via cmake)
#   3. APK static-analysis: ./gradlew assembleDebug (compiles JNI + Java)
#
# Usage:
#   scripts/run_tests.sh             # all three
#   scripts/run_tests.sh --no-apk    # skip the slow gradle build
#
# Env:
#   QBLAST_BUILD_DIR  build dir for the C++ database test (default /tmp/qblast_test_build)

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${QBLAST_BUILD_DIR:-/tmp/qblast_test_build}"
SKIP_APK=0

for arg in "$@"; do
    case "$arg" in
        --no-apk) SKIP_APK=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

fail=0
section() { echo; echo "==== $1 ===="; }

section "1. codegen round-trip (scripts/database/test_codegen.py)"
if ! "$REPO_ROOT/scripts/database/test_codegen.py"; then
    fail=1
fi

section "2. database lookup (src/database/test_database.cpp)"
if ! cmake -S "$REPO_ROOT/src/database" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release > /dev/null; then
    echo "FAIL: cmake configure"
    fail=1
fi
if ! cmake --build "$BUILD_DIR" --target test_database > /dev/null; then
    echo "FAIL: cmake build"
    fail=1
fi
if ! "$BUILD_DIR/test_database"; then
    fail=1
fi

if [[ $SKIP_APK -eq 0 ]]; then
    section "3. APK build (./gradlew assembleDebug)"
    if [[ ! -x "$REPO_ROOT/android/gradlew" ]]; then
        echo "SKIP: $REPO_ROOT/android/gradlew not present (run gradle wrapper once)"
    elif (cd "$REPO_ROOT/android" && ./gradlew --no-daemon assembleDebug > /tmp/qblast_apk.log 2>&1); then
        echo "  ok: APK assembleDebug succeeded"
    else
        echo "FAIL: APK build (last 20 lines of log)"
        tail -20 /tmp/qblast_apk.log
        fail=1
    fi
else
    section "3. APK build  -- SKIPPED (--no-apk)"
fi

echo
if [[ $fail -eq 0 ]]; then
    echo "ALL HOST-SIDE TESTS PASSED"
    exit 0
fi
echo "TESTS FAILED"
exit 1

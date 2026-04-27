#!/usr/bin/env bash
# QNN HTP smoke on the device. Pushes the QAIRT-shipped InceptionV3 example
# to /data/local/tmp/qnn_test/ on the OnePlus 15 and runs qnn-net-run with
# the htp-v81 backend. Useful as a "QNN HTP works on retail unsigned PD"
# sanity check; the InceptionV3 example doesn't match qblast's matmul
# shapes, so this is for QNN-toolchain validation only.
#
# Confirmed: qnn-net-run + libQnnHtp.so + libQnnHtpV81Skel.so all run from
# adb shell with no root + no Qualcomm signing. plan §40-43 implied this
# wouldn't work; it does. The reason: libQnnHtp.so internally calls
# `remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE)` before opening
# the FastRPC handle, the same path qblast's APK uses.
#
# Prereqs:
#   - QAIRT 2.45 at /home/<user>/qairt/2.45.0.260326/ (or via $QAIRT)
#   - ANDROID_NDK_ROOT set (for the libc++_shared.so push)
#   - ANDROID_SERIAL or ANDROID_DEVICE pointing at the OP15

set -euo pipefail
QAIRT="${QAIRT:-/home/myid/zs89458/qairt/2.45.0.260326}"
DEVICE="${ANDROID_SERIAL:-${ANDROID_DEVICE:-3C15AU002CL00000}}"
NDK="${ANDROID_NDK_ROOT:-/home/myid/zs89458/android/android-ndk-r27c}"
TGT=/data/local/tmp/qnn_test

echo "[qnn] device=$DEVICE  qairt=$QAIRT"
adb -s "$DEVICE" shell "mkdir -p $TGT"

push() { adb -s "$DEVICE" push "$1" "$TGT/" > /dev/null; }
push "$QAIRT/bin/aarch64-android/qnn-net-run"
push "$QAIRT/lib/aarch64-android/libQnnHtp.so"
push "$QAIRT/lib/aarch64-android/libQnnHtpV81Stub.so"
push "$QAIRT/lib/aarch64-android/libQnnHtpPrepare.so"
push "$QAIRT/lib/hexagon-v81/unsigned/libQnnHtpV81Skel.so"
push "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"

# Pre-built model lib has to come from qnn-model-lib-generator; users
# without that env should run the QAIRT-shipped script first:
#   $QAIRT/examples/QNN/NetRun/android/android-qnn-net-run.sh -b htp-v81
# This script just exercises an existing libqnn_model_8bit_quantized.so
# if it's already on the device.
adb -s "$DEVICE" shell "
cd $TGT &&
chmod +x qnn-net-run &&
LD_LIBRARY_PATH=$TGT \
ADSP_LIBRARY_PATH=\"$TGT;/vendor/dsp/cdsp;/vendor/lib/rfsa/adsp;/system/lib/rfsa/adsp;/dsp\" \
./qnn-net-run --model libqnn_model_8bit_quantized.so \
              --input_list input_list_float.txt \
              --backend libQnnHtp.so \
              --output_dir output \
              --profiling_level basic > /sdcard/qnn_smoke.log 2>&1
echo \"qnn-net-run exit=\$?\"
tail -20 /sdcard/qnn_smoke.log
"

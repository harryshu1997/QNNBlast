# qblast

CLBlast-style auto-tuning library for the Qualcomm Hexagon HTP (NPU) on
Snapdragon 8 Elite Gen 5. See [`../qblast_plan.md`](../qblast_plan.md) for the
master plan.

## Phase 1, Week 1 — APK skeleton

This week ships an empty tuner APK that:

- loads `libqblast_tuner_jni.so` and sets `DSP_LIBRARY_PATH=/data/local/tmp`
  (so retail-device unsigned PD can find skel libs pushed via `adb push`)
- registers a `BroadcastReceiver` for the `com.qblast.TUNE` action, which
  forwards the broadcast extras (`cfg_id`, `shape`) into a `ForegroundService`
- the service logs the request to `logcat` (tag `qblast_svc`)

Real DSP execution arrives in Week 2 (baseline GEMV W4A16 kernel).

## Layout

    android/        APK Gradle project (qblast-tuner)
    cmake/          cross-compile toolchain files (host + Hexagon, populated later)
    include/        public C/C++ API headers (populated Week 7+)
    src/            ARM-side routines + tuner host driver + database (populated Week 4+)
      kernels/hexagon/    DSP-side parameterized kernels (Week 2+)
      tuning/host/        host-side tuner driver
      tuning/ondevice/    on-device runner (linked into APK)
      database/           CLBlast-style JSON -> .hpp codegen + lookup
      routines/           shape -> kernel-variant dispatch
    bench/          on-device benchmark code (linked into APK)
    scripts/        build / device prep / tune trigger helpers
    docs/           architecture notes (populated Week 11)

## Quick smoke (Week 1 acceptance)

Once Android SDK + JDK are installed:

    cd qblast/android
    gradle wrapper --gradle-version 7.4         # one-time, needs system gradle
    cd ..
    scripts/prep_device.sh                       # disable Play Protect verifier
    scripts/apk_build_install.sh                 # ./gradlew assembleDebug + adb install
    adb logcat -c
    scripts/trigger_tune.sh --cfg-id 42 --shape 4096_4096_n1
    adb logcat -d | grep qblast                  # expect: qblast_rx then qblast_svc lines

`ANDROID_DEVICE` env var picks the adb serial (default: OnePlus 15
`3C15AU002CL00000`).

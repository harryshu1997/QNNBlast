# Device & workstation setup

What you need installed on a workstation and the OnePlus 15 to build,
push, and run qblast end-to-end. SSH-only Linux works (verified on
Ubuntu 24.04).

## Workstation prerequisites

| Tool                  | Version | Where it lives (on the verified box)            |
|-----------------------|---------|-------------------------------------------------|
| Hexagon SDK           | 6.5.0.1 | `/local/mnt/workspace/Qualcomm/Hexagon_SDK/6.5.0.1/` |
| QAIRT (QNN)           | 2.45.0.260326 | `/home/<user>/qairt/2.45.0.260326/` (optional, for QNN-side comparison) |
| Android cmdline-tools | latest  | `~/Android/Sdk/cmdline-tools/latest/`           |
| Android platform-tools| 34+     | included with cmdline-tools                     |
| Android NDK           | r27c    | `~/android/android-ndk-r27c/`                   |
| Android SDK platform  | android-33 | installed via `sdkmanager`                   |
| Build tools           | 33.0.2  | installed via `sdkmanager`                      |
| CMake                 | 3.22.1  | installed via `sdkmanager`                      |
| OpenJDK               | 17      | `/usr/lib/jvm/java-17-openjdk-amd64` (apt)      |
| Gradle                | 7.4     | bootstrapped via `gradle wrapper` once          |
| Python                | 3.10+   | system + a `research` conda env                 |
| adb                   | any     | `apt install android-sdk-platform-tools-common` |

### One-time Hexagon SDK env

The SDK setup script needs to be sourced in a way that resolves
`HEXAGON_SDK_ROOT` correctly. zsh's `${BASH_SOURCE[0]}` is empty, so
naïve `source $HEXAGON_SDK_ROOT/setup_sdk_env.source` from zsh leaves
the var pointing at `$PWD`. The repo-recommended workaround is a
zsh function `qblast-env`:

```zsh
qblast-env() {
    conda activate research
    unset HEXAGON_SDK_ROOT
    pushd /local/mnt/workspace/Qualcomm/Hexagon_SDK/6.5.0.1 > /dev/null
    source ./setup_sdk_env.source
    popd > /dev/null
}
```

After running, `echo $HEXAGON_SDK_ROOT` should show the SDK install
path, and `echo $SDK_SETUP_ENV` should be `Done`.

### One-time Gradle wrapper bootstrap

If `qblast/android/gradlew` doesn't exist yet:

```bash
cd /tmp && wget https://services.gradle.org/distributions/gradle-7.4-bin.zip
unzip gradle-7.4-bin.zip
cd ~/Documents/QNNBlast/qblast/android
/tmp/gradle-7.4/bin/gradle wrapper --gradle-version 7.4
```

After this, `gradlew` + `gradle/wrapper/*` are in place. Subsequent
builds use the project-local wrapper.

### Per-machine local.properties

`qblast/android/local.properties` is gitignored. Contents:

```properties
sdk.dir=/home/<user>/Android/Sdk
ndk.dir=/home/<user>/android/android-ndk-r27c
hexagon.sdk.root=/local/mnt/workspace/Qualcomm/Hexagon_SDK/6.5.0.1
```

## Device prerequisites (OnePlus 15, retail Android 16)

The flow uses **no root and no Qualcomm signing**. The APK runs in
`untrusted_app:s0`, opens `libcdsprpc.so`, calls
`remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE)` before
`*_open`, and pushes skel `.so` files to `/data/local/tmp/` from
where `DSP_LIBRARY_PATH=/data/local/tmp` makes them findable.

### One-time

1. Enable Developer Options (tap Build Number 7×).
2. Enable USB Debugging.
3. Enable **"通过 USB 安装应用"** ("Install via USB") — OnePlus-specific
   toggle, default off.
4. On first `adb install`, accept the on-screen prompt for "Allow USB
   install". This re-prompts each time the APK is uninstalled and reinstalled.

### Per-session

```bash
export ANDROID_SERIAL=3C15AU002CL00000   # OnePlus 15
export ANDROID_DEVICE=$ANDROID_SERIAL    # qblast scripts also accept this
```

`scripts/prep_device.sh` flips off the Play Protect ADB-install
verifier (which otherwise pops a "scanning…" dialog and can block
install):

```bash
scripts/prep_device.sh
```

Idempotent; safe to run any time.

### Environment that survives reboot

`adb shell settings put global verifier_verify_adb_installs 0` and
`package_verifier_enable 0` from prep_device.sh persist across reboots
on Android 16 OnePlus, but the per-app "通过 USB 安装应用" grant
sometimes resets after major OS updates.

## Common troubleshooting

| Symptom                                                       | Fix                                                   |
|---------------------------------------------------------------|-------------------------------------------------------|
| `adb devices` shows the device as `unauthorized`              | Replug, accept the dialog, tick "Always allow from this computer" |
| `INSTALL_FAILED_VERIFICATION_FAILURE`                         | Run `scripts/prep_device.sh` to disable verifier      |
| Gradle build error "AGP requires Java 17"                     | `gradle.properties` already pins `org.gradle.java.home=/usr/lib/jvm/java-17-openjdk-amd64`; ensure `apt install openjdk-17-jdk-headless` |
| `make` in DSP build complains "SDK_SETUP_ENV not set"         | Run `qblast-env` first; see One-time Hexagon SDK env above |
| `HEXAGON_SDK_ROOT` resolves to your `pwd` instead of the SDK path | zsh-vs-bash quirk; the `qblast-env` function pushd-wraps the source |
| `am broadcast` succeeds but kernel never runs                 | Android 12+ blocks FGS-from-broadcast; `TuneBroadcastReceiver` does the work directly in `onReceive`. If you see `ForegroundServiceStartNotAllowedException` in `adb logcat -b crash`, an old version of the receiver is installed — `apk_build_install.sh` to refresh |
| `scripts/build_dsp.sh` succeeds but kernel output is wrong   | The SDK Makefile reuses .o cache across "different" make invocations sharing OBJ_DIR. variant_builder.py sidesteps this with per-variant tmp directories; manual `build_dsp.sh` is for a single SO at a time |
| `gemv_w4a16_open` returns `0xc0000003 / AEE_EUNSUPPORTED`     | Forgot `remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE)` before open. The JNI does this; if you write a new caller, do the same |
| `rel_err = 51.x` with rest looking fine                       | The validator's per-element rel_err inflates on rows where ref ≈ 0. Phase-1 uses max-norm relative error; if you see this, check you're on the latest tuner-jni.cpp |

## Smoke checklist

After setup, in 60 seconds you should be able to:

```bash
qblast-env

cd ~/Documents/QNNBlast/qblast
scripts/prep_device.sh

# Build the 3 sample variants + push to device
scripts/variant_builder.py \
    --kernel gemv_w4a16 \
    --cfg-file scripts/database/cfgs/sample.json \
    --output-dir scripts/database/variants/
scripts/push_skels.sh scripts/database/variants/manifest.json

# Build + install the APK
scripts/apk_build_install.sh

# Auto-dispatch via the database
adb logcat -c
scripts/trigger_tune.sh --auto --shape 4096_4096_64 --warmup 2 --iters 3
sleep 25
adb logcat -d | grep "qblast_jni"
```

Expected: a `auto-dispatch: shape=4096_4096_64 -> 4096_4096_64
(cfg_id=0 Q=64)` line, then `validate: peak_ref=N max_abs_diff=N
max_norm_rel=N`, then a `gemv: ... status=ok` line.

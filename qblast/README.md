# qblast

CLBlast-style auto-tuning library for the **Qualcomm Hexagon HTP** (NPU on
Snapdragon 8 Elite Gen 5 / SM8850 / Hexagon v81), targeting LLM decode-shape
GEMV.

The full design and 12-week plan live in
[`../qblast_plan.md`](../qblast_plan.md) — this README only covers what's
shipped, how to run it, and what's next.

## Status — Phase 1 Week 2 complete

End-to-end pipeline working on a retail OnePlus 15 (CPH2749, Hexagon v81)
with **no root and no Qualcomm signing**. The host sends a single
`am broadcast`; the APK runs the kernel on cDSP under unsigned-PD; results
land as JSON in the APK's external files dir, ready for `adb pull`.

Single-config GEMV W4A16 baseline measured on `M=K=256, q_block=64` (warmup
3, iters 20):

| variant                                  | dsp_cycles_med | cycles/MAC | rel_err  | speedup |
|------------------------------------------|---------------:|-----------:|---------:|--------:|
| scalar W4 + FP16 x, FP32 acc             |      1,578,246 |       24.1 |  4.3e-04 |   1.00x |
| scalar W4 + int8 x, int32 acc per block  |        560,510 |       8.55 |  3.2e-03 |   2.82x |
| HVX-int, 4 blocks/iter, deinterleaved x  |        161,724 |       2.47 |  3.2e-03 |   9.76x |

Tolerance budget per plan §130 is `1e-2`; we're at 3.2e-03, well inside.
HVX path produces numerically *identical* output to the scalar-int path —
the speedup is pure throughput.

## Architecture (1 minute version)

```text
host (Linux x86_64)                    OnePlus 15 (Android 16, retail)
─────────────────                       ──────────────────────────────
adb push libgemv_w4a16_skel.so       /data/local/tmp/        (DSP skel)
adb shell am broadcast com.qblast.TUNE
                                       └─ TuneBroadcastReceiver
                                          └─ TunerService.nativeRunGemv()
                                             └─ rpcmem_alloc 4 buffers
                                             └─ FastRPC -> cDSP (unsigned PD)
                                                └─ libgemv_w4a16_skel.so
                                                   HVX kernel (vrmpy + vadd)
                                             └─ FP32 reference validator
                                             └─ write {cfg_id}.json
adb pull /sdcard/Android/data/com.qblast.tuner/files/results/
```

Three-layer build:

- **DSP skel** under `src/kernels/hexagon/<kernel>/` — Hexagon SDK Makefile
  produces `lib<kernel>_skel.so` for v81. Pushed to `/data/local/tmp/` and
  loaded by FastRPC at `_open()` time.
- **APK + JNI** under `android/` — Gradle 7.4 / AGP 7.2 / JDK 17. The JNI
  layer (`tuner-jni.cpp`) hosts `nativePing` and `nativeRunGemv`, caches
  FastRPC handles statically (so the 41 ms cold open cost is paid once
  per process), and writes per-cfg JSON to the app's external files dir.
- **Host scripts** under `scripts/` — adb shell helpers for build / push /
  trigger / pull.

## Quick start

Prerequisites:

- Linux x86_64 (verified on Ubuntu 24.04). SSH-only is fine; no GUI needed.
- Hexagon SDK 6.5.0.1 at `$HEXAGON_SDK_ROOT`.
- JDK 17 (`/usr/lib/jvm/java-17-openjdk-amd64`).
- Android cmdline-tools SDK at `$ANDROID_HOME` with `platforms;android-33`,
  `build-tools;33.0.2`, `cmake;3.22.1`. NDK r27c at
  `~/android/android-ndk-r27c`.
- An adb-attached OnePlus 15 (or any v81 device); set `ANDROID_SERIAL`.

One-time setup:

```bash
# qblast-env is a zsh function in ~/.zshrc:
#   conda activate research
#   pushd $HEXAGON_SDK_ROOT > /dev/null
#   source ./setup_sdk_env.source  &&  popd > /dev/null
qblast-env

# Bootstrap Gradle wrapper (only the first time):
cd qblast/android
/tmp/gradle-7.4/bin/gradle wrapper --gradle-version 7.4
```

Edit `qblast/android/local.properties` so it points at your installs:

```properties
sdk.dir=/home/<user>/Android/Sdk
ndk.dir=/home/<user>/android/android-ndk-r27c
hexagon.sdk.root=/local/mnt/workspace/Qualcomm/Hexagon_SDK/6.5.0.1
```

Run a single GEMV smoke:

```bash
cd qblast
export ANDROID_SERIAL=3C15AU002CL00000   # OnePlus 15 default

scripts/prep_device.sh                          # disable Play Protect verifier (one-shot)
scripts/build_dsp.sh gemv_w4a16                 # make tree + adb push
scripts/apk_build_install.sh                    # ./gradlew assembleDebug + adb install
adb logcat -c
scripts/trigger_tune.sh \
    --cfg-id 1 --shape 256_256_64 \
    --warmup 3 --iters 20
sleep 3
adb logcat -d | grep qblast | tail
scripts/pull_results.sh
cat scripts/database/json/1.json
```

The `--shape` parameter:

- `ping` — just exercises the FastRPC roundtrip via the qblast_hello skel,
  returns `magic=4950`.
- `M_K_q` — runs the gemv_w4a16 kernel; e.g. `4096_4096_64` is the
  Q/K/V/O-projection shape from LLaMA-7B decode.

Result JSON (one file per `cfg_id`):

```json
{
  "cfg_id": 1,
  "kernel": "gemv_w4a16",
  "shape": "256_256_64",
  "M": 256, "K": 256, "q_block": 64,
  "seed": 1234,
  "warmup": 3, "iters": 20,
  "dsp_cycles_min": 160477,
  "dsp_cycles_med": 161724,
  "dsp_cycles_max": 167546,
  "compute_us_min": 467,
  "compute_us_med": 476,
  "compute_us_max": 484,
  "max_rel_err": 3.213653e-03,
  "status": "ok"
}
```

Rank variants on `dsp_cycles_med` — `compute_us` includes ARM-side OS noise
that makes it a poor primary metric (see [`docs/`](docs/) once we write it
up properly).

## Layout

```text
qblast/
├── android/                APK Gradle project (com.qblast.tuner)
│   └── app/src/main/
│       ├── java/com/qblast/tuner/      MainActivity, TunerService, BroadcastReceiver
│       ├── cpp/                         tuner-jni.cpp + per-IDL stub.c
│       └── AndroidManifest.xml
├── src/kernels/hexagon/    DSP-side kernels (one subdir per kernel)
│   ├── common/                          shared helpers (FP16<->FP32 conv)
│   ├── qblast_hello/                    smoke-test skel; just returns 4950
│   └── gemv_w4a16/                      W4A16 GEMV; HVX path for q_block=64
├── scripts/                build / device-prep / tune-trigger helpers
│   ├── build_dsp.sh                     make tree + adb push for one kernel
│   ├── apk_build_install.sh             gradlew assembleDebug + adb install
│   ├── prep_device.sh                   disable verifier, print SoC info
│   ├── trigger_tune.sh                  am broadcast wrapper
│   ├── pull_results.sh                  adb pull tuner JSON
│   └── database/                        Tuner output collation (Week 6+)
├── cmake/                  Cross-compile toolchain files (Hexagon, populated later)
├── include/                Public C API (populated Week 7+)
├── bench/                  On-device benchmark code (Week 7+)
└── docs/                   Architecture notes (Week 11)
```

## Roadmap

- **Week 3** — Parameterize the gemv kernel (`#define TILE_M`, `Q_BLOCK`,
  `N_HW_THREADS`, …), add `variant_builder.py` to build N variants in
  one go, ship a `dlopen("libgemv_v{cfg_id}.so")` swap path in the APK.
- **Week 4–5** — Host-side `tuner_driver` that generates a config space,
  drives variant build + push, fans out broadcasts, collects JSONs.
- **Week 6** — JSON → C header codegen (CLBlast-style), shape-bucket
  lookup table.
- **Week 7–8** — Public C API, end-to-end LLaMA-decode benchmark Activity.
- **Week 9–12** — Tune the LLaMA shape buckets, accuracy hardening,
  documentation.

Targets per plan §319: ≥ 2× median speedup over QNN's stock MatMul
across 5 LLaMA-7B decode shapes; max relative error < 1e-2.

## Known limitations (Phase 1)

- **HVX path is `q_block=64` only.** Other block sizes fall through to the
  scalar fallback. Generalizing the lane partition is on the Week 3 list.
- **Single hardware thread.** `N_HW_THREADS=1` for now; v81 has up to 4
  HVX-capable contexts that we haven't tapped.
- **Horizontal sum is scalar.** 32× `Q6_R_vextract_VR` per HVX iter
  dominates the per-row time. Day-7 candidate optimization.
- **Test data is synthetic.** Seeded LCG; no real model activations yet.

## Key memos (lessons that bit us)

- Android 12+ blocks `startForegroundService()` from background broadcasts;
  the `BroadcastReceiver` does the work directly in `onReceive` instead.
- Hexagon SDK `setup_sdk_env.source` uses `${BASH_SOURCE[0]}`, which is
  empty in zsh — the `qblast-env` shell function `pushd`-wraps the source
  so `HEXAGON_SDK_ROOT` resolves correctly.
- HVX intrinsic gotchas (vextract takes byte offset not lane index;
  `vshuff_VVR` Rt semantics undocumented; `vshuffoe` lane assignment
  ambiguous) — workaround is to deinterleave on the host and avoid byte
  shuffle entirely.

# qblast

CLBlast-style auto-tuning library for the **Qualcomm Hexagon HTP** (NPU on
Snapdragon 8 Elite Gen 5 / SM8850 / Hexagon v81), targeting LLM decode-shape
GEMV.

The full design and 12-week plan live in
[`../qblast_plan.md`](../qblast_plan.md) — this README only covers what's
shipped, how to run it, and what's next.

## Status — Phase 1 weeks 1-8 complete

End-to-end pipeline working on a retail OnePlus 15 (CPH2749, Hexagon v81)
with **no root and no Qualcomm signing**. Tuner pipeline + database +
auto-dispatch all wired up. The host passes only a shape; the library
queries the offline-tuned C++ database and dispatches to the right
variant skel automatically.

The Day-7 baseline (HVX 9.76x over scalar-FP) on a single-cfg 256x256:

| variant                                  | dsp_cycles_med | cycles/MAC | rel_err  | speedup |
|------------------------------------------|---------------:|-----------:|---------:|--------:|
| scalar W4 + FP16 x, FP32 acc             |      1,578,246 |       24.1 |  4.3e-04 |   1.00x |
| scalar W4 + int8 x, int32 acc per block  |        560,510 |       8.55 |  3.2e-03 |   2.82x |
| HVX-int, 4 blocks/iter, deinterleaved x  |        161,724 |       2.47 |  3.2e-03 |   9.76x |

The Day-5 LLaMA-7B/13B decode-shape leaderboard (10 shapes, max-norm
rel-err metric, all ok at 2e-2 tolerance):

| LLaMA bucket    | shape (M_K_q)     | winner | cycles_med    |
|-----------------|-------------------|--------|--------------:|
| Q/K/V/O proj    | 4096_4096_64      | Q=64   |  67 M         |
| Q/K/V/O proj    | 4096_4096_128     | Q=128  |  59 M         |
| FFN up 7B       | 11008_4096_64     | Q=64   | 172 M         |
| FFN up 7B       | 11008_4096_128    | Q=128  | 162 M         |
| FFN down 7B     | 4096_11008_64     | Q=64   | 172 M         |
| FFN down 7B     | 4096_11008_128    | Q=128  | **116 M**     |
| FFN up 13B      | 14336_4096_64     | Q=64   | 227 M         |
| FFN up 13B      | 14336_4096_128    | Q=128  | 209 M         |
| LM head LLaMA   | 32000_4096_64     | **Q=64** | 402 M       |
| LM head LLaMA   | 32000_4096_128    | Q=128  | 543 M         |

Two non-obvious findings the auto-tuner surfaced:
- **Q=128 generally beats the LLaMA-default Q=64 by 5-18%** on cycles
  AND on numerical precision (fewer FP scale rounding events per row).
- **LM head q=64 inverts the rule** — at M=32000 (A_packed = 65 MB,
  beyond L2 cache) Q=64 wins. A static "always pick Q=128" library is
  35% slower here.

The runtime database routes correctly in either case; auto-dispatch
verified end-to-end.

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
│   ├── trigger_tune.sh                  am broadcast wrapper (--auto for DB lookup)
│   ├── pull_results.sh                  adb pull tuner JSON
│   ├── variant_builder.py               build all cfgs (per-tmp-dir copies)
│   ├── push_skels.sh                    adb push all variants from manifest
│   ├── tuner_driver.py                  end-to-end orchestration + leaderboard
│   └── database/
│       ├── cfgs/                        cfg JSON inputs (sample.json)
│       ├── database.py                  leaderboard.json -> sd8e_gen5.hpp codegen
│       └── json/                        per-cfg + leaderboard output
├── src/database/           Runtime tuner-database lookup (host C++17)
│   ├── database_structure.hpp           VariantParams + DatabaseEntry
│   ├── database.hpp                     inline lookup_variant()
│   ├── kernels/gemv_w4a16/
│   │   └── sd8e_gen5.hpp                AUTO-GENERATED from leaderboard.json
│   ├── test_database.cpp                15 lookup assertions
│   └── CMakeLists.txt                   header-only lib + test exe
├── cmake/                  Cross-compile toolchain files (Hexagon, populated later)
├── include/
│   └── qblast.h                         Public C++ API contract
├── bench/                  On-device benchmark code (populated later)
└── docs/
    ├── architecture.md                  System layers + data flow
    ├── tuning_space.md                  Cfg JSON schema + axis status
    └── device_setup.md                  Workstation + device prereqs
```

## Roadmap

| Phase 1 week | Status | What           |
|--------------|--------|----------------|
| 0–1   | ✅ | Environment, APK skeleton, broadcast plumbing       |
| 2     | ✅ | gemv_w4a16 baseline, HVX path, validator            |
| 3     | ✅ | Kernel parameterization + variant build pipeline    |
| 4–5   | ✅ | tuner_driver.py orchestration + LLaMA shape sweep   |
| 6     | ✅ | JSON → C header codegen + runtime lookup            |
| 7–8   | ✅ | Auto-dispatch via database; public API stub         |
| 9–10  | 🔲 | QNN MatMul comparison (plan §319 acceptance)        |
| 11    | 🔲 | Docs + robustness polish                            |
| 12    | 🔲 | Phase 2 scoping                                     |

**Phase 1 acceptance target** (plan §319): median speedup ≥ 2× over
QNN's stock MatMul on 5 LLaMA-7B decode shapes, with max-norm rel
error < 1e-2 (currently using 2e-2 to absorb int8-x quant noise).

## Known limitations (Phase 1)

- **HVX kernel TILE_M is fixed at 1.** TILE_M > 1 cfgs compile but the
  kernel falls through to scalar. Wiring TILE_M=2 into HVX is the next
  big perf win; ~2× expected on row-bound shapes.
- **Single hardware thread.** N_HW_THREADS=1 for now; v81 has 4
  HVX-capable contexts we haven't tapped. ~3-4× headroom.
- **No VTCM staging or DMA double-buffering.** Plan §253-261 lists them
  as future tuning axes; the current kernel reads weights directly from
  L2 via `vmemu`. Memory-bandwidth-bound shapes (large M) probably gain
  from VTCM tiling.
- **Tuner picks among 3 hand-authored cfgs.** Plan §369 random-sampling
  + constraint filter for ~200-cfg search lands when more axes are wired.
- **Test data is synthetic.** Seeded LCG; real-model activations differ
  in distribution but the int8 quant noise floor (~1.5%) is the same.
- **Single SoC.** Database hard-codes "sd8e_gen5". v75 / v79 backports
  need a second generated header + runtime SoC detection.

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

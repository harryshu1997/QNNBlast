# qblast architecture

## What it is

qblast is a CLBlast-style auto-tuning library targeting the **Qualcomm
Hexagon HTP** (NPU) on Snapdragon 8 Elite Gen 5 / SM8850 / Hexagon v81.
Phase 1 covers W4A16 GEMV at LLaMA decode shapes.

The library follows the CLBlast template: parameterized kernel sources,
host-driven auto-tuner, JSON results, codegen to a C++ database, runtime
lookup picks the tuned variant per shape. The deltas vs CLBlast are documented
in [`qblast_plan.md`](../../qblast_plan.md) §90-140 (database keys include
shape buckets, tuner is host/device split, accuracy validation is built-in).

## Three layers

The codebase has three distinct compile units, each with its own toolchain:

```text
host (Linux x86_64)             OnePlus 15 (Android 16)
───────────────────             ────────────────────────
                                Java APK (com.qblast.tuner)
scripts/ (Python + bash)            ↓ JNI
   variant_builder.py            libqblast_tuner_jni.so (NDK clang, arm64-v8a)
   tuner_driver.py                  ↓ FastRPC
   database/database.py             ↓ unsigned-PD on cDSP
   build_dsp.sh / push_skels.sh
                                libgemv_w4a16_v{N}_skel.so
                                (Hexagon clang 19, v81)
src/database/ (C++17 host)
   test_database.cpp
   sd8e_gen5.hpp (auto-gen)
```

| Layer    | Toolchain                              | Where it lives                              |
|----------|----------------------------------------|---------------------------------------------|
| DSP skel | Hexagon clang 19 + Hexagon SDK 6.5     | `src/kernels/hexagon/<kernel>/`             |
| JNI lib  | Android NDK r27c                       | `android/app/src/main/cpp/`                 |
| Database | host C++17 (g++/clang)                 | `src/database/`                             |
| Scripts  | Python 3.11+ / bash                    | `scripts/`                                  |

The DSP skel is built by the SDK Makefile chain, the JNI lib by Gradle/AGP
+ NDK CMake, the database by host CMake (or directly with `c++ -std=c++17`).

## Data flow at runtime

```text
host                                       device
────                                       ──────
adb shell am broadcast com.qblast.TUNE
       --ei cfg_id <N|-1> --es shape M_K_q
       (-1 = "auto-dispatch via database")
                                           TuneBroadcastReceiver.onReceive()
                                              └─ TunerService.nativeRunGemv()
                                                 ├─ if cfg_id < 0:
                                                 │     lookup_variant(M,K,q_block)
                                                 │     → DatabaseEntry{cfg_id, params}
                                                 ├─ rpcmem_alloc 4 ION buffers
                                                 ├─ generate seeded test data
                                                 ├─ ensure_gemv_handle(cfg_id)
                                                 │     → FastRPC opens
                                                 │       libgemv_w4a16_v{cfg_id}_skel.so
                                                 ├─ warmup × N iters timed runs
                                                 ├─ FP32 reference validator
                                                 │     → max_norm_rel
                                                 └─ write
                                                    /sdcard/Android/data/.../
                                                    cfgN_M_K_q.json
adb pull → leaderboard.json
```

The host script `tuner_driver.py` orchestrates this loop end-to-end across
multiple cfgs × shapes and produces a leaderboard.json that the codegen
step consumes.

## Build pipeline (offline tuning)

```text
scripts/database/cfgs/sample.json     ── list of cfgs to evaluate
        │  {"cfg_id": 0, "q_block": 64,  ...}
        │  {"cfg_id": 1, "q_block": 128, ...}
        │  {"cfg_id": 2, "q_block": 32,  ...}
        ▼
scripts/variant_builder.py
        ├─ tmp/qblast_v0/ ── qblast_variant_config.h with Q_BLOCK=64
        ├─ tmp/qblast_v1/ ── ...                       Q_BLOCK=128
        ├─ tmp/qblast_v2/ ── ...                       Q_BLOCK=32
        │  each → make tree → libgemv_w4a16_v{N}_skel.so
        ▼
scripts/database/variants/
        ├─ libgemv_w4a16_v0_skel.so
        ├─ libgemv_w4a16_v1_skel.so
        ├─ libgemv_w4a16_v2_skel.so
        └─ manifest.json
        ▼
scripts/push_skels.sh manifest.json
        → adb push to /data/local/tmp/ on the device
        ▼
scripts/tuner_driver.py
        → for each (cfg, shape): broadcast + wait for JSON
        ▼
scripts/database/json/
        ├─ cfg{N}_{M}_{K}_{q}.json     ── per-run telemetry
        └─ leaderboard.json            ── shape → winning cfg map
        ▼
scripts/database/database.py
        → reads leaderboard.json, picks winners
        ▼
src/database/kernels/gemv_w4a16/sd8e_gen5.hpp
        ── auto-generated constexpr DatabaseEntry kEntries[]
```

That last header is what `lookup_variant()` reads at runtime to do
auto-dispatch.

## The kernel itself (gemv_w4a16)

Source: [`src/kernels/hexagon/gemv_w4a16/gemv_w4a16_imp.c`](../src/kernels/hexagon/gemv_w4a16/gemv_w4a16_imp.c)
Source-level params via `qblast_variant_config.h` (overwritten per variant
build by `variant_builder.py`):

| Param                | Phase-1 values | Effect on code                            |
|----------------------|----------------|-------------------------------------------|
| QBLAST_Q_BLOCK       | {32, 64, 128}  | lane → block partition (varies tree depth) |
| QBLAST_TILE_M        | {1}            | rows produced per outer step (only 1 wired)|
| QBLAST_N_HW_THREADS  | {1}            | QuRT thread count (only 1 wired)          |

The HVX inner loop handles 256 K-elements per iteration regardless of
Q_BLOCK; only the lane → block partition + horizontal-sum count changes.
See [`tuning_space.md`](tuning_space.md) for the param-space rationale.

Algorithmic invariants (independent of params):

- x activation is host-quantized to int8 Q.7 + dequant `x_scale`.
- x_quant is host-deinterleaved: first K/2 bytes = x at even k, next K/2
  bytes = x at odd k. DSP avoids any byte shuffle.
- Output y is FP16; per-block accumulator is int32, converts to FP32 once
  at block boundary for the scale multiply.

Numerical noise floor: ~1.5-2% max-norm relative error (int8 x quant).
Bigger shapes (4096×4096) get under 3e-3 due to averaging.

## Library surface

[`include/qblast.h`](../include/qblast.h) declares the public API. Phase-1
the header is a contract; the standalone `libqblast.so` (ARM-side) lands
when the ABI freezes. Until then, callers go through the JNI's
`nativeRunGemv` which exercises the same auto-dispatch logic.

## What's not covered yet

- TILE_M > 1 and N_HW_THREADS > 1 are declared as variant-builder params
  but aren't wired into the HVX inner loop yet — those cfgs fall through
  to scalar.
- VTCM staging + DMA double-buffering aren't implemented (plan §253-261
  lists them as future tuning axes).
- Multi-SoC support: the database currently hard-codes `sd8e_gen5`.
  Adding v75 / v79 means a second generated header + a runtime SoC
  detection step.
- The standalone `libqblast.so` ARM library doesn't exist yet —
  callers integrate via the JNI layer.

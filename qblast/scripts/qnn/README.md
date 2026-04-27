# scripts/qnn/

QNN comparison toolchain artifacts. Phase-1 scope: prove access path,
collect QNN baseline data for plan §319 acceptance. **Status: partial.**
Access path confirmed; per-shape MatMul comparison blocked on a
qairt-converter bug.

## What works

- `run_qnn_smoke.sh` — pushes QAIRT 2.45's qnn-net-run + htp-v81 backend
  to `/data/local/tmp/qnn_test/` on the OnePlus 15 and runs the
  shipped InceptionV3 example with `--profiling_level basic`. **Confirmed
  end-to-end: QNN HTP runs from adb shell on retail unsigned PD without
  root and without APK wrapping.** This contradicts plan §40-43's
  assumption that `qnn-net-run` from adb shell wouldn't work on retail.

  Mechanism: `libQnnHtp.so` internally calls
  `remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE)` before
  `*_open`, the same path qblast's APK uses. plan §41 saw the failure
  *before* unsigned-PD enablement was understood.

- `qnn-profile-viewer` (host) parses the device-side
  `qnn-profiling-data_0.log` into a CSV with per-event timings:
  RPC time, accelerator (excluding wait) time, HVX-thread count.
  InceptionV3's first `convReluModel` graph clocks ~50 us steady-state
  on 8 HVX threads.

## What's blocked

- `build_matmul.py` writes a 2-D MatMul ONNX model
  (`y[1,M] = x[1,K] @ W[K,M]`, FP16) — that part works.
- `qairt-converter` Linux x86_64 wheel **requires Python 3.10**
  (libpython3.10.so.1.0). System ships 3.12; need a conda env at 3.10
  + `LD_LIBRARY_PATH=$CONDA_PREFIX/lib`.
- `qairt-converter` officially supports Ubuntu 22.04. Ubuntu 24.04
  (workstation) trips the dependency checker but the actual conversion
  proceeds past it.
- The conversion fails with an **internal IR layout-transform crash**
  on our 2-D MatMul:
  ```text
  ValueError: permute: IrTensorShape permute error: illegal order
              [791121512,23022] for shape 0x59ebb1bff210
  ```
  The crash is inside the optimizer's NHWC↔NCHW transpose insertion
  pass. Likely affected by our shape being (1,K) × (K,M) — not a
  spatial 4-D conv shape the optimizer expects. Worked around in QNN's
  hand-written examples (e.g. `qnn_model_8bit_quantized.cpp`) by
  authoring the QNN ModelOp graph in C++ directly.

## Path forward (when this resumes)

1. **Bypass qairt-converter.** Hand-write a `matmul_4096_4096.cpp` +
   `.bin` pair following the structure of QAIRT's example
   `examples/QNN/converter/models/qnn_model_8bit_quantized.cpp`.
   The ModelOp API is QnnModel_addNode + QnnModel_freeze. ~200 LoC
   per shape; the .bin holds quantization tables.
2. Or: file a Qualcomm forum post with the IR layout error + a
   minimal repro ONNX. Likely a known bug fixed in QAIRT 2.46+.
3. Or: install Ubuntu 22.04 in a container / use the QAIRT 2.31
   release we already have at `/opt/qcom/aistack/qairt/2.31.0.250130/`
   if it ships an older converter that doesn't trip the same path.
4. Or: skip QNN MatMul comparison and use scalar-fallback as the
   baseline (qblast HVX vs qblast scalar = ~3x consistent across
   shapes). Honest but doesn't satisfy plan §319 strict "vs QNN".

## Files in this directory

| file                | role                                              |
|---------------------|---------------------------------------------------|
| build_matmul.py     | ONNX MatMul model builder (works)                 |
| run_qnn_smoke.sh    | adb push + qnn-net-run on InceptionV3 example     |
| README.md           | this file                                         |

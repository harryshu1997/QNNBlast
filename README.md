# QNNBlast

Research repo for **qblast** — a CLBlast-style auto-tuning library for the
Qualcomm Hexagon HTP (NPU) on Snapdragon 8 Elite Gen 5 / SM8850 / Hexagon v81.
Phase 1 targets W4A16 GEMV at LLM-decode shapes; future phases extend to
INT8 GEMM, fused attention, and ggml/llama.cpp integration.

## Layout

- [qblast/](qblast/) — the library and its build/test infrastructure.
  See [qblast/README.md](qblast/README.md) for the project front page,
  current numbers, and quick-start commands.
- [qblast_plan.md](qblast_plan.md) — the 12-week master plan: project
  goals, target hardware, architecture decisions vs CLBlast, parameter
  space, week-by-week deliverables, known traps. The qblast README
  pulls just what you need to run something today; this document is the
  authority on *why*.

## Status

Phase 1 Week 2 complete: scalar baseline → int8-quantized x → HVX vrmpy
inner loop, 9.76× over the scalar-FP baseline, numerically equivalent to
scalar-int (rel_err 3.2e-03, plan tolerance is 1e-2). End-to-end pipeline
(host → broadcast → cDSP unsigned PD → JSON result) verified on a retail
OnePlus 15 with no root and no Qualcomm signing.

Acceptance criterion (plan §319) is ≥ 2× median speedup over QNN's stock
MatMul across 5 LLaMA-7B decode shapes; that benchmark gets wired up in
Week 7–8.

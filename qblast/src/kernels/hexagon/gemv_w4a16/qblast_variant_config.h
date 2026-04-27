// Per-variant tunable parameters for gemv_w4a16. The committed file holds
// the *default* baseline values (Q=64, T=1, N=1, equivalent to Week-2 Day-7).
// variant_builder.py overwrites this file (in a per-variant temp copy of the
// kernel source) with the cfg-specific values before each `make tree` run.
//
// Don't manually rely on stale per-variant edits to this file — if you want
// to test a non-default cfg, run variant_builder.py and let it manage the
// tmp directory.

#ifndef QBLAST_VARIANT_CONFIG_H
#define QBLAST_VARIANT_CONFIG_H

#define QBLAST_Q_BLOCK         64
#define QBLAST_TILE_M           1
#define QBLAST_N_HW_THREADS     1

#endif  // QBLAST_VARIANT_CONFIG_H

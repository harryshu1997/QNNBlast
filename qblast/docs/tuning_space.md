# Tuning space

What variant_builder.py can vary, what currently maps to a real code path,
and what's reserved for future work. Source of truth for the cfg JSON
schema.

## Cfg JSON schema

Every entry in `scripts/database/cfgs/*.json` is an object:

```json
{
  "cfg_id":        <int>,    required, identifies the variant; also the
                             dlopen URI suffix (libgemv_w4a16_v{cfg_id}_skel.so)
  "q_block":       <int>,    required, ∈ {32, 64, 128}
  "tile_m":        <int>,    optional, default 1
  "n_hw_threads":  <int>,    optional, default 1
}
```

variant_builder.py turns each into:

```c
// qblast_variant_config.h (per-variant tmp copy)
#define QBLAST_Q_BLOCK         <q_block>
#define QBLAST_TILE_M          <tile_m>
#define QBLAST_N_HW_THREADS    <n_hw_threads>
```

then runs `make tree` which builds `libgemv_w4a16_v{cfg_id}_skel.so`.

## Phase-1 status of each axis

| Param                | Status      | Notes                                              |
|----------------------|-------------|----------------------------------------------------|
| QBLAST_Q_BLOCK       | **active**  | All three values {32, 64, 128} have HVX inner loops |
| QBLAST_TILE_M        | declared    | Only TILE_M=1 has HVX impl; >1 falls to scalar     |
| QBLAST_N_HW_THREADS  | declared    | Only =1 wired; QuRT worker pool is Phase-2 work    |
| (DMA / VTCM / prefetch) | not present | Plan §253-261 future params, kernel doesn't use VTCM staging yet |

The HVX fast path activates iff:
- the variant's compile-time `QBLAST_Q_BLOCK` matches the runtime
  `q_block` arg
- runtime `K % 256 == 0` (each HVX iter processes 256 K-elements)
- TILE_M == 1 and N_HW_THREADS == 1

Otherwise the scalar fallback runs (correct, ~3× slower).

## Q_BLOCK lane partitioning

Each HVX iter does two `vrmpy_VubVb` ops + one `vadd_VwVw`, producing 32
int32 lanes where lane *i* contains `Σ_{k=8i..8i+7} W[k] * x_quant[k]`.
Q_BLOCK changes how those 32 lanes group into per-block sums:

| Q_BLOCK | LANES_PER_BLOCK | BLOCKS_PER_ITER | per-iter FP scale ops |
|---------|----------------:|----------------:|----------------------:|
| 32      |              4  |              8  |                    8  |
| 64      |              8  |              4  |                    4  |
| 128     |             16  |              2  |                    2  |

Larger Q_BLOCK → fewer blocks per iter → fewer FP scale-mul-add per iter.
The Day-3 sweep found Q=128 wins by 5-18% on most LLaMA decode shapes
because of this; see
[`memory/q128_wins_on_v81_hvx.md`](#) (private notes).

But Q_BLOCK is a property of the **quantized model**, not just a kernel
choice — a model quantized with block_size=64 cannot suddenly be served
with a Q=128 variant (the scales don't line up). So the tuner picks the
fastest variant *that matches the model's q_block*.

## Database key format

Currently exact-match on `"M_K_q"` decimal strings:

```text
"4096_4096_64"     → cfg 0  (Q=64)
"4096_4096_128"    → cfg 1  (Q=128)
"32000_4096_64"    → cfg 0  (Q=64)   // LM head, q=64 wins despite Q=128 elsewhere
"<unknown>"        → default cfg
```

`scripts/database/database.py` generates the table from
`leaderboard.json` (one entry per shape, picks lowest cycles_med among
`status=="ok"` rows).

Plan §283-292 describes shape *buckets* (decode_n1_attn, decode_n1_lm_head,
etc.) — the idea is that shapes within a bucket share parameters, so
"4096_4096_64" and a hypothetical "4096_4096_64 in a different model"
both look up the same entry. Phase-1 keeps it exact-match because all
LLaMA decode shapes are at concrete sizes; bucket-based lookup is Phase-2
when we want to handle "shapes the tuner hasn't seen but are similar to
known ones".

## Adding a new cfg

1. Append to `scripts/database/cfgs/sample.json`:
   ```json
   {"cfg_id": 3, "q_block": 64, "tile_m": 2, "n_hw_threads": 1}
   ```
2. Run `scripts/variant_builder.py --cfg-file ... --output-dir scripts/database/variants/`
3. `scripts/push_skels.sh scripts/database/variants/manifest.json`
4. Re-run sweeps via `scripts/tuner_driver.py`
5. Re-run `scripts/database/database.py` to regenerate the C++ table.
6. Rebuild + reinstall the APK.

Note: cfg_id 3 with `tile_m=2` will currently fall through to scalar
(no HVX impl yet) — this is fine for testing variant_builder mechanics
but won't show speedup. Wiring TILE_M=2 into the HVX inner loop is the
next big kernel work item.

## Adding a new tuning axis

Steps to introduce `QBLAST_VTCM_A_BYTES`:

1. Define the macro in `qblast_variant_config.h` (committed default value).
2. Reference in `gemv_w4a16_imp.c` to gate VTCM allocation logic.
3. Add to `cfg_to_defines()` in `scripts/variant_builder.py` if the JSON key
   doesn't naturally turn into `QBLAST_VTCM_A_BYTES` (it does, by
   uppercasing the key, so usually no change is needed).
4. Update this doc + the cfg JSON schema in `sample.json`.
5. (Optional) Update `VariantParams` in `src/database/database_structure.hpp`
   so the runtime knows the param even though variant_builder already
   stamped it into the per-variant SO.

The variant_builder's design is "any unknown JSON key → `QBLAST_<KEY_UPPER>`
define", so most new params are zero code change to add. The cost is
in the kernel + database struct.

## Constraints (when the cfg space gets bigger)

Plan §266-277 lists three constraints worth enforcing in
variant_builder.py before it tries to compile cfg combinations:

```python
# total VTCM budget
vtcm_a_bytes + vtcm_x_bytes <= TOTAL_VTCM_BUDGET   # ~8 MB on v81

# tile_k must be multiple of q_block
tile_k % q_block == 0

# HMX-style hard tile minima (HMX-specific, not yet used)
tile_m >= 8
```

Phase-1's tiny cfg space (3 entries) doesn't need filtering, but Week-12
work introduces Cartesian-product expansion + random sampling, at which
point constraint filters become essential to keep build time bounded.

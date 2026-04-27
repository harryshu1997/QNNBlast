// DSP-side W4A16 GEMV with int8-quantized x.
//
// Phase-1 Week-3 Day-1: parameterized via -D build flags so a single source
// produces multiple skel .so variants for the auto-tuner. The host
// variant_builder.py stamps a (cfg_id, params) pair into a per-variant
// build, calls `make tree EXTRA_CFLAGS=-DQBLAST_Q_BLOCK=..` for each, and
// names the output libgemv_w4a16_v{cfg_id}_skel.so.
//
// Tunable parameters (defaults reproduce Week-2 Day-7 behaviour):
//   QBLAST_Q_BLOCK       quantization block size along K. ∈ {32, 64, 128}.
//                        Each HVX iter still processes 256 K-elements; only
//                        the lane → block partition + tree-reduction depth
//                        change with this knob.
//   QBLAST_TILE_M        rows produced per outer-loop step. ∈ {1, 2, 4, 8}.
//                        Day-1 only the tile_m==1 path is HVX-vectorized;
//                        other values fall back to scalar (will be wired in
//                        Day-2). Declared so variant_builder cfgs can probe
//                        the param now.
//   QBLAST_N_HW_THREADS  HVX-capable QuRT threads. ∈ {1, 2, 4}. Same Day-1
//                        story as TILE_M — declared but only ==1 is fast.
//
// Algorithmic invariants (independent of params):
//   * x activation is host-quantized to int8 Q.7 + dequant `x_scale`.
//   * x_quant is host-deinterleaved: first K/2 bytes = x at even k, next
//     K/2 bytes = x at odd k. DSP avoids any byte shuffle.
//   * Output y is FP16; per-block accumulator stays int32 then converts to
//     FP32 once at block boundary for the scale multiply.

#include <stdlib.h>
#include "HAP_farf.h"
#include "HAP_perf.h"
#include "gemv_w4a16.h"
#include "../common/fp16_compat.h"

// Per-variant tunable parameters live in a generated header that
// variant_builder.py overwrites in a per-variant tmp source copy.
// The committed version of this header holds the default baseline values.
#include "qblast_variant_config.h"

#if defined(__HVX__) && (__HVX_LENGTH__ == 128)
#  include "hexagon_types.h"
#  include "hexagon_protos.h"
#  define QBLAST_HAVE_HVX 1
#else
#  define QBLAST_HAVE_HVX 0
#endif

#if QBLAST_Q_BLOCK != 32 && QBLAST_Q_BLOCK != 64 && QBLAST_Q_BLOCK != 128
#  error "QBLAST_Q_BLOCK must be 32, 64, or 128"
#endif

// One HVX iter processes 256 K-elements regardless of Q_BLOCK; the only
// difference is how many blocks fit and how deep the reduction tree is.
#define QBLAST_K_PER_ITER         256
#define QBLAST_LANES_PER_BLOCK    (QBLAST_Q_BLOCK / 8)
#define QBLAST_BLOCKS_PER_ITER    (QBLAST_K_PER_ITER / QBLAST_Q_BLOCK)

// HVX path is the fast path whenever we've implemented the inner loop in
// vector form. Day-1 limits this to TILE_M==1 + N_HW_THREADS==1; once those
// gain HVX implementations the gate widens.
//
// Earlier cfg=1/2 smokes (Q=128, Q=32) reported rel_err 51 / 0.137 and we
// briefly thought the HVX path was wrong for those Q values. Diagnosis
// after instrumentation showed the kernel was numerically correct
// (max_abs_diff ~3e-3, same as Q=64), but the per-element rel_err metric
// blew up on rows where random cancellation drove ref ≈ 0. Switching the
// validator to max-norm relative error fixed it; HVX is re-enabled for
// all Q ∈ {32, 64, 128} here.
#define QBLAST_HVX_FAST_PATH                                          \
    (QBLAST_HAVE_HVX && QBLAST_TILE_M == 1 && QBLAST_N_HW_THREADS == 1)

// ---------- Open / close -----------------------------------------------------

int gemv_w4a16_open(const char* uri, remote_handle64* handle) {
    void* tptr = malloc(1);
    if (!tptr) return -1;
    *handle = (remote_handle64)tptr;
    return 0;
}

int gemv_w4a16_close(remote_handle64 handle) {
    if (handle) free((void*)handle);
    return 0;
}

// ---------- Scalar fallback --------------------------------------------------

// Used when params are outside the HVX-implemented combinations, or when the
// runtime q_block / K disagree with QBLAST_Q_BLOCK.
static inline int scalar_block_dot_di(const unsigned char* row_packed,
                                      const signed char*   x_even,
                                      const signed char*   x_odd,
                                      unsigned int         k0,
                                      unsigned int         q_block_runtime)
{
    int acc = 0;
    for (unsigned int j = 0; j < q_block_runtime; ++j) {
        const unsigned int k = k0 + j;
        const unsigned char byte = row_packed[k >> 1];
        const unsigned int q = (k & 1u) ? ((unsigned int)byte >> 4)
                                        : ((unsigned int)byte & 0xFu);
        const signed char xv = (k & 1u) ? x_odd[k >> 1] : x_even[k >> 1];
        acc += (int)q * (int)xv;
    }
    return acc;
}

// ---------- HVX inner loop (per Q_BLOCK) -------------------------------------

#if QBLAST_HVX_FAST_PATH

// One HVX iter: process 256 K-elements; produce QBLAST_BLOCKS_PER_ITER int32
// block sums in `out[]`. Same shape regardless of Q_BLOCK; only the tree
// depth + extract offsets change.
static inline void hvx_inner_iter(const unsigned char* row_packed,
                                  const signed char*   x_even,
                                  const signed char*   x_odd,
                                  unsigned int         k0,
                                  int                  out[QBLAST_BLOCKS_PER_ITER])
{
    HVX_Vector packed = *(const HVX_UVector*)(row_packed + (k0 >> 1));
    HVX_Vector mask_0f = Q6_V_vsplat_R(0x0f0f0f0f);
    HVX_Vector v_lo    = Q6_V_vand_VV(packed, mask_0f);
    HVX_Vector v_hi    = Q6_Vub_vlsr_VubR(packed, 4);

    HVX_Vector xe = *(const HVX_UVector*)(x_even + (k0 >> 1));
    HVX_Vector xo = *(const HVX_UVector*)(x_odd  + (k0 >> 1));

    HVX_Vector dot_e = Q6_Vw_vrmpy_VubVb(v_lo, xe);
    HVX_Vector dot_o = Q6_Vw_vrmpy_VubVb(v_hi, xo);
    HVX_Vector dot   = Q6_Vw_vadd_VwVw(dot_e, dot_o);
    // Lane i = sum_{k=8i..8i+7} W[k] * x[k]. There are always 32 lanes,
    // partitioned into BLOCKS_PER_ITER groups of LANES_PER_BLOCK lanes each.
    //
    // Horizontal sum per block via direct scalar extracts. A valign-based
    // tree reduction was tried in Day-7 (I) for Q=64 (cycles unchanged) and
    // Day-8 for the parameterised path (Q=32/128 numerically wrong with
    // boundary conditions the SDK protos don't pin down). Direct extract is
    // provably correct for any Q_BLOCK and the cost (~32 vextracts + 31
    // scalar adds = ~96 cycles) is small relative to the per-row inner loop.
    for (int b = 0; b < QBLAST_BLOCKS_PER_ITER; ++b) {
        int sum = 0;
        for (int l = 0; l < QBLAST_LANES_PER_BLOCK; ++l) {
            const int lane_idx = b * QBLAST_LANES_PER_BLOCK + l;
            sum += Q6_R_vextract_VR(dot, lane_idx * 4);
        }
        out[b] = sum;
    }
}

#endif  // QBLAST_HVX_FAST_PATH

// ---------- Public entry point -----------------------------------------------

int gemv_w4a16_compute(
    remote_handle64 h,
    unsigned int M, unsigned int K, unsigned int q_block,
    const unsigned char* A_packed, int A_packedLen,
    const short* A_scales, int A_scalesLen,
    const unsigned char* x_quant, int x_quantLen,
    float x_scale,
    short* y, int yLen,
    uint64* dsp_cycles)
{
    if (q_block == 0 || (K % q_block) != 0) return -2;
    if (A_packedLen != (int)((unsigned long long)M * K / 2)) return -3;
    if (A_scalesLen != (int)((unsigned long long)M * (K / q_block))) return -4;
    if (x_quantLen != (int)K) return -5;
    if (yLen != (int)M) return -6;

    const unsigned int blocks_per_row = K / q_block;
    const uint64 t0 = HAP_perf_get_pcycles();

    const signed char* x_even = (const signed char*)x_quant;
    const signed char* x_odd  = (const signed char*)x_quant + (K / 2);

    // Whether HVX fired for this call — used only for the FARF tag so we can
    // tell which path executed from the device log. Declared inside the
    // `#if QBLAST_HVX_FAST_PATH` block so -Wunused-variable doesn't fire on
    // variants where the HVX path is compiled out entirely (e.g. Q!=64).
    int hvx_taken = 0;
    (void)hvx_taken;  // suppress -Wunused-variable on pure-scalar variants

    for (unsigned int m = 0; m < M; ++m) {
        float row_acc = 0.0f;
        const unsigned char* row = A_packed + ((unsigned long long)m * K) / 2;
        const short* row_scales = A_scales + (unsigned long long)m * blocks_per_row;

#if QBLAST_HVX_FAST_PATH
        // HVX path lights up only when the runtime q_block matches what this
        // variant was compiled for AND K is a whole number of HVX iters.
        if (q_block == QBLAST_Q_BLOCK && (K % QBLAST_K_PER_ITER) == 0) {
            hvx_taken = 1;
            for (unsigned int b = 0; b < blocks_per_row; b += QBLAST_BLOCKS_PER_ITER) {
                int blocks[QBLAST_BLOCKS_PER_ITER];
                hvx_inner_iter(row, x_even, x_odd, b * q_block, blocks);
                for (int j = 0; j < QBLAST_BLOCKS_PER_ITER; ++j) {
                    float scale = qblast_fp16_to_fp32(row_scales[b + j]);
                    row_acc += (float)blocks[j] * scale * x_scale;
                }
            }
        } else
#endif
        {
            for (unsigned int b = 0; b < blocks_per_row; ++b) {
                float scale = qblast_fp16_to_fp32(row_scales[b]);
                int block_acc = scalar_block_dot_di(row, x_even, x_odd,
                                                    b * q_block, q_block);
                row_acc += (float)block_acc * scale * x_scale;
            }
        }

        y[m] = qblast_fp32_to_fp16(row_acc);
    }

    *dsp_cycles = HAP_perf_get_pcycles() - t0;
    FARF(HIGH, "gemv_w4a16 (q8x %s di Q=%d T=%d N=%d): M=%u K=%u q=%u pcycles=%llu",
         hvx_taken ? "HVX" : "scalar",
         QBLAST_Q_BLOCK, QBLAST_TILE_M, QBLAST_N_HW_THREADS,
         M, K, q_block, (unsigned long long)*dsp_cycles);
    return 0;
}

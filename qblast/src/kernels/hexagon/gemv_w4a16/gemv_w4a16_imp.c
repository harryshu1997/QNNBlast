// DSP-side W4A16 GEMV with int8-quantized x.
//
// Phase-1 Day-6 (β-step-2): HVX inner loop, no byte-shuffle path.
// The host pre-deinterleaves x: x_quant[0..K/2-1] = x at even k, x_quant[K/2..K-1] = x at odd k.
// This lets us do TWO vrmpy ops per HVX iter (one for even k, one for odd k) without
// any byte-shuffle on the DSP — the W4 unpack lanes line up directly with x.
//
// Per HVX iter covers 256 K-elements (= 4 q_blocks of 64):
//   - load 128 packed bytes -> 256 W4 quants
//   - v_lo = packed & 0x0F  : 128 lanes, each = W at even k
//   - v_hi = packed >> 4    : 128 lanes, each = W at odd k
//   - load 128 bytes x_even, 128 bytes x_odd
//   - vrmpy(v_lo, x_even) + vrmpy(v_hi, x_odd) = 32 int32 lanes,
//     each lane summing 8 consecutive k-products
//   - 8 lanes per q_block: lanes 0..7 -> block 0, 8..15 -> block 1,
//     16..23 -> block 2, 24..31 -> block 3
//
// Falls back to scalar for q_block != 64 or K not a multiple of 256.

#include <stdlib.h>
#include "HAP_farf.h"
#include "HAP_perf.h"
#include "gemv_w4a16.h"
#include "../common/fp16_compat.h"

#if defined(__HVX__) && (__HVX_LENGTH__ == 128)
#  include "hexagon_types.h"
#  include "hexagon_protos.h"
#  define QBLAST_HAVE_HVX 1
#else
#  define QBLAST_HAVE_HVX 0
#endif

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

// Scalar fallback: takes the deinterleaved x layout (x_even = first K/2 bytes,
// x_odd = next K/2 bytes). Used when q_block != 64 or K % 256 != 0.
static inline int scalar_block_dot_di(const unsigned char* row_packed,
                                      const signed char*   x_even,
                                      const signed char*   x_odd,
                                      unsigned int         k0,
                                      unsigned int         q_block)
{
    int acc = 0;
    for (unsigned int j = 0; j < q_block; ++j) {
        const unsigned int k = k0 + j;
        const unsigned char byte = row_packed[k >> 1];
        const unsigned int q = (k & 1u) ? ((unsigned int)byte >> 4)
                                        : ((unsigned int)byte & 0xFu);
        const signed char xv = (k & 1u) ? x_odd[k >> 1] : x_even[k >> 1];
        acc += (int)q * (int)xv;
    }
    return acc;
}

#if QBLAST_HAVE_HVX
// One HVX iter: process 256 K-elements (4 q_blocks of 64), produce 4 int32 sums.
static inline void hvx_4block_q64(const unsigned char* row_packed,
                                  const signed char*   x_even,
                                  const signed char*   x_odd,
                                  unsigned int         k0,
                                  int                  out[4])
{
    HVX_Vector packed = *(const HVX_UVector*)(row_packed + (k0 >> 1));
    HVX_Vector mask_0f = Q6_V_vsplat_R(0x0f0f0f0f);
    HVX_Vector v_lo    = Q6_V_vand_VV(packed, mask_0f);
    HVX_Vector v_hi    = Q6_Vub_vlsr_VubR(packed, 4);

    // x_even and x_odd are deinterleaved by the host; both are contiguous
    // 128-byte runs starting at byte offset (k0 >> 1) of their respective halves.
    HVX_Vector xe = *(const HVX_UVector*)(x_even + (k0 >> 1));
    HVX_Vector xo = *(const HVX_UVector*)(x_odd  + (k0 >> 1));

    HVX_Vector dot_e = Q6_Vw_vrmpy_VubVb(v_lo, xe);  // lane i = sum_{j=0..3} W[2(4i+j)]   * x[2(4i+j)]
    HVX_Vector dot_o = Q6_Vw_vrmpy_VubVb(v_hi, xo);  // lane i = sum_{j=0..3} W[2(4i+j)+1] * x[2(4i+j)+1]
    HVX_Vector dot   = Q6_Vw_vadd_VwVw(dot_e, dot_o);
    // After add, lane i = sum_{k=8i..8i+7} W[k] * x[k].
    // Lanes 0..7 -> block 0, 8..15 -> block 1, 16..23 -> block 2, 24..31 -> block 3.

    int b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    for (int i =  0; i <  8; ++i) b0 += Q6_R_vextract_VR(dot, i * 4);
    for (int i =  8; i < 16; ++i) b1 += Q6_R_vextract_VR(dot, i * 4);
    for (int i = 16; i < 24; ++i) b2 += Q6_R_vextract_VR(dot, i * 4);
    for (int i = 24; i < 32; ++i) b3 += Q6_R_vextract_VR(dot, i * 4);
    out[0] = b0; out[1] = b1; out[2] = b2; out[3] = b3;
}
#endif  // QBLAST_HAVE_HVX

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

    // x_quant is deinterleaved by the host: first K/2 bytes are x at even k,
    // next K/2 bytes are x at odd k.
    const signed char* x_even = (const signed char*)x_quant;
    const signed char* x_odd  = (const signed char*)x_quant + (K / 2);

#if QBLAST_HAVE_HVX
    const int hvx_path = (q_block == 64 && (K % 256) == 0);
#else
    const int hvx_path = 0;
#endif

    for (unsigned int m = 0; m < M; ++m) {
        float row_acc = 0.0f;
        const unsigned char* row = A_packed + ((unsigned long long)m * K) / 2;
        const short* row_scales = A_scales + (unsigned long long)m * blocks_per_row;

#if QBLAST_HAVE_HVX
        if (hvx_path) {
            // 4 blocks per HVX iter; iters per row = K / 256.
            for (unsigned int b = 0; b < blocks_per_row; b += 4) {
                int blocks[4];
                hvx_4block_q64(row, x_even, x_odd, b * q_block, blocks);
                for (unsigned int j = 0; j < 4; ++j) {
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
    FARF(HIGH, "gemv_w4a16 (q8x %s di): M=%u K=%u q=%u pcycles=%llu",
         hvx_path ? "HVX" : "scalar",
         M, K, q_block, (unsigned long long)*dsp_cycles);
    return 0;
}

// DSP-side scalar baseline for W4A16 GEMV with int8-quantized x activation.
//
// Phase-1 Day-5 (β-step-1): correctness focus, no HVX yet. The next iteration
// (Day-6) replaces the per-block dot-product loop with HVX vmpybus + reduce.
//
// Per-block accumulator pattern (key for the int path):
//   for each row m:
//     row_acc_fp32 = 0
//     for each block b of q_block weights along K:
//       block_acc_int32 = sum_{k in block} W4[m,k] (uint4) * x_quant[k] (int8)
//                       — max single product 15 * 127 = 1905
//                       — max block sum (q_block=64) ≈ 122K, fits in int32
//       row_acc_fp32 += block_acc_int32 * scale_fp32 * x_scale
//     y[m] = fp32_to_fp16(row_acc_fp32)
//
// The block boundary is where we leave int and apply the FP scale, so the
// inner loop stays integer-only — that's what HVX vrmpybus consumes.

#include <stdlib.h>
#include "HAP_farf.h"
#include "HAP_perf.h"
#include "gemv_w4a16.h"
#include "../common/fp16_compat.h"

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

// Signature must match qaic-generated gemv_w4a16.h exactly (no AEE typedef
// substitutions — Hexagon clang in -Werror mode treats `uint32` vs
// `unsigned int` as distinct types even though they alias).
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
    const signed char* x_signed = (const signed char*)x_quant;

    for (unsigned int m = 0; m < M; ++m) {
        float row_acc = 0.0f;
        const unsigned char* row = A_packed + ((unsigned long long)m * K) / 2;
        const short* row_scales = A_scales + (unsigned long long)m * blocks_per_row;

        for (unsigned int b = 0; b < blocks_per_row; ++b) {
            const float scale = qblast_fp16_to_fp32(row_scales[b]);
            const unsigned int k0 = b * q_block;

            // Pure int per-block dot product. HVX vmpybus replaces this loop in Day 6.
            int block_acc = 0;
            for (unsigned int j = 0; j < q_block; ++j) {
                const unsigned int k = k0 + j;
                const unsigned char byte = row[k >> 1];
                const unsigned int q = (k & 1u) ? ((unsigned int)byte >> 4)
                                                : ((unsigned int)byte & 0xFu);
                block_acc += (int)q * (int)x_signed[k];
            }
            row_acc += (float)block_acc * scale * x_scale;
        }

        y[m] = qblast_fp32_to_fp16(row_acc);
    }

    *dsp_cycles = HAP_perf_get_pcycles() - t0;
    FARF(HIGH, "gemv_w4a16 (q8x): M=%u K=%u q=%u pcycles=%llu",
         M, K, q_block, (unsigned long long)*dsp_cycles);
    return 0;
}

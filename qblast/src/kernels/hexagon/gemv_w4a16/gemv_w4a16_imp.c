// DSP-side scalar baseline for W4A16 GEMV.
//
// Phase-1 Day-2 priority: correctness, not speed. Pure scalar loop, no HVX,
// no VTCM staging, no DMA, no threading. Day 4+ replaces the inner loop with
// HVX vmpy/vrmpy and adds VTCM tile staging once the validator is happy.
//
// Quantization scheme (matches qblast_fp16_compat.h on the host side):
//   A is uint4 packed two-per-byte; low nibble = even k, high nibble = odd k.
//   A_scales is FP16 (stored as int16 wire format), one scale per quant block
//   of q_block weights along K. blocks_per_row = K / q_block.
//   Dequantized weight: float w = scale * (float)q_uint4   (q in [0..15]).
//   Per-row dot product: y[m] = sum_k w[m,k] * x[k], in FP32 accumulator,
//   then rounded to FP16 for output.

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
    const short* x, int xLen,
    short* y, int yLen,
    uint64* dsp_cycles)
{
    // Layout sanity. Returning a negative error keeps the host-side log
    // helpful when somebody changes a buffer size and forgets a multiplier.
    if (q_block == 0 || (K % q_block) != 0) return -2;
    if (A_packedLen != (int)((unsigned long long)M * K / 2)) return -3;
    if (A_scalesLen != (int)((unsigned long long)M * (K / q_block))) return -4;
    if (xLen != (int)K) return -5;
    if (yLen != (int)M) return -6;

    const unsigned int blocks_per_row = K / q_block;
    const uint64 t0 = HAP_perf_get_pcycles();

    for (unsigned int m = 0; m < M; ++m) {
        float acc = 0.0f;
        const unsigned char* row = A_packed + ((unsigned long long)m * K) / 2;
        const short* row_scales = A_scales + (unsigned long long)m * blocks_per_row;

        for (unsigned int b = 0; b < blocks_per_row; ++b) {
            const float scale = qblast_fp16_to_fp32(row_scales[b]);
            const unsigned int k0 = b * q_block;
            for (unsigned int j = 0; j < q_block; ++j) {
                const unsigned int k = k0 + j;
                const unsigned char byte = row[k >> 1];
                const unsigned int q = (k & 1u) ? ((unsigned int)byte >> 4) : ((unsigned int)byte & 0xFu);
                const float wq = scale * (float)q;
                const float xv = qblast_fp16_to_fp32(x[k]);
                acc += wq * xv;
            }
        }
        y[m] = qblast_fp32_to_fp16(acc);
    }

    *dsp_cycles = HAP_perf_get_pcycles() - t0;
    FARF(HIGH, "gemv_w4a16: M=%u K=%u q_block=%u pcycles=%llu",
         M, K, q_block, (unsigned long long)*dsp_cycles);
    return 0;
}

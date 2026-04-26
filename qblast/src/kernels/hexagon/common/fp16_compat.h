// IEEE-754 binary16 <-> binary32 conversion via bit manipulation.
// Used by both the DSP skel (no HVX, baseline scalar) and the host-side ARM
// validator. Pure C, no compiler intrinsics — the optimized HVX path will
// supersede this once we add Day 4+ kernel work.

#ifndef QBLAST_FP16_COMPAT_H
#define QBLAST_FP16_COMPAT_H

#include <stdint.h>
#include <string.h>

static inline float qblast_fp16_to_fp32(int16_t h_bits) {
    uint16_t h = (uint16_t)h_bits;
    uint32_t sign = ((uint32_t)(h & 0x8000u)) << 16;
    uint32_t exp_h = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t f_bits;

    if (exp_h == 0) {
        if (mant == 0) {
            f_bits = sign;  // signed zero
        } else {
            // subnormal -> normalize
            int32_t e = -1;
            do { e++; mant <<= 1; } while (!(mant & 0x400));
            mant &= 0x3FFu;
            f_bits = sign | (((uint32_t)(127 - 15 - e)) << 23) | (mant << 13);
        }
    } else if (exp_h == 0x1F) {
        f_bits = sign | 0x7F800000u | (mant << 13);  // inf or NaN
    } else {
        f_bits = sign | (((exp_h + 127 - 15)) << 23) | (mant << 13);
    }

    float result;
    memcpy(&result, &f_bits, sizeof(result));
    return result;
}

static inline int16_t qblast_fp32_to_fp16(float f) {
    uint32_t f_bits;
    memcpy(&f_bits, &f, sizeof(f_bits));
    uint32_t sign = (f_bits >> 16) & 0x8000u;
    int32_t exp_f = (int32_t)((f_bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = f_bits & 0x7FFFFFu;

    if (exp_f >= 0x1F) {
        // overflow -> +/- inf, NaN preserved as +inf (we don't generate NaNs in test data)
        return (int16_t)(sign | 0x7C00u);
    }
    if (exp_f <= 0) {
        // underflow -> flush to signed zero (subnormal handling deferred until needed)
        return (int16_t)sign;
    }

    // round-to-nearest-even on the lower 13 bits of mantissa
    uint32_t mant_lo = mant & 0x1FFFu;
    uint32_t mant_hi = mant >> 13;
    if (mant_lo > 0x1000u || (mant_lo == 0x1000u && (mant_hi & 1))) {
        mant_hi += 1;
        if (mant_hi == 0x400u) {  // mantissa overflow
            mant_hi = 0;
            exp_f += 1;
            if (exp_f >= 0x1F) return (int16_t)(sign | 0x7C00u);
        }
    }
    return (int16_t)(sign | ((uint32_t)exp_f << 10) | mant_hi);
}

#endif  // QBLAST_FP16_COMPAT_H

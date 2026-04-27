// qblast public C++ API.
//
// Phase-1 status: header is the stable interface contract; the ARM-side
// implementation (libqblast.so) lands in Week 8+. The current APK uses the
// JNI-internal nativeRunGemv path with database auto-dispatch (cfg_id<0)
// to demonstrate the same selection logic this header will provide.
//
// Modeled loosely on CLBlast::Gemv signature so a future ggml-hexagon
// integration can swap backends with a #ifdef. Plan §437-438.

#ifndef QBLAST_QBLAST_H
#define QBLAST_QBLAST_H

#include <cstdint>
#include <cstddef>

namespace qblast {

// W4A16 GEMV inputs. Buffers must be ION-allocated via rpcmem so FastRPC can
// share them with cDSP without bouncing through a copy buffer.
//
// Layout details:
//   A_packed   M*K/2 bytes, row-major. Each byte holds two W4 quants:
//              low nibble = W[m, 2i], high nibble = W[m, 2i+1].
//   A_scales   M * (K/q_block) FP16 scales (stored as int16). One scale per
//              quantization block of q_block weights along K.
//   x_quant    K bytes, host-deinterleaved: [x_even || x_odd]. Even-indexed
//              k values fill x_quant[0..K/2-1], odd-indexed fill K/2..K-1.
//   x_scale    Dequant factor; full-precision x ≈ (int8)x_quant[k] * x_scale.
//   y          M FP16 outputs (int16 wire format).
struct GemvW4A16Args {
    unsigned int        M;
    unsigned int        K;
    unsigned int        q_block;
    const unsigned char* A_packed;
    const short*        A_scales;
    const unsigned char* x_quant;
    float               x_scale;
    short*              y;
};

// Result codes.
enum class Status : int {
    kOk             = 0,
    kBadArgs        = -1,
    kRpcSetupFail   = -2,
    kRpcOpenFail    = -3,
    kComputeFail    = -4,
    kAccuracyFail   = -5,
};

// Picks the tuned variant for this (M, K, q_block) via the offline database
// (src/database/kernels/gemv_w4a16/sd8e_gen5.hpp), opens the FastRPC handle
// if needed, and dispatches. Falls back to the default cfg for unknown shapes.
//
// Phase-1 declaration only — implementation lives in the JNI today; the
// standalone libqblast.so wiring lands when the public ABI freezes.
Status gemv_w4a16(const GemvW4A16Args& args);

}  // namespace qblast

#endif  // QBLAST_QBLAST_H

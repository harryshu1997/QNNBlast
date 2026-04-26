// JNI bridge for the qblast tuner APK.
//
// Phase-1 contains two RPC entry points:
//   nativePing()      — qblast_hello roundtrip; validates the FastRPC plumbing.
//   nativeRunGemv()   — gemv_w4a16 baseline; allocates rpcmem buffers, fills
//                       with seeded test data, runs on cDSP, validates
//                       against a host-side FP32 reference matmul.
//
// All metrics (DSP cycles, ARM wall-clock, max relative error) are emitted
// via __android_log_print so the host tuner driver can adb-pull them later.
// Java callers see only int return codes (0 = ok, < 0 = error).

#include <jni.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <chrono>

#include <stddef/AEEStdErr.h>
#include "rpcmem.h"
#include "remote.h"

#include "qblast_hello.h"  // qaic-generated
#include "gemv_w4a16.h"    // qaic-generated
#include "common/fp16_compat.h"

namespace {
constexpr const char* kTag = "qblast_jni";
}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_qblast_tuner_TunerService_nativeInit(JNIEnv* env, jclass /*cls*/, jstring location) {
    const char* path = env->GetStringUTFChars(location, nullptr);
    if (path == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "nativeInit: null location");
        return;
    }
    if (setenv("DSP_LIBRARY_PATH", path, 1) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "setenv DSP_LIBRARY_PATH=%s failed", path);
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "DSP_LIBRARY_PATH=%s", path);
    }
    env->ReleaseStringUTFChars(location, path);
}

namespace {

// Enables unsigned-PD on cDSP. Must be called once per process before any
// fastrpc _open. Returns 0 on success, < 0 on failure.
int enable_unsigned_pd_cdsp() {
    struct remote_rpc_control_unsigned_module ctrl;
    ctrl.enable = 1;
    ctrl.domain = CDSP_DOMAIN_ID;
    int err = remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE, &ctrl, sizeof(ctrl));
    if (err) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                "remote_session_control unsigned-PD failed: 0x%x", err);
        return -1;
    }
    return 0;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_qblast_tuner_TunerService_nativePing(JNIEnv* /*env*/, jclass /*cls*/) {
    if (enable_unsigned_pd_cdsp() != 0) return -1;

    remote_handle64 handle = 0;
    int err = 0;
    int64 magic = -1;
    uint64 dsp_cycles = 0;

    char uri[256];
    snprintf(uri, sizeof(uri), "%s%s", qblast_hello_URI, "&_dom=cdsp");

    auto t0 = std::chrono::steady_clock::now();

    err = qblast_hello_open(uri, &handle);
    if (err) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "qblast_hello_open failed: 0x%x", err);
        return -1;
    }

    err = qblast_hello_ping(handle, &magic, &dsp_cycles);
    if (err) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "qblast_hello_ping failed: 0x%x", err);
        magic = -1;
    }

    auto t1 = std::chrono::steady_clock::now();
    auto rtt_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    int close_err = qblast_hello_close(handle);
    if (close_err) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                "qblast_hello_close failed: 0x%x", close_err);
    }

    __android_log_print(ANDROID_LOG_INFO, kTag,
            "ping: magic=%lld dsp_cycles=%llu arm_rtt_us=%lld",
            (long long)magic, (unsigned long long)dsp_cycles, (long long)rtt_us);
    return (jlong)magic;
}

namespace {

// Linear-congruential PRNG. Same constants as numerical-recipes — fine for
// deterministic test-data generation; we don't care about distribution quality.
struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
};

// Returns max relative error between DSP output y[] and an FP32 reference
// matmul over the same A_packed / A_scales / x. Inputs and outputs are FP16
// (stored as int16 wire format).
double validate_gemv(uint32_t M, uint32_t K, uint32_t q_block,
                     const uint8_t* A_packed,
                     const int16_t* A_scales,
                     const int16_t* x,
                     const int16_t* y) {
    const uint32_t blocks_per_row = K / q_block;
    double max_rel = 0.0;
    for (uint32_t m = 0; m < M; ++m) {
        const uint8_t* row = A_packed + (size_t)m * K / 2;
        const int16_t* row_scales = A_scales + (size_t)m * blocks_per_row;
        double acc = 0.0;
        for (uint32_t b = 0; b < blocks_per_row; ++b) {
            float scale = qblast_fp16_to_fp32(row_scales[b]);
            uint32_t k0 = b * q_block;
            for (uint32_t j = 0; j < q_block; ++j) {
                uint32_t k = k0 + j;
                uint8_t byte = row[k >> 1];
                uint32_t q = (k & 1u) ? ((uint32_t)byte >> 4) : ((uint32_t)byte & 0xFu);
                float xv = qblast_fp16_to_fp32(x[k]);
                acc += (double)scale * (double)q * (double)xv;
            }
        }
        double dsp_y = (double)qblast_fp16_to_fp32(y[m]);
        double abs_diff = fabs(dsp_y - acc);
        double abs_ref = fabs(acc);
        double rel = abs_ref > 1e-6 ? abs_diff / abs_ref : abs_diff;
        if (rel > max_rel) max_rel = rel;
    }
    return max_rel;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_qblast_tuner_TunerService_nativeRunGemv(
        JNIEnv* /*env*/, jclass /*cls*/,
        jint jM, jint jK, jint jQBlock, jint jSeed) {
    const uint32_t M = (uint32_t)jM;
    const uint32_t K = (uint32_t)jK;
    const uint32_t q_block = (uint32_t)jQBlock;
    const uint32_t seed = (uint32_t)jSeed;

    if (M == 0 || K == 0 || q_block == 0 || (K % q_block) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                "nativeRunGemv: bad args M=%u K=%u q=%u", M, K, q_block);
        return -1;
    }

    if (enable_unsigned_pd_cdsp() != 0) return -2;

    const size_t a_packed_bytes = (size_t)M * K / 2;
    const size_t a_scales_bytes = (size_t)M * (K / q_block) * sizeof(int16_t);
    const size_t x_bytes = (size_t)K * sizeof(int16_t);
    const size_t y_bytes = (size_t)M * sizeof(int16_t);

    uint8_t* A_packed = (uint8_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, a_packed_bytes);
    int16_t* A_scales = (int16_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, a_scales_bytes);
    int16_t* x_buf = (int16_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, x_bytes);
    int16_t* y_buf = (int16_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, y_bytes);
    if (!A_packed || !A_scales || !x_buf || !y_buf) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "rpcmem_alloc failed");
        if (A_packed) rpcmem_free(A_packed);
        if (A_scales) rpcmem_free(A_scales);
        if (x_buf) rpcmem_free(x_buf);
        if (y_buf) rpcmem_free(y_buf);
        return -3;
    }

    // Seeded test data. Scales bounded to keep accumulators in FP16 range
    // (M*K accumulations of scale * q_max * x_max ~ 4096 * 0.05 * 15 * 1 ≈ 3000).
    Lcg rng(seed);
    for (size_t i = 0; i < a_packed_bytes; ++i) {
        A_packed[i] = (uint8_t)(rng.next() & 0xFFu);
    }
    const size_t n_scales = a_scales_bytes / sizeof(int16_t);
    for (size_t i = 0; i < n_scales; ++i) {
        // 0.001 .. 0.005 — keeps per-row dot product in [-300, 300] FP16 range
        float val = 0.001f + 0.004f * (float)(rng.next() & 0xFFu) / 255.0f;
        A_scales[i] = qblast_fp32_to_fp16(val);
    }
    for (size_t i = 0; i < K; ++i) {
        // x in [-1, 1]
        float val = (float)((int32_t)(rng.next() & 0xFFFFu) - 32768) / 32768.0f;
        x_buf[i] = qblast_fp32_to_fp16(val);
    }
    memset(y_buf, 0, y_bytes);

    // Open + compute + close.
    remote_handle64 handle = 0;
    char uri[256];
    snprintf(uri, sizeof(uri), "%s%s", gemv_w4a16_URI, "&_dom=cdsp");

    int err = gemv_w4a16_open(uri, &handle);
    if (err) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "gemv_w4a16_open failed: 0x%x", err);
        rpcmem_free(A_packed); rpcmem_free(A_scales); rpcmem_free(x_buf); rpcmem_free(y_buf);
        return -4;
    }

    uint64 dsp_cycles = 0;

    auto t0 = std::chrono::steady_clock::now();
    err = gemv_w4a16_compute(
            handle, M, K, q_block,
            A_packed, (int)a_packed_bytes,
            A_scales, (int)n_scales,
            x_buf, (int)K,
            y_buf, (int)M,
            &dsp_cycles);
    auto t1 = std::chrono::steady_clock::now();
    auto compute_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    int close_err = gemv_w4a16_close(handle);
    if (close_err) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "gemv_w4a16_close failed: 0x%x", close_err);
    }

    if (err != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "gemv_w4a16_compute failed: %d", err);
        rpcmem_free(A_packed); rpcmem_free(A_scales); rpcmem_free(x_buf); rpcmem_free(y_buf);
        return -5;
    }

    double max_rel = validate_gemv(M, K, q_block, A_packed, A_scales, x_buf, y_buf);

    __android_log_print(ANDROID_LOG_INFO, kTag,
            "gemv: M=%u K=%u q=%u seed=%u dsp_cycles=%llu compute_us=%lld max_rel_err=%.4e",
            M, K, q_block, seed,
            (unsigned long long)dsp_cycles, (long long)compute_us, max_rel);

    rpcmem_free(A_packed); rpcmem_free(A_scales); rpcmem_free(x_buf); rpcmem_free(y_buf);

    // Surface validation pass/fail to the broadcast caller. Tolerance per plan
    // §130 is 1e-2 relative error.
    return (max_rel <= 1e-2) ? 0 : -7;
}

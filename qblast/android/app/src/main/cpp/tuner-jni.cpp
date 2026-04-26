// JNI bridge for the qblast tuner APK.
//
// Phase-1 entry points:
//   nativePing()      — qblast_hello roundtrip; validates FastRPC plumbing.
//   nativeRunGemv()   — gemv_w4a16 baseline. Allocates rpcmem buffers, fills
//                       with seeded test data, runs warmup + iters reps on
//                       cDSP, validates the first rep against an FP32 ARM
//                       reference, and writes a JSON record to
//                       <resultsDir>/<cfg_id>.json.
//
// Both entry points cache their FastRPC handle as a static so the per-skel
// open() cost is amortized across calls. Handles leak on process exit, which
// is fine — the OS reaps them.

#include <jni.h>
#include <android/log.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <vector>

#include <stddef/AEEStdErr.h>
#include "rpcmem.h"
#include "remote.h"

#include "qblast_hello.h"  // qaic-generated
#include "gemv_w4a16.h"    // qaic-generated
#include "common/fp16_compat.h"

namespace {
constexpr const char* kTag = "qblast_jni";

// Process-wide state. Initialized lazily; never explicitly freed.
bool                  g_unsigned_pd_enabled = false;
remote_handle64       g_qblast_hello_handle = 0;
remote_handle64       g_gemv_w4a16_handle   = 0;

int ensure_unsigned_pd_cdsp() {
    if (g_unsigned_pd_enabled) return 0;
    struct remote_rpc_control_unsigned_module ctrl;
    ctrl.enable = 1;
    ctrl.domain = CDSP_DOMAIN_ID;
    int err = remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE, &ctrl, sizeof(ctrl));
    if (err) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                "remote_session_control unsigned-PD failed: 0x%x", err);
        return -1;
    }
    g_unsigned_pd_enabled = true;
    return 0;
}

template <typename OpenFn>
int ensure_handle(remote_handle64* slot, const char* uri_base, OpenFn open_fn,
                  const char* skel_name) {
    if (*slot != 0) return 0;
    char uri[256];
    snprintf(uri, sizeof(uri), "%s%s", uri_base, "&_dom=cdsp");
    int err = open_fn(uri, slot);
    if (err) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                "%s_open failed: 0x%x", skel_name, err);
        *slot = 0;
        return -1;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s_open ok (handle cached)", skel_name);
    return 0;
}

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

extern "C" JNIEXPORT jlong JNICALL
Java_com_qblast_tuner_TunerService_nativePing(JNIEnv* /*env*/, jclass /*cls*/) {
    if (ensure_unsigned_pd_cdsp() != 0) return -1;
    if (ensure_handle(&g_qblast_hello_handle, qblast_hello_URI, qblast_hello_open,
                      "qblast_hello") != 0) {
        return -1;
    }

    int64 magic = -1;
    uint64 dsp_cycles = 0;
    auto t0 = std::chrono::steady_clock::now();
    int err = qblast_hello_ping(g_qblast_hello_handle, &magic, &dsp_cycles);
    auto t1 = std::chrono::steady_clock::now();
    auto rtt_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    if (err) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "qblast_hello_ping failed: 0x%x", err);
        magic = -1;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
            "ping: magic=%lld dsp_cycles=%llu arm_rtt_us=%lld",
            (long long)magic, (unsigned long long)dsp_cycles, (long long)rtt_us);
    return (jlong)magic;
}

namespace {

struct Lcg {
    uint32_t s;
    explicit Lcg(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
};

// Reference matmul: uses the *unquantized* x (FP32) so the reported relative
// error includes both DSP-impl drift and x-quantization noise from the int8
// path. Plan §130 tolerance is 1e-2; expected magnitude here is ~1e-4 to ~1e-3.
double validate_gemv(uint32_t M, uint32_t K, uint32_t q_block,
                     const uint8_t* A_packed,
                     const int16_t* A_scales,
                     const float* x_fp32_ref,
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
                acc += (double)scale * (double)q * (double)x_fp32_ref[k];
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

template <typename T>
T median_of(std::vector<T> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Recursive mkdir -p; returns 0 on success.
int mkdirs(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
    return 0;
}

void write_result_json(const char* dir, int cfg_id,
                       uint32_t M, uint32_t K, uint32_t q_block,
                       uint32_t seed, int warmup, int iters,
                       const std::vector<uint64_t>& dsp_cycles,
                       const std::vector<int64_t>& compute_us,
                       double max_rel_err, const char* status) {
    if (mkdirs(dir) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                "mkdirs(%s) failed errno=%d", dir, errno);
        return;
    }
    char path[600];
    snprintf(path, sizeof(path), "%s/%d.json", dir, cfg_id);
    FILE* f = fopen(path, "w");
    if (!f) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                "fopen(%s) failed errno=%d", path, errno);
        return;
    }
    auto cyc_min = *std::min_element(dsp_cycles.begin(), dsp_cycles.end());
    auto cyc_max = *std::max_element(dsp_cycles.begin(), dsp_cycles.end());
    auto cyc_med = median_of(dsp_cycles);
    auto us_min = *std::min_element(compute_us.begin(), compute_us.end());
    auto us_max = *std::max_element(compute_us.begin(), compute_us.end());
    auto us_med = median_of(compute_us);
    fprintf(f,
        "{\n"
        "  \"cfg_id\": %d,\n"
        "  \"kernel\": \"gemv_w4a16\",\n"
        "  \"shape\": \"%u_%u_%u\",\n"
        "  \"M\": %u, \"K\": %u, \"q_block\": %u,\n"
        "  \"seed\": %u,\n"
        "  \"warmup\": %d, \"iters\": %d,\n"
        "  \"dsp_cycles_min\": %llu, \"dsp_cycles_med\": %llu, \"dsp_cycles_max\": %llu,\n"
        "  \"compute_us_min\": %lld, \"compute_us_med\": %lld, \"compute_us_max\": %lld,\n"
        "  \"max_rel_err\": %.6e,\n"
        "  \"status\": \"%s\"\n"
        "}\n",
        cfg_id, M, K, q_block, M, K, q_block, seed, warmup, iters,
        (unsigned long long)cyc_min, (unsigned long long)cyc_med, (unsigned long long)cyc_max,
        (long long)us_min, (long long)us_med, (long long)us_max,
        max_rel_err, status);
    fclose(f);
    __android_log_print(ANDROID_LOG_INFO, kTag, "wrote %s", path);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_qblast_tuner_TunerService_nativeRunGemv(
        JNIEnv* env, jclass /*cls*/,
        jint jCfgId, jint jM, jint jK, jint jQBlock,
        jint jSeed, jint jWarmup, jint jIters,
        jstring jResultsDir) {
    const int cfg_id = (int)jCfgId;
    const uint32_t M = (uint32_t)jM;
    const uint32_t K = (uint32_t)jK;
    const uint32_t q_block = (uint32_t)jQBlock;
    const uint32_t seed = (uint32_t)jSeed;
    const int warmup = jWarmup < 0 ? 0 : (int)jWarmup;
    const int iters = jIters < 1 ? 1 : (int)jIters;

    if (M == 0 || K == 0 || q_block == 0 || (K % q_block) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                "nativeRunGemv: bad args M=%u K=%u q=%u", M, K, q_block);
        return -1;
    }

    const char* results_dir = env->GetStringUTFChars(jResultsDir, nullptr);
    if (!results_dir) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "nativeRunGemv: null resultsDir");
        return -2;
    }
    char results_dir_copy[512];
    snprintf(results_dir_copy, sizeof(results_dir_copy), "%s", results_dir);
    env->ReleaseStringUTFChars(jResultsDir, results_dir);

    if (ensure_unsigned_pd_cdsp() != 0) return -3;
    if (ensure_handle(&g_gemv_w4a16_handle, gemv_w4a16_URI, gemv_w4a16_open,
                      "gemv_w4a16") != 0) {
        return -4;
    }

    const size_t a_packed_bytes = (size_t)M * K / 2;
    const size_t a_scales_bytes = (size_t)M * (K / q_block) * sizeof(int16_t);
    const size_t x_quant_bytes = (size_t)K;                           // int8
    const size_t y_bytes = (size_t)M * sizeof(int16_t);

    // ION buffers (FastRPC zero-copies these).
    uint8_t* A_packed = (uint8_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, a_packed_bytes);
    int16_t* A_scales = (int16_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, a_scales_bytes);
    uint8_t* x_quant  = (uint8_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, x_quant_bytes);
    int16_t* y_buf    = (int16_t*)rpcmem_alloc(RPCMEM_HEAP_ID_SYSTEM, RPCMEM_DEFAULT_FLAGS, y_bytes);

    // Host-only FP32 mirror of x for the reference validator.
    std::vector<float> x_fp32_ref(K, 0.0f);

    if (!A_packed || !A_scales || !x_quant || !y_buf) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "rpcmem_alloc failed");
        if (A_packed) rpcmem_free(A_packed);
        if (A_scales) rpcmem_free(A_scales);
        if (x_quant) rpcmem_free(x_quant);
        if (y_buf) rpcmem_free(y_buf);
        return -5;
    }

    // Test data. x is generated as FP32 and quantized into int8 Q.7
    // (range [-1,1] → [-127,127]). x_scale = 1/127 reconstructs.
    Lcg rng(seed);
    for (size_t i = 0; i < a_packed_bytes; ++i) {
        A_packed[i] = (uint8_t)(rng.next() & 0xFFu);
    }
    const size_t n_scales = a_scales_bytes / sizeof(int16_t);
    for (size_t i = 0; i < n_scales; ++i) {
        float val = 0.001f + 0.004f * (float)(rng.next() & 0xFFu) / 255.0f;
        A_scales[i] = qblast_fp32_to_fp16(val);
    }
    const float x_scale = 1.0f / 127.0f;
    for (size_t i = 0; i < K; ++i) {
        float val = (float)((int32_t)(rng.next() & 0xFFFFu) - 32768) / 32768.0f;
        x_fp32_ref[i] = val;
        int q = (int)lroundf(val * 127.0f);
        if (q > 127) q = 127;
        if (q < -128) q = -128;
        x_quant[i] = (uint8_t)(int8_t)q;  // octet wire format, signed reinterpret on DSP
    }
    memset(y_buf, 0, y_bytes);

    // Warmup.
    for (int i = 0; i < warmup; ++i) {
        uint64 dsp_cycles_unused = 0;
        int err = gemv_w4a16_compute(
                g_gemv_w4a16_handle, M, K, q_block,
                A_packed, (int)a_packed_bytes,
                A_scales, (int)n_scales,
                x_quant, (int)x_quant_bytes,
                x_scale,
                y_buf, (int)M,
                &dsp_cycles_unused);
        if (err) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                    "warmup iter %d: gemv_w4a16_compute err %d", i, err);
            rpcmem_free(A_packed); rpcmem_free(A_scales); rpcmem_free(x_quant); rpcmem_free(y_buf);
            return -6;
        }
    }

    double max_rel = validate_gemv(M, K, q_block, A_packed, A_scales, x_fp32_ref.data(), y_buf);

    std::vector<uint64_t> dsp_cycles_v;
    std::vector<int64_t>  compute_us_v;
    dsp_cycles_v.reserve(iters);
    compute_us_v.reserve(iters);

    for (int i = 0; i < iters; ++i) {
        uint64 dsp_cycles = 0;
        auto t0 = std::chrono::steady_clock::now();
        int err = gemv_w4a16_compute(
                g_gemv_w4a16_handle, M, K, q_block,
                A_packed, (int)a_packed_bytes,
                A_scales, (int)n_scales,
                x_quant, (int)x_quant_bytes,
                x_scale,
                y_buf, (int)M,
                &dsp_cycles);
        auto t1 = std::chrono::steady_clock::now();
        if (err) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                    "iter %d: gemv_w4a16_compute err %d", i, err);
            rpcmem_free(A_packed); rpcmem_free(A_scales); rpcmem_free(x_quant); rpcmem_free(y_buf);
            return -7;
        }
        dsp_cycles_v.push_back((uint64_t)dsp_cycles);
        compute_us_v.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }

    rpcmem_free(A_packed); rpcmem_free(A_scales); rpcmem_free(x_quant); rpcmem_free(y_buf);

    const bool ok = max_rel <= 1e-2;
    const char* status = ok ? "ok" : "rel_err_exceeds_tolerance";

    auto cyc_med = median_of(dsp_cycles_v);
    auto us_med = median_of(compute_us_v);
    __android_log_print(ANDROID_LOG_INFO, kTag,
            "gemv: cfg=%d M=%u K=%u q=%u seed=%u warmup=%d iters=%d "
            "dsp_cycles_med=%llu compute_us_med=%lld max_rel_err=%.4e status=%s",
            cfg_id, M, K, q_block, seed, warmup, iters,
            (unsigned long long)cyc_med, (long long)us_med, max_rel, status);

    write_result_json(results_dir_copy, cfg_id, M, K, q_block, seed, warmup, iters,
                      dsp_cycles_v, compute_us_v, max_rel, status);

    return ok ? 0 : -8;
}

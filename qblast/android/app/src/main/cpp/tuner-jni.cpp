// JNI bridge for the qblast tuner APK.
//
// Phase-1 Week-2 Day-1: adds nativePing(), which opens a FastRPC session to the
// qblast_hello DSP skel on cDSP, calls its ping() method, and returns the magic
// number (4950) plus a side-channel log of DSP cycles + ARM-side RTT. Validates
// the IDL/stub/skel/cdsprpc plumbing end-to-end.
//
// Replaced by gemv_w4a16 once Day-1 lights up.

#include <jni.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

#include <stddef/AEEStdErr.h>
#include "remote.h"

#include "qblast_hello.h"  // qaic-generated from qblast_hello.idl

namespace {
constexpr const char* kTag = "qblast_jni";
}

// Sets DSP_LIBRARY_PATH so fastrpc finds variant skel libs pushed to /data/local/tmp/.
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

// Calls qblast_hello_ping on the cDSP. Returns magic on success, -1 on failure.
// DSP cycle count + ARM RTT logged via __android_log_print.
extern "C" JNIEXPORT jlong JNICALL
Java_com_qblast_tuner_TunerService_nativePing(JNIEnv* /*env*/, jclass /*cls*/) {
    remote_handle64 handle = 0;
    int err = 0;
    // Use SDK's int64/uint64 (long long) typedefs to match qaic-generated
    // qblast_hello_ping signature exactly — int64_t (long) won't overload.
    int64 magic = -1;
    uint64 dsp_cycles = 0;

    // Unsigned-PD enable: on retail OnePlus 15 we are not Qualcomm-signed, so
    // FastRPC must be told to use unsigned-module mode before any _open call.
    // remote_session_control is provided by libcdsprpc.so at dynamic link time;
    // if the .so failed to load we'd already be dead before reaching this code.
    {
        struct remote_rpc_control_unsigned_module ctrl;
        ctrl.enable = 1;
        ctrl.domain = CDSP_DOMAIN_ID;
        err = remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE, &ctrl, sizeof(ctrl));
        if (err) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                    "remote_session_control unsigned-PD failed: 0x%x", err);
            return -1;
        }
    }

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
        magic = -1;  // surface failure to Java even if close succeeds below
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

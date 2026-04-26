// JNI bridge for the qblast tuner APK. Phase 1: only sets DSP_LIBRARY_PATH so the
// fastrpc loader can find variant .so files pushed to /data/local/tmp/. Week 2 adds
// FastRPC + skel calls for the GEMV W4A16 baseline.

#include <jni.h>
#include <stdlib.h>
#include <android/log.h>

namespace {
constexpr const char* kTag = "qblast_jni";
}

extern "C" JNIEXPORT void JNICALL
Java_com_qblast_tuner_MainActivity_init(JNIEnv* env, jobject /*self*/, jstring location) {
    const char* path = env->GetStringUTFChars(location, nullptr);
    if (path == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "init: null location string");
        return;
    }
    if (setenv("DSP_LIBRARY_PATH", path, 1) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "setenv DSP_LIBRARY_PATH=%s failed", path);
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "DSP_LIBRARY_PATH=%s", path);
    }
    env->ReleaseStringUTFChars(location, path);
}

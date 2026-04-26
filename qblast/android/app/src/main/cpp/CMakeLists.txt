cmake_minimum_required(VERSION 3.18)
project(qblast_tuner_jni)

# HEXAGON_SDK_ROOT is passed in via app/build.gradle's externalNativeBuild
# arguments (read from android/local.properties). Fail fast if missing.
if(NOT DEFINED HEXAGON_SDK_ROOT OR HEXAGON_SDK_ROOT STREQUAL "")
    message(FATAL_ERROR
        "HEXAGON_SDK_ROOT not set. Add `hexagon.sdk.root=...` to "
        "qblast/android/local.properties.")
endif()

if(NOT EXISTS "${HEXAGON_SDK_ROOT}/build/cmake/hexagon_fun.cmake")
    message(FATAL_ERROR
        "HEXAGON_SDK_ROOT=${HEXAGON_SDK_ROOT} is not a Hexagon SDK install "
        "(missing build/cmake/hexagon_fun.cmake).")
endif()

# QBLAST_KERNELS_DIR points at qblast/src/kernels/hexagon/, also passed by Gradle.
if(NOT DEFINED QBLAST_KERNELS_DIR OR QBLAST_KERNELS_DIR STREQUAL "")
    message(FATAL_ERROR "QBLAST_KERNELS_DIR not set (should come from app/build.gradle).")
endif()

# hexagon_fun.cmake reads PREBUILT_LIB_DIR (line ~103) at include-time;
# must be set before the include or the string(FIND ...) calls explode.
set(PREBUILT_LIB_DIR "android_aarch64")

# Pull in build_idl() and friends.
include(${HEXAGON_SDK_ROOT}/build/cmake/hexagon_fun.cmake)

# Headers Hexagon SDK FastRPC clients need + the qblast common helpers
# (fp16_compat.h lives at ${QBLAST_KERNELS_DIR}/common/).
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}/                       # qaic-generated *.h
    ${HEXAGON_SDK_ROOT}/incs/
    ${HEXAGON_SDK_ROOT}/incs/stddef/
    ${HEXAGON_SDK_ROOT}/ipc/fastrpc/rpcmem/inc/
    ${HEXAGON_SDK_ROOT}/utils/examples
    ${QBLAST_KERNELS_DIR}                               # so #include "common/fp16_compat.h" works
)

add_library(qblast_tuner_jni SHARED
    tuner-jni.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/qblast_hello_stub.c
    ${CMAKE_CURRENT_BINARY_DIR}/gemv_w4a16_stub.c
)

# Generate stubs + headers for both IDLs. Each invocation drops a foo_stub.c
# and foo.h into ${CMAKE_CURRENT_BINARY_DIR}.
build_idl("${QBLAST_KERNELS_DIR}/qblast_hello/qblast_hello.idl" qblast_tuner_jni)
build_idl("${QBLAST_KERNELS_DIR}/gemv_w4a16/gemv_w4a16.idl" qblast_tuner_jni)

# libcdsprpc.so is the FastRPC client library shipped by the SDK for arm64.
target_link_directories(qblast_tuner_jni PUBLIC
    "${HEXAGON_SDK_ROOT}/ipc/fastrpc/remote/ship/android_aarch64"
)

find_library(log-lib log)
target_link_libraries(qblast_tuner_jni cdsprpc ${log-lib})

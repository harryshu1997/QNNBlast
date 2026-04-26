cmake_minimum_required(VERSION 3.18)
project(qblast LANGUAGES CXX)

# Phase 1 stub. The APK is built by Gradle in android/, not by this CMakeLists.
# Real subprojects land here from Week 6 onward (host tuner_driver, ARM-side
# routine layer, database codegen integration). For now, building this top-level
# CMake is a no-op.
message(STATUS "qblast top-level CMake is a Phase-1 placeholder.")
message(STATUS "Build the APK with: (cd android && ./gradlew assembleDebug)")

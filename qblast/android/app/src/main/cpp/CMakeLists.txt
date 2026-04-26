cmake_minimum_required(VERSION 3.18)
project(qblast_tuner_jni)

add_library(qblast_tuner_jni SHARED
    tuner-jni.cpp
)

find_library(log-lib log)
target_link_libraries(qblast_tuner_jni ${log-lib})

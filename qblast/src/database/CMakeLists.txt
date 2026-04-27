cmake_minimum_required(VERSION 3.18)
project(qblast_database CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only lib for now; the runtime uses inline lookup_variant().
add_library(qblast_database INTERFACE)
target_include_directories(qblast_database INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Host-side test harness — runs on the build machine, no Hexagon SDK.
add_executable(test_database test_database.cpp)
target_link_libraries(test_database PRIVATE qblast_database)
target_compile_options(test_database PRIVATE -Wall -Wextra -Wpedantic)

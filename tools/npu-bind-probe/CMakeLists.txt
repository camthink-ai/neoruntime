cmake_minimum_required(VERSION 3.10)
project(npu-bind-probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# HailoRT headers + library live in the Poky target sysroot; the repo toolchain
# file (cmake/toolchain-aarch64-hailo.cmake) pins CMAKE_FIND_ROOT_PATH to it,
# so the plain find_* calls below resolve inside the sysroot only.
find_path(HAILORT_INC_DIR NAMES hailo/hailort.hpp)
find_library(HAILORT_LIB NAMES hailort)

if(NOT HAILORT_INC_DIR OR NOT HAILORT_LIB)
    message(FATAL_ERROR "HailoRT not found in sysroot — configure with "
        "-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-hailo.cmake "
        "-DHAILO_SDK_PATH=/opt/poky/4.0.23")
endif()

add_executable(npu-bind-probe npu_bind_probe.cpp)
target_include_directories(npu-bind-probe PRIVATE ${HAILORT_INC_DIR})
target_link_libraries(npu-bind-probe PRIVATE ${HAILORT_LIB} pthread)

install(TARGETS npu-bind-probe DESTINATION /opt/aipc/bin)

# Standalone build for test_peripheral_all_func only (does not modify hal_v2/CMakeLists.txt).
# Links peripheral HAL + common sources into one executable so libaipc_hal.so / hal-*.so are not required at runtime.
# Sysroot libs (e.g. libgpiod) may still be needed.

cmake_minimum_required(VERSION 3.16)
project(hal_test_peripheral_standalone LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(Threads REQUIRED)

get_filename_component(HAL_V2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HAL_V2_INC "${HAL_V2_ROOT}/include")

# CPU draw path only (avoid hal_draw_hailo15 + OpenCV).
set(_HAL_DRAW_OPS_SRC "${HAL_V2_ROOT}/common/hal_draw_ops_cpu.cpp")

set(_HAL_PERIPH_HAL_SOURCES
    ${HAL_V2_ROOT}/common/hal_common.c
    ${HAL_V2_ROOT}/common/hal_log.c
    ${HAL_V2_ROOT}/common/hal_buffer.c
    ${HAL_V2_ROOT}/common/hal_postprocess_common.c
    ${HAL_V2_ROOT}/common/hal_clip_scoring.cpp
    ${HAL_V2_ROOT}/common/hal_draw_cpu.cpp
    ${_HAL_DRAW_OPS_SRC}
    ${HAL_V2_ROOT}/common/hal_udp_stream.cpp
    ${HAL_V2_ROOT}/common/host_link/host_link_core.c
    ${HAL_V2_ROOT}/common/host_link/host_link_port_posix.c
    ${HAL_V2_ROOT}/platforms/hailo15/mcu/hailo15_mcu_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/io/hailo15_io_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_rtc_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_led_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_sensor_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_alarm_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_rs485_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_lens_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_env_ctrl_impl.cpp
    ${HAL_V2_ROOT}/common/devices/hal_lens_af0832.c
)

# --- libgpiod ---
# Never add a Yocto SDK `usr/include` to native builds: it shadows the host libc and breaks compilation.
if(CMAKE_CROSSCOMPILING)
    if(NOT CMAKE_SYSROOT OR CMAKE_SYSROOT STREQUAL "")
        message(FATAL_ERROR "Cross-build requires CMAKE_SYSROOT (use your Poky/SDK toolchain file).")
    endif()
    find_library(GPIOD_LIB NAMES gpiod
        PATHS
            "${CMAKE_SYSROOT}/usr/lib"
            "${CMAKE_SYSROOT}/usr/lib64"
            "${CMAKE_SYSROOT}/lib"
            "${CMAKE_SYSROOT}/lib64"
        NO_DEFAULT_PATH
    )
    if(NOT GPIOD_LIB)
        message(FATAL_ERROR "libgpiod not found under sysroot: ${CMAKE_SYSROOT}")
    endif()
    set(GPIOD_INC_DIR "")
else()
    find_path(GPIOD_INC_DIR NAMES gpiod.h PATHS /usr/include /usr/local/include)
    find_library(GPIOD_LIB NAMES gpiod PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
    if(NOT GPIOD_INC_DIR OR NOT GPIOD_LIB)
        message(FATAL_ERROR "Native build needs libgpiod (e.g. apt install libgpiod-dev on Debian/Ubuntu).")
    endif()
endif()

add_executable(ne503_mcu_cli_tool
    test_peripheral_all_func.cpp
    ${_HAL_PERIPH_HAL_SOURCES}
)

target_include_directories(ne503_mcu_cli_tool PRIVATE
    ${HAL_V2_INC}
    ${HAL_V2_ROOT}
)
if(NOT CMAKE_CROSSCOMPILING AND GPIOD_INC_DIR)
    target_include_directories(ne503_mcu_cli_tool PRIVATE ${GPIOD_INC_DIR})
endif()

target_link_libraries(ne503_mcu_cli_tool PRIVATE
    Threads::Threads
    ${GPIOD_LIB}
)

target_compile_options(ne503_mcu_cli_tool PRIVATE -Wall -Wextra)

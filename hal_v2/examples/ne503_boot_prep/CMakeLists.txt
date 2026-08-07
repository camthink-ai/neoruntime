# Standalone build for ne503_boot_prep (MCU RTC + optional OTA); no libhal.so required at runtime.
# Needs libgpiod for MCU NRST (same as test_peripheral_all_func).

cmake_minimum_required(VERSION 3.16)
project(ne503_boot_prep_standalone LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(Threads REQUIRED)

get_filename_component(HAL_V2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HAL_V2_INC "${HAL_V2_ROOT}/include")

set(_HAL_BOOT_PREP_SOURCES
    ${HAL_V2_ROOT}/common/hal_common.c
    ${HAL_V2_ROOT}/common/hal_log.c
    ${HAL_V2_ROOT}/common/host_link/host_link_core.c
    ${HAL_V2_ROOT}/common/host_link/host_link_port_posix.c
    ${HAL_V2_ROOT}/platforms/hailo15/mcu/hailo15_mcu_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_rtc_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_env_ctrl_impl.cpp
    ${HAL_V2_ROOT}/platforms/hailo15/peripherals/hailo15_ota_impl.cpp
)

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

add_executable(ne503_boot_prep
    ne503_boot_prep.cpp
    ${_HAL_BOOT_PREP_SOURCES}
)

target_include_directories(ne503_boot_prep PRIVATE
    ${HAL_V2_INC}
    ${HAL_V2_ROOT}
)
if(NOT CMAKE_CROSSCOMPILING AND GPIOD_INC_DIR)
    target_include_directories(ne503_boot_prep PRIVATE ${GPIOD_INC_DIR})
endif()

target_link_libraries(ne503_boot_prep PRIVATE
    Threads::Threads
    ${GPIOD_LIB}
)

target_compile_options(ne503_boot_prep PRIVATE -Wall -Wextra)

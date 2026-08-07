cmake_minimum_required(VERSION 3.16)
project(hal_v2 VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(HAL_V2_BUILD_MONOLITHIC "Build a single libaipc_hal.so instead of many component .so files" ON)

if(NOT HAL_PLATFORM)
    set(HAL_PLATFORM "hailo15" CACHE STRING "HAL platform: hailo15 | stub (no vendor media/ISP/OSD)")
endif()

# Stub is a native host library (glibc + host GCC). Never reuse a Yocto CMAKE_SYSROOT from a
# previous hailo15 configure — that mixes --sysroot with the desktop compiler and breaks libc
# headers (e.g. bits/timesize-32.h).
if(HAL_PLATFORM STREQUAL "stub")
    set(CMAKE_SYSROOT "" CACHE PATH "stub: native host sysroot only (clear for SDK-free build)" FORCE)
    message(STATUS "HAL_PLATFORM=stub: using native host toolchain (CMAKE_SYSROOT cleared)")
endif()

if(HAL_PLATFORM STREQUAL "hailo15")
    set(_HAL_DRAW_BACKEND_DEFAULT "hailo15")
else()
    set(_HAL_DRAW_BACKEND_DEFAULT "cpu")
endif()
set(HAL_DRAW_BACKEND "${_HAL_DRAW_BACKEND_DEFAULT}" CACHE STRING "HAL draw: cpu | hailo15 (Hailo-15 default: HailoNV12Mat on NV12 Y/UV when hailo_postprocess_tools+OpenCV; else CPU)")

set(HAL_V2_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(HAL_V2_INC ${HAL_V2_ROOT}/include)

set(_HAL_V2_DRAW_OPS_SRC common/hal_draw_ops_cpu.cpp)
if(HAL_PLATFORM STREQUAL "hailo15" AND HAL_DRAW_BACKEND STREQUAL "hailo15")
    set(_HAL_V2_DRAW_OPS_SRC platforms/hailo15/model/hal_draw_hailo15.cpp)
endif()

set(_HAL_V2_COMMON_SOURCES
    common/hal_common.c
    common/hal_log.c
    common/hal_buffer.c
    common/hal_audio_format.c
    common/hal_postprocess_common.c
    common/hal_clip_scoring.cpp
    common/hal_draw_cpu.cpp
    ${_HAL_V2_DRAW_OPS_SRC}
    common/hal_udp_stream.cpp
    common/host_link/host_link_core.c
    common/host_link/host_link_port_posix.c
)

if(HAL_V2_BUILD_MONOLITHIC)
    add_library(hal SHARED)
    target_sources(hal PRIVATE ${_HAL_V2_COMMON_SOURCES})
    target_include_directories(hal PUBLIC ${HAL_V2_INC})
    target_link_libraries(hal PRIVATE pthread)
    target_compile_options(hal PRIVATE -Wall -Wextra -fPIC)
    set_target_properties(hal PROPERTIES
        OUTPUT_NAME aipc_hal
        VERSION ${PROJECT_VERSION}
        SOVERSION 2
    )
else()
    # Common helpers (C + optional C++ helpers)
    add_library(hal-common SHARED ${_HAL_V2_COMMON_SOURCES})
    target_include_directories(hal-common PUBLIC ${HAL_V2_INC})
    target_link_libraries(hal-common PRIVATE pthread)
    target_compile_options(hal-common PRIVATE -Wall -Wextra -fPIC)
    set_target_properties(hal-common PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION 2
    )
endif()

if(HAL_PLATFORM STREQUAL "hailo15")
    if(NOT CMAKE_CROSSCOMPILING AND DEFINED ENV{OECORE_TARGET_SYSROOT})
        set(CMAKE_SYSROOT $ENV{OECORE_TARGET_SYSROOT} CACHE PATH "cross sysroot" FORCE)
    endif()
    # Fallback for developer environments: when OECORE_TARGET_SYSROOT isn't exported, try local SDK sysroots.
    # (keeps `build-hailo15` buildable on host machines).
    # Primary: /opt/poky/4.0.23/ (system SDK install)
    # Fallback: repo-adjacent sdk_4.0.23/ or ../../sdk_4.0.23/
    set(_LOCAL_SDK_SYSROOT "/opt/poky/4.0.23/sysroots/armv8a-poky-linux")
    set(_LOCAL_SDK_SYSROOT2 "${CMAKE_CURRENT_SOURCE_DIR}/../sdk_4.0.23/sysroots/armv8a-poky-linux")
    set(_LOCAL_SDK_SYSROOT3 "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk_4.0.23/sysroots/armv8a-poky-linux")
    if(NOT CMAKE_SYSROOT OR CMAKE_SYSROOT STREQUAL "")
        if(EXISTS "${_LOCAL_SDK_SYSROOT}/usr/include/hailo/media_library/media_library.hpp")
            set(CMAKE_SYSROOT "${_LOCAL_SDK_SYSROOT}" CACHE PATH "cross sysroot" FORCE)
            message(STATUS "Using local SDK sysroot: ${CMAKE_SYSROOT}")
        elseif(EXISTS "${_LOCAL_SDK_SYSROOT2}/usr/include/hailo/media_library/media_library.hpp")
            set(CMAKE_SYSROOT "${_LOCAL_SDK_SYSROOT2}" CACHE PATH "cross sysroot" FORCE)
            message(STATUS "Using local SDK sysroot: ${CMAKE_SYSROOT}")
        elseif(EXISTS "${_LOCAL_SDK_SYSROOT3}/usr/include/hailo/media_library/media_library.hpp")
            set(CMAKE_SYSROOT "${_LOCAL_SDK_SYSROOT3}" CACHE PATH "cross sysroot" FORCE)
            message(STATUS "Using local SDK sysroot: ${CMAKE_SYSROOT}")
        endif()
    endif()

    # Include search roots for the Hailo SDK.
    # NOTE: Some cross toolchains provide --sysroot via compiler flags but do not populate CMAKE_SYSROOT.
    # We therefore always prefer a repo-local SDK include path when present, and only then fall back to
    # ${CMAKE_SYSROOT} roots.
    # Prefer repo-adjacent SDK sysroots when present.
    set(HAILO_ML_INC "")
    if(EXISTS "${_LOCAL_SDK_SYSROOT}/usr/include/hailo/media_library/media_library.hpp")
        list(APPEND HAILO_ML_INC
            "${_LOCAL_SDK_SYSROOT}/usr/include/hailo"
            "${_LOCAL_SDK_SYSROOT}/usr/include"
        )
    elseif(EXISTS "${_LOCAL_SDK_SYSROOT2}/usr/include/hailo/media_library/media_library.hpp")
        list(APPEND HAILO_ML_INC
            "${_LOCAL_SDK_SYSROOT2}/usr/include/hailo"
            "${_LOCAL_SDK_SYSROOT2}/usr/include"
        )
    elseif(EXISTS "${_LOCAL_SDK_SYSROOT3}/usr/include/hailo/media_library/media_library.hpp")
        list(APPEND HAILO_ML_INC
            "${_LOCAL_SDK_SYSROOT3}/usr/include/hailo"
            "${_LOCAL_SDK_SYSROOT3}/usr/include"
        )
    endif()
    if(DEFINED CMAKE_SYSROOT AND NOT CMAKE_SYSROOT STREQUAL "")
        list(APPEND HAILO_ML_INC
            "${CMAKE_SYSROOT}/usr/include/hailo"
            "${CMAKE_SYSROOT}/usr/include"
        )
    endif()

    # Optional: HailoRT (inference) headers + library.
    find_path(HAILORT_INC_DIR
        NAMES hailo/hailort.hpp
        PATHS ${HAILO_ML_INC}
        NO_DEFAULT_PATH
    )
    find_library(HAILORT_LIB
        NAMES hailort
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/usr/lib64 ${CMAKE_SYSROOT}/lib ${CMAKE_SYSROOT}/lib64
        NO_DEFAULT_PATH
    )
    set(HAVE_HAILORT OFF)
    if(HAILORT_INC_DIR AND HAILORT_LIB)
        set(HAVE_HAILORT ON)
    endif()

    # HailoRT GenAI (LLM/VLM) headers ship with libhailort in SDK 4.x+.
    set(HAVE_HAL_GENAI OFF)
    if(HAVE_HAILORT AND HAILORT_INC_DIR AND EXISTS "${HAILORT_INC_DIR}/hailo/genai/llm/llm.hpp")
        set(HAVE_HAL_GENAI ON)
    endif()

    # Some hailo15 components depend on Hailo media library headers + lib (medialib).
    # Allow building non-media components (MCU/IO/peripherals) even when medialib is absent.
    find_path(HAILO_MEDIALIB_INC_DIR
        NAMES
            media_library/media_library.hpp
            media_library/buffer_pool.hpp
        PATHS ${HAILO_ML_INC}
        NO_DEFAULT_PATH
    )
    find_library(HAILO_MEDIALIB_LIB
        NAMES medialib
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/usr/lib64 ${CMAKE_SYSROOT}/lib ${CMAKE_SYSROOT}/lib64
        NO_DEFAULT_PATH
    )
    set(HAVE_HAILO_MEDIALIB OFF)
    if(HAILO_MEDIALIB_INC_DIR AND HAILO_MEDIALIB_LIB
       AND EXISTS "${HAILO_MEDIALIB_INC_DIR}/media_library/media_library.hpp"
       AND EXISTS "${HAILO_MEDIALIB_INC_DIR}/media_library/buffer_pool.hpp")
        set(HAVE_HAILO_MEDIALIB ON)
    endif()

    # NOTE: this helper intentionally accepts a variable number of source files.
    # Use ARGN so calls like:
    #   hal_hailo15_target(foo a.cpp b.cpp c.cpp)
    # include *all* sources, not just the first one.
    function(hal_hailo15_target name)
        set(sources ${ARGN})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_sources(hal PRIVATE ${sources})
            target_include_directories(hal PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC} ${HAILO_ML_INC} ${HAILO_MEDIALIB_INC_DIR})
            target_link_libraries(hal PRIVATE ${HAILO_MEDIALIB_LIB})
        else()
            add_library(${name} SHARED ${sources})
            target_include_directories(${name} PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC} ${HAILO_ML_INC} ${HAILO_MEDIALIB_INC_DIR})
            target_link_libraries(${name} PRIVATE hal-common ${HAILO_MEDIALIB_LIB} pthread)
            target_compile_options(${name} PRIVATE -Wall -Wextra -fPIC)
            set_target_properties(${name} PROPERTIES
                VERSION ${PROJECT_VERSION}
                SOVERSION 2
            )
        endif()
    endfunction()

    function(hal_hailo15_plain_target name)
        set(sources ${ARGN})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_sources(hal PRIVATE ${sources})
            target_include_directories(hal PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
        else()
            add_library(${name} SHARED ${sources})
            target_include_directories(${name} PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
            target_link_libraries(${name} PRIVATE hal-common pthread)
            target_compile_options(${name} PRIVATE -Wall -Wextra -fPIC)
            set_target_properties(${name} PROPERTIES
                VERSION ${PROJECT_VERSION}
                SOVERSION 2
            )
        endif()
    endfunction()

    if(HAVE_HAILO_MEDIALIB)
        # ---- Compiled-in default medialib config (self-contained fallback when the caller
        # ---- passes neither config_path nor config_json). Generated at build time from the
        # ---- SDK's webserver config for the Basic profiles; references rewritten to a scratch
        # ---- dir so no on-device config path is needed at runtime.
        set(HAL_V2_EMBED_DEFAULT_MEDIALIB ON CACHE BOOL "Embed a self-contained default medialib config in the HAL")
        set(HAL_V2_DEFAULT_MEDIALIB_PROFILES "Daylight_Basic,High_Dynamic_Range_Basic,AI_ISP_Gen1_Basic,AI_ISP_Gen2_Basic,AI_ISP_Gen3_Basic" CACHE STRING "Profiles embedded in the default medialib config")
        set(HAL_V2_DEFAULT_MEDIALIB_SCRATCH_ROOT "/var/tmp/hal_medialib_default" CACHE STRING "Runtime scratch dir for the embedded default medialib config")
        set(HAL_V2_DEFAULT_MEDIALIB_WS_CFG "etc/imaging/cfg/medialib_configs/webserver_medialib_config.json" CACHE STRING "Webserver medialib config path (relative to SDK sysroot)")
        get_filename_component(_HAL_SDK_SYSROOT "${HAILO_MEDIALIB_INC_DIR}/../../.." ABSOLUTE)
        find_program(HAL_PYTHON3 python3 REQUIRED)
        set(_GEN_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/hailo15_media")
        set(_GEN_BUNDLE "${_GEN_BUNDLE_DIR}/hailo15_default_medialib_bundle.cpp")
        if(HAL_V2_EMBED_DEFAULT_MEDIALIB)
            add_custom_command(
                OUTPUT ${_GEN_BUNDLE}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_GEN_BUNDLE_DIR}
                COMMAND ${HAL_PYTHON3}
                        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/hailo15/media/gen_default_medialib_bundle.py
                        --sysroot ${_HAL_SDK_SYSROOT}
                        --webserver-config ${HAL_V2_DEFAULT_MEDIALIB_WS_CFG}
                        --scratch-root ${HAL_V2_DEFAULT_MEDIALIB_SCRATCH_ROOT}
                        --profiles ${HAL_V2_DEFAULT_MEDIALIB_PROFILES}
                        --out ${_GEN_BUNDLE}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platforms/hailo15/media/gen_default_medialib_bundle.py
                COMMENT "Generating self-contained default medialib bundle (5 Basic profiles)"
                VERBATIM
            )
            set(_DEFAULT_MEDIALIB_SOURCES
                platforms/hailo15/media/hailo15_default_medialib.cpp
                ${_GEN_BUNDLE})
        else()
            set(_DEFAULT_MEDIALIB_SOURCES "")
        endif()

        hal_hailo15_target(hal-hailo15-media platforms/hailo15/media/hailo15_media_impl.cpp
            platforms/hailo15/media/hailo15_ml_frontend_bridge.cpp
            platforms/hailo15/media/hailo15_medialib_config_field.cpp
            ${_DEFAULT_MEDIALIB_SOURCES})
        hal_hailo15_target(hal-hailo15-video platforms/hailo15/media/hailo15_video_impl.cpp
            platforms/hailo15/media/hailo15_ml_frontend_bridge.cpp
            platforms/hailo15/media/hailo15_medialib_config_resolve.cpp)
        hal_hailo15_target(hal-hailo15-codec platforms/hailo15/media/hailo15_codec_impl.cpp
            platforms/hailo15/media/hailo15_medialib_config_resolve.cpp)
        set(_HAILO15_CFG_TMPL "${CMAKE_CURRENT_SOURCE_DIR}/platforms/hailo15/media/config_templates")
        if(HAL_V2_BUILD_MONOLITHIC)
            target_compile_definitions(hal PRIVATE "HAILO15_CFG_TEMPLATE_ROOT=\"${_HAILO15_CFG_TMPL}\"")
        else()
            target_compile_definitions(hal-hailo15-video PRIVATE "HAILO15_CFG_TEMPLATE_ROOT=\"${_HAILO15_CFG_TMPL}\"")
            target_compile_definitions(hal-hailo15-codec PRIVATE "HAILO15_CFG_TEMPLATE_ROOT=\"${_HAILO15_CFG_TMPL}\"")
        endif()
        hal_hailo15_target(hal-hailo15-isp platforms/hailo15/media/hailo15_isp_impl.cpp)
        hal_hailo15_target(hal-hailo15-osd platforms/hailo15/media/hailo15_osd_impl.cpp)

        # Media calls into ISP for webserver-equivalent reset side-effects; ensure consumers of the media HAL
        # automatically pull in the ISP implementation.
        if(NOT HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-hailo15-media PRIVATE hal-hailo15-isp)
        endif()
    else()
        message(WARNING "Hailo medialib headers/lib not found; skipping media/video/codec/isp/osd targets.")
    endif()

    # Audio capture/encode does not depend on Hailo MediaLibrary (ALSA + libavcodec from SDK sysroot).
    find_library(HAL_V2_ASOUND_LIB NAMES asound
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
    find_library(HAL_V2_AVCODEC_LIB NAMES avcodec
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
    find_library(HAL_V2_AVUTIL_LIB NAMES avutil
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
    find_library(HAL_V2_SWRESAMPLE_LIB NAMES swresample
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/lib
        NO_DEFAULT_PATH)
    if(HAL_V2_ASOUND_LIB AND HAL_V2_AVCODEC_LIB AND HAL_V2_AVUTIL_LIB AND HAL_V2_SWRESAMPLE_LIB)
        if(HAL_V2_BUILD_MONOLITHIC)
            target_sources(hal PRIVATE platforms/hailo15/media/hailo15_audio_impl.cpp)
            target_include_directories(hal PRIVATE
                ${CMAKE_SYSROOT}/usr/include
                ${CMAKE_SYSROOT}/usr/include/alsa)
            target_link_libraries(hal PRIVATE
                ${HAL_V2_ASOUND_LIB}
                ${HAL_V2_AVCODEC_LIB}
                ${HAL_V2_AVUTIL_LIB}
                ${HAL_V2_SWRESAMPLE_LIB})
        else()
            add_library(hal-hailo15-audio SHARED platforms/hailo15/media/hailo15_audio_impl.cpp)
            target_include_directories(hal-hailo15-audio PRIVATE
                ${HAL_V2_ROOT} ${HAL_V2_INC}
                ${CMAKE_SYSROOT}/usr/include
                ${CMAKE_SYSROOT}/usr/include/alsa)
            target_link_libraries(hal-hailo15-audio PRIVATE
                hal-common
                ${HAL_V2_ASOUND_LIB}
                ${HAL_V2_AVCODEC_LIB}
                ${HAL_V2_AVUTIL_LIB}
                ${HAL_V2_SWRESAMPLE_LIB}
                pthread)
            target_compile_options(hal-hailo15-audio PRIVATE -Wall -Wextra -fPIC)
            set_target_properties(hal-hailo15-audio PROPERTIES
                VERSION ${PROJECT_VERSION}
                SOVERSION 2)
        endif()
        message(STATUS "HAL hailo15: audio enabled (ALSA + libavcodec)")
    else()
        if(HAL_V2_BUILD_MONOLITHIC)
            target_sources(hal PRIVATE platforms/stub/media/stub_audio_impl.c)
        else()
            add_library(hal-hailo15-audio SHARED platforms/stub/media/stub_audio_impl.c)
            target_include_directories(hal-hailo15-audio PRIVATE ${HAL_V2_INC})
            target_link_libraries(hal-hailo15-audio PRIVATE hal-common pthread)
        endif()
        message(WARNING "ALSA/libavcodec not found in sysroot; HAL audio will use stub implementation.")
    endif()

    hal_hailo15_plain_target(hal-hailo15-mcu platforms/hailo15/mcu/hailo15_mcu_impl.cpp)
    hal_hailo15_plain_target(hal-hailo15-io platforms/hailo15/io/hailo15_io_impl.cpp)

    # Model / AI components (inference + postprocess). These are buildable without medialib.
    set(_HAL_V2_HAILO15_MODEL_SOURCES
        platforms/hailo15/model/hailo15_inference_impl.cpp
        platforms/hailo15/model/hal_internal_ocr_paddle.cpp
        platforms/hailo15/model/hal_internal_yolov8_pose.cpp
        platforms/hailo15/model/hal_internal_scdepth.cpp
        platforms/hailo15/model/hailo15_postprocess_impl.cpp
    )
    if(HAL_V2_BUILD_MONOLITHIC)
        target_sources(hal PRIVATE ${_HAL_V2_HAILO15_MODEL_SOURCES})
        target_include_directories(hal PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
    else()
        add_library(hal-hailo15-model SHARED ${_HAL_V2_HAILO15_MODEL_SOURCES})
        target_include_directories(hal-hailo15-model PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
        target_link_libraries(hal-hailo15-model PRIVATE hal-common pthread)
        target_compile_options(hal-hailo15-model PRIVATE -Wall -Wextra -fPIC)
        set_target_properties(hal-hailo15-model PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION 2
        )
    endif()
    if(HAVE_HAILORT)
        if(HAL_V2_BUILD_MONOLITHIC)
            target_include_directories(hal PRIVATE ${HAILORT_INC_DIR})
            target_link_libraries(hal PRIVATE ${HAILORT_LIB})
            target_compile_definitions(hal PRIVATE HAL_HAVE_HAILORT=1)
        else()
            target_include_directories(hal-hailo15-model PRIVATE ${HAILORT_INC_DIR})
            target_link_libraries(hal-hailo15-model PRIVATE ${HAILORT_LIB})
            target_compile_definitions(hal-hailo15-model PRIVATE HAL_HAVE_HAILORT=1)
        endif()
    else()
        message(WARNING "HailoRT headers/lib not found; hal-hailo15-model will be built in stub mode.")
    endif()

    # GenAI (LLM/VLM): real impl when SDK headers present; otherwise stub OPS table.
    if(HAVE_HAL_GENAI)
        if(HAL_V2_BUILD_MONOLITHIC)
            target_sources(hal PRIVATE platforms/hailo15/model/hailo15_genai_impl.cpp)
            target_compile_definitions(hal PRIVATE HAL_HAVE_HAL_GENAI=1)
        else()
            target_sources(hal-hailo15-model PRIVATE platforms/hailo15/model/hailo15_genai_impl.cpp)
            target_compile_definitions(hal-hailo15-model PRIVATE HAL_HAVE_HAL_GENAI=1)
        endif()
    else()
        if(HAL_V2_BUILD_MONOLITHIC)
            target_sources(hal PRIVATE common/hal_genai_stub.c)
        else()
            target_sources(hal-hailo15-model PRIVATE common/hal_genai_stub.c)
        endif()
    endif()

    # CLIP text encoder + prompt scorer (compiled into model target unconditionally;
    # runtime checks is_ready() before use).
    if(HAL_V2_BUILD_MONOLITHIC)
        target_sources(hal PRIVATE
            common/hal_clip_text_encoder.cpp
            common/hal_clip_prompt_scorer.cpp
        )
    else()
        target_sources(hal-hailo15-model PRIVATE
            common/hal_clip_text_encoder.cpp
            common/hal_clip_prompt_scorer.cpp
        )
    endif()

    # Hailo15 postprocess is vendor-plugin-based; require postprocess tools headers when available.
    # If missing (e.g. host build), postprocess will be built as NOT_SUPPORTED stubs (no CPU fallback).
    find_path(HAILO_POSTPROCESS_TOOLS_INC_DIR
        NAMES hailo_postprocess_tools/objects/hailo_objects.hpp
        PATHS ${HAILO_ML_INC}
        NO_DEFAULT_PATH
    )
    find_path(CPPZMQ_INC_DIR
        NAMES zmq.hpp
        PATHS ${HAILO_ML_INC}
        NO_DEFAULT_PATH
    )
    set(HAVE_HAILO_POSTPROCESS_TOOLS OFF)
    if(HAILO_POSTPROCESS_TOOLS_INC_DIR AND CPPZMQ_INC_DIR)
        set(HAVE_HAILO_POSTPROCESS_TOOLS ON)
        if(HAL_V2_BUILD_MONOLITHIC)
            target_include_directories(hal PRIVATE ${HAILO_POSTPROCESS_TOOLS_INC_DIR} ${CPPZMQ_INC_DIR})
            target_compile_definitions(hal PRIVATE HAL_HAVE_HAILO_POSTPROCESS_TOOLS=1)
            target_link_libraries(hal PRIVATE dl)
        else()
            # hal_draw_hailo15.cpp is part of hal-common (not hal-hailo15-model); without this macro the
            # translation unit compiles the CPU fallback HAL_DRAW_OPS table even when SDK headers exist.
            target_include_directories(hal-common PRIVATE ${HAILO_POSTPROCESS_TOOLS_INC_DIR} ${CPPZMQ_INC_DIR})
            target_compile_definitions(hal-common PRIVATE HAL_HAVE_HAILO_POSTPROCESS_TOOLS=1)
            target_link_libraries(hal-common PRIVATE dl)
            target_include_directories(hal-hailo15-model PRIVATE ${HAILO_POSTPROCESS_TOOLS_INC_DIR} ${CPPZMQ_INC_DIR})
            target_compile_definitions(hal-hailo15-model PRIVATE HAL_HAVE_HAILO_POSTPROCESS_TOOLS=1)
            target_link_libraries(hal-hailo15-model PRIVATE dl)
        endif()
    else()
        message(WARNING "Hailo postprocess tools headers not found; hailo15 postprocess will be stubbed (NOT_SUPPORTED).")
    endif()

    # hal_draw_hailo15.cpp: HailoNV12Mat + OpenCV on Y/UV (aligns with V1 hal-hailo15-ml).
    # OpenCV: the SDK's OpenCV CMake package is partially broken (missing 3rdparty libs like
    # libcorrespondence.a), so we avoid find_package(OpenCV) and use include + plain opencv_* libs
    # (same pattern as hal/CMakeLists.txt).
    if(HAVE_HAILO_POSTPROCESS_TOOLS AND HAL_DRAW_BACKEND STREQUAL "hailo15")
        set(_HAL_DRAW_OPENCV_INCLUDE "")
        set(_HAL_DRAW_OPENCV_LIBS opencv_core opencv_imgproc)
        if(DEFINED ENV{OECORE_TARGET_SYSROOT})
            set(_HAL_DRAW_OPENCV_INCLUDE "$ENV{OECORE_TARGET_SYSROOT}/usr/include/opencv4")
        elseif(CMAKE_SYSROOT AND NOT CMAKE_SYSROOT STREQUAL "")
            set(_HAL_DRAW_OPENCV_INCLUDE "${CMAKE_SYSROOT}/usr/include/opencv4")
        endif()
        if(HAL_V2_BUILD_MONOLITHIC)
            target_include_directories(hal PRIVATE ${_HAL_DRAW_OPENCV_INCLUDE})
            target_link_libraries(hal PRIVATE ${_HAL_DRAW_OPENCV_LIBS})
        else()
            target_include_directories(hal-common PRIVATE ${_HAL_DRAW_OPENCV_INCLUDE})
            target_link_libraries(hal-common PRIVATE ${_HAL_DRAW_OPENCV_LIBS})
        endif()
    endif()

    hal_hailo15_plain_target(hal-hailo15-rtc platforms/hailo15/peripherals/hailo15_rtc_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-rtc PRIVATE hal-hailo15-mcu)
    endif()
    hal_hailo15_plain_target(hal-hailo15-led platforms/hailo15/peripherals/hailo15_led_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-led PRIVATE hal-hailo15-mcu)
    endif()
    hal_hailo15_plain_target(hal-hailo15-sensor platforms/hailo15/peripherals/hailo15_sensor_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-sensor PRIVATE hal-hailo15-mcu)
    endif()
    hal_hailo15_plain_target(hal-hailo15-alarm platforms/hailo15/peripherals/hailo15_alarm_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-alarm PRIVATE hal-hailo15-mcu)
    endif()
    hal_hailo15_plain_target(hal-hailo15-rs485 platforms/hailo15/peripherals/hailo15_rs485_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-rs485 PRIVATE hal-hailo15-mcu)
    endif()
    hal_hailo15_plain_target(hal-hailo15-lens platforms/hailo15/peripherals/hailo15_lens_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-lens PRIVATE hal-hailo15-mcu)
    endif()
    hal_hailo15_plain_target(hal-hailo15-env-ctrl platforms/hailo15/peripherals/hailo15_env_ctrl_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-env-ctrl PRIVATE hal-hailo15-mcu)
    endif()

    # libgpiod — hailo15_io_impl + hailo15_env_ctrl_impl reference gpiod_*; without this,
    # libaipc_hal.so keeps undefined symbols and linking hal-udp-stream-test / consumers fails.
    find_library(HAL_V2_GPIOD_LIB
        NAMES gpiod
        PATHS
            "${CMAKE_SYSROOT}/usr/lib"
            "${CMAKE_SYSROOT}/usr/lib64"
            "${CMAKE_SYSROOT}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT HAL_V2_GPIOD_LIB)
        find_library(HAL_V2_GPIOD_LIB NAMES gpiod)
    endif()
    if(HAL_V2_GPIOD_LIB)
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal PRIVATE ${HAL_V2_GPIOD_LIB})
        else()
            target_link_libraries(hal-hailo15-io PRIVATE ${HAL_V2_GPIOD_LIB})
            target_link_libraries(hal-hailo15-env-ctrl PRIVATE ${HAL_V2_GPIOD_LIB})
        endif()
        message(STATUS "HAL hailo15: libgpiod: ${HAL_V2_GPIOD_LIB}")
    else()
        message(FATAL_ERROR "libgpiod not found (hailo15 GPIO / MCU reset GPIO need it). Add libgpiod to the SDK sysroot or install libgpiod-dev on the host.")
    endif()

    hal_hailo15_plain_target(hal-hailo15-ota platforms/hailo15/peripherals/hailo15_ota_impl.cpp)
    if(NOT HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-hailo15-ota PRIVATE hal-hailo15-mcu)
    endif()

    # Optional: HailoDSP library for image processing (crop/resize, convert, blend, privacy mask).
    find_path(HAILODSP_INC_DIR
        NAMES hailo/hailodsp.h
        PATHS ${HAILO_ML_INC}
        NO_DEFAULT_PATH
    )
    find_library(HAILODSP_LIB
        NAMES hailodsp
        PATHS ${CMAKE_SYSROOT}/usr/lib ${CMAKE_SYSROOT}/usr/lib64 ${CMAKE_SYSROOT}/lib ${CMAKE_SYSROOT}/lib64
        NO_DEFAULT_PATH
    )
    set(HAVE_HAILODSP OFF)
    if(HAILODSP_INC_DIR AND HAILODSP_LIB)
        set(HAVE_HAILODSP ON)
    endif()

    # DSP image-processing component. Requires libhailodsp at runtime.
    if(HAVE_HAILODSP)
        if(HAL_V2_BUILD_MONOLITHIC)
            target_sources(hal PRIVATE platforms/hailo15/dsp/hailo15_dsp_impl.cpp)
            target_include_directories(hal PRIVATE
                ${HAL_V2_ROOT}
                ${HAL_V2_INC}
                ${HAILO_ML_INC}
                ${HAILODSP_INC_DIR}
            )
            target_link_libraries(hal PRIVATE ${HAILODSP_LIB})
        else()
            add_library(hal-hailo15-dsp SHARED
                platforms/hailo15/dsp/hailo15_dsp_impl.cpp
            )
            target_include_directories(hal-hailo15-dsp PRIVATE
                ${HAL_V2_ROOT}
                ${HAL_V2_INC}
                ${HAILO_ML_INC}
                ${HAILODSP_INC_DIR}
            )
            target_link_libraries(hal-hailo15-dsp PRIVATE
                hal-common
                ${HAILODSP_LIB}
                pthread
            )
            target_compile_options(hal-hailo15-dsp PRIVATE -Wall -Wextra -fPIC)
            set_target_properties(hal-hailo15-dsp PROPERTIES
                VERSION ${PROJECT_VERSION}
                SOVERSION 2
            )
        endif()
    else()
        message(WARNING "HailoDSP headers/lib not found; hal-hailo15-dsp will not be built.")
    endif()

    if(HAL_V2_BUILD_MONOLITHIC)
        target_sources(hal PRIVATE common/devices/hal_lens_af0832.c)
        target_include_directories(hal PUBLIC ${HAL_V2_INC})
    else()
        add_library(hal-devices SHARED
            common/devices/hal_lens_af0832.c
        )
        target_include_directories(hal-devices PUBLIC ${HAL_V2_INC})
        target_link_libraries(hal-devices PRIVATE hal-common pthread)
        target_compile_options(hal-devices PRIVATE -Wall -Wextra -fPIC)
        set_target_properties(hal-devices PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION 2
        )
    endif()

    # Lens bridge: thin C wrappers for Go dlopen (device-control).
    # Links against the monolithic libaipc_hal.so to resolve HAL_MCU_OPS / HAL_LENS_OPS.
    if(HAL_V2_BUILD_MONOLITHIC)
        add_library(hal-lens-bridge SHARED
            platforms/hailo15/bridge/hal_lens_bridge.c
        )
        target_include_directories(hal-lens-bridge PRIVATE ${HAL_V2_INC})
        target_link_libraries(hal-lens-bridge PRIVATE hal)
        set_target_properties(hal-lens-bridge PROPERTIES
            OUTPUT_NAME hal-lens-bridge
            VERSION ${PROJECT_VERSION}
            SOVERSION 1
            INSTALL_RPATH "\$ORIGIN"
        )
        install(TARGETS hal-lens-bridge LIBRARY DESTINATION /opt/aipc/lib/hal)
    endif()

    if(HAVE_HAILO_MEDIALIB)
        add_executable(hal-udp-stream-test examples/udp_stream_test/udp_stream_test.cpp)
        target_include_directories(hal-udp-stream-test PRIVATE ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-udp-stream-test PRIVATE hal)
        else()
            target_link_libraries(hal-udp-stream-test PRIVATE
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-hailo15-isp
                hal-common
            )
        endif()
        target_compile_options(hal-udp-stream-test PRIVATE -Wall -Wextra -fPIC)

        add_executable(hal-video-test-sub-v2 examples/video_test_sub_v2/video_test_sub_v2.cpp)
        target_include_directories(hal-video-test-sub-v2 PRIVATE
            ${HAL_V2_INC}
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/hailo15/media
            ${HAILO_ML_INC}
        )
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-video-test-sub-v2 PRIVATE hal)
        else()
            target_link_libraries(hal-video-test-sub-v2 PRIVATE
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-hailo15-isp
                hal-common
            )
        endif()
        target_compile_options(hal-video-test-sub-v2 PRIVATE -Wall -Wextra -fPIC)

        add_executable(hal-test-media-all-func examples/test_media_all_func/test_media_all_func.cpp)
        target_include_directories(hal-test-media-all-func PRIVATE ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-test-media-all-func PRIVATE hal)
        else()
            target_link_libraries(hal-test-media-all-func PRIVATE
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-hailo15-isp
                hal-hailo15-osd
                hal-hailo15-mcu
                hal-hailo15-io
                hal-hailo15-rtc
                hal-hailo15-led
                hal-hailo15-sensor
                hal-hailo15-alarm
                hal-hailo15-rs485
                hal-hailo15-lens
                hal-hailo15-env-ctrl
                hal-devices
                hal-common
            )
        endif()
        target_compile_options(hal-test-media-all-func PRIVATE -Wall -Wextra -fPIC)

        add_executable(hal-auto-af-test examples/auto_af_test/auto_af_test.cpp)
        target_include_directories(hal-auto-af-test PRIVATE ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-auto-af-test PRIVATE hal)
        else()
            target_link_libraries(hal-auto-af-test PRIVATE
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-hailo15-isp
                hal-hailo15-osd
                hal-hailo15-mcu
                hal-hailo15-lens
                hal-devices
                hal-common
            )
        endif()
        target_compile_options(hal-auto-af-test PRIVATE -Wall -Wextra -fPIC)
    endif()

    if(HAVE_HAILODSP)
        # NOTE: examples/dsp_test was removed from this repo; keep DSP validation via dsp_media_udp_resize_test.
    endif()

    if(HAVE_HAILODSP AND HAVE_HAILO_MEDIALIB)
        # Optional: Tokenizers (used for CLIP text encoder on-device prompt encoding).
        # SDK ships static libs (libtokenizers_cpp.a, libtokenizers_c.a) under the cross sysroot — prefer
        # those so the final hal-ai-example-v2 does not need libtokenizers*.so on the target rootfs.
        find_path(TOKENIZERS_INC_DIR
            NAMES tokenizers_cpp.h
            PATHS ${HAILO_ML_INC}
            NO_DEFAULT_PATH
        )
        set(_TOKENIZERS_LIB_PATHS "")
        if(EXISTS "${_LOCAL_SDK_SYSROOT}/usr/include/hailo/media_library/media_library.hpp")
            list(APPEND _TOKENIZERS_LIB_PATHS
                "${_LOCAL_SDK_SYSROOT}/usr/lib"
                "${_LOCAL_SDK_SYSROOT}/usr/lib64"
            )
        endif()
        if(EXISTS "${_LOCAL_SDK_SYSROOT2}/usr/include/hailo/media_library/media_library.hpp")
            list(APPEND _TOKENIZERS_LIB_PATHS
                "${_LOCAL_SDK_SYSROOT2}/usr/lib"
                "${_LOCAL_SDK_SYSROOT2}/usr/lib64"
            )
        endif()
        if(EXISTS "${_LOCAL_SDK_SYSROOT3}/usr/include/hailo/media_library/media_library.hpp")
            list(APPEND _TOKENIZERS_LIB_PATHS
                "${_LOCAL_SDK_SYSROOT3}/usr/lib"
                "${_LOCAL_SDK_SYSROOT3}/usr/lib64"
            )
        endif()
        if(DEFINED CMAKE_SYSROOT AND NOT CMAKE_SYSROOT STREQUAL "")
            list(APPEND _TOKENIZERS_LIB_PATHS
                "${CMAKE_SYSROOT}/usr/lib"
                "${CMAKE_SYSROOT}/usr/lib64"
                "${CMAKE_SYSROOT}/lib"
                "${CMAKE_SYSROOT}/lib64"
            )
        endif()
        set(_SAVE_TOK_LIB_SUFFIXES "${CMAKE_FIND_LIBRARY_SUFFIXES}")
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".so")
        find_library(TOKENIZERS_CPP_LIB
            NAMES tokenizers_cpp
            PATHS ${_TOKENIZERS_LIB_PATHS}
            NO_DEFAULT_PATH
        )
        find_library(TOKENIZERS_C_LIB
            NAMES tokenizers_c
            PATHS ${_TOKENIZERS_LIB_PATHS}
            NO_DEFAULT_PATH
        )
        set(CMAKE_FIND_LIBRARY_SUFFIXES "${_SAVE_TOK_LIB_SUFFIXES}")
        set(HAVE_TOKENIZERS OFF)
        if(TOKENIZERS_INC_DIR AND TOKENIZERS_CPP_LIB AND TOKENIZERS_C_LIB)
            set(HAVE_TOKENIZERS ON)
            message(STATUS "Tokenizers (CLIP text): include=${TOKENIZERS_INC_DIR} cpp=${TOKENIZERS_CPP_LIB} c=${TOKENIZERS_C_LIB}")
        endif()

        add_executable(hal-dsp-media-udp-resize-test
            examples/dsp_media_udp_resize_test/dsp_media_udp_resize_test.cpp
        )
        target_include_directories(hal-dsp-media-udp-resize-test PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-dsp-media-udp-resize-test PRIVATE hal)
        else()
            target_link_libraries(hal-dsp-media-udp-resize-test PRIVATE
                hal-hailo15-dsp
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-common
            )
        endif()
        target_compile_options(hal-dsp-media-udp-resize-test PRIVATE -Wall -Wextra -fPIC)

        add_executable(hal-ai-example-v2
            examples/ai_example_v2/ai_example_v2.cpp
            common/hal_clip_text_encoder.cpp
            common/hal_clip_prompt_scorer.cpp
        )
        target_include_directories(hal-ai-example-v2 PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-ai-example-v2 PRIVATE hal)
        else()
            target_link_libraries(hal-ai-example-v2 PRIVATE
                hal-hailo15-dsp
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-hailo15-isp
                hal-hailo15-model
                hal-common
            )
        endif()
        if(HAVE_TOKENIZERS)
            target_include_directories(hal-ai-example-v2 PRIVATE ${TOKENIZERS_INC_DIR})
            target_link_libraries(hal-ai-example-v2 PRIVATE ${TOKENIZERS_CPP_LIB} ${TOKENIZERS_C_LIB})
            target_compile_definitions(hal-ai-example-v2 PRIVATE HAL_HAVE_TOKENIZERS=1)
        else()
            message(WARNING "Tokenizers not found; hal-ai-example-v2 will not support on-device CLIP text encoding.")
        endif()
        # Also link tokenizers into the shared library so ai-runtime can use EncodeText.
        if(HAVE_TOKENIZERS)
            if(HAL_V2_BUILD_MONOLITHIC)
                target_include_directories(hal PRIVATE ${TOKENIZERS_INC_DIR})
                target_link_libraries(hal PRIVATE ${TOKENIZERS_CPP_LIB} ${TOKENIZERS_C_LIB})
                target_compile_definitions(hal PRIVATE HAL_HAVE_TOKENIZERS=1)
            else()
                target_include_directories(hal-hailo15-model PRIVATE ${TOKENIZERS_INC_DIR})
                target_link_libraries(hal-hailo15-model PRIVATE ${TOKENIZERS_CPP_LIB} ${TOKENIZERS_C_LIB})
                target_compile_definitions(hal-hailo15-model PRIVATE HAL_HAVE_TOKENIZERS=1)
            endif()
        endif()
        target_compile_options(hal-ai-example-v2 PRIVATE -Wall -Wextra -fPIC)

        add_executable(hal-ocr-example-v2 examples/ocr_example_v2/ocr_example_v2.cpp)
        target_include_directories(hal-ocr-example-v2 PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-ocr-example-v2 PRIVATE hal)
        else()
            target_link_libraries(hal-ocr-example-v2 PRIVATE
                hal-hailo15-dsp
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-hailo15-isp
                hal-hailo15-model
                hal-common
            )
        endif()
        target_compile_options(hal-ocr-example-v2 PRIVATE -Wall -Wextra -fPIC)

        add_executable(hal-lpr-example-v2 examples/lpr_example_v2/lpr_example_v2.cpp)
        target_include_directories(hal-lpr-example-v2 PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-lpr-example-v2 PRIVATE hal)
        else()
            target_link_libraries(hal-lpr-example-v2 PRIVATE
                hal-hailo15-dsp
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-codec
                hal-hailo15-isp
                hal-hailo15-model
                hal-common
            )
        endif()
        target_compile_options(hal-lpr-example-v2 PRIVATE -Wall -Wextra -fPIC)

        add_executable(hal-parallel-infer-example-v2
            examples/parallel_infer_example_v2/parallel_infer_example_v2.cpp
        )
        target_include_directories(hal-parallel-infer-example-v2 PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-parallel-infer-example-v2 PRIVATE hal pthread)
        else()
            target_link_libraries(hal-parallel-infer-example-v2 PRIVATE
                hal-hailo15-dsp
                hal-hailo15-media
                hal-hailo15-video
                hal-hailo15-model
                hal-common
                pthread
            )
        endif()
        target_compile_options(hal-parallel-infer-example-v2 PRIVATE -Wall -Wextra -fPIC)
    endif()

    add_executable(hal-test-peripheral-all-func examples/test_peripheral_all_func/test_peripheral_all_func.cpp)
    target_include_directories(hal-test-peripheral-all-func PRIVATE ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-test-peripheral-all-func PRIVATE hal)
    else()
        target_link_libraries(hal-test-peripheral-all-func PRIVATE
            hal-hailo15-mcu
            hal-hailo15-io
            hal-hailo15-rtc
            hal-hailo15-led
            hal-hailo15-sensor
            hal-hailo15-alarm
            hal-hailo15-rs485
            hal-hailo15-lens
            hal-hailo15-env-ctrl
            hal-devices
            hal-common
        )
    endif()
    target_compile_options(hal-test-peripheral-all-func PRIVATE -Wall -Wextra -fPIC)

    add_executable(hal-inference-perf-sample examples/inference_perf_sample/inference_perf_sample.cpp)
    target_include_directories(hal-inference-perf-sample PRIVATE ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-inference-perf-sample PRIVATE hal)
    else()
        target_link_libraries(hal-inference-perf-sample PRIVATE hal-hailo15-model hal-common pthread)
    endif()
    target_compile_options(hal-inference-perf-sample PRIVATE -Wall -Wextra -fPIC)

    add_executable(hal-audio-test examples/audio_test/hal_audio_test.cpp)
    target_include_directories(hal-audio-test PRIVATE ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-audio-test PRIVATE hal pthread)
    else()
        target_link_libraries(hal-audio-test PRIVATE hal-hailo15-audio hal-common pthread)
    endif()
    target_compile_options(hal-audio-test PRIVATE -Wall -Wextra -fPIC)

    if(HAVE_HAL_GENAI AND HAVE_HAILORT)
        add_executable(hal-genai-example-v2
            examples/genai_example_v2/genai_example_v2.cpp
            examples/genai_example_v2/image_decode_resize.cpp
        )
        target_include_directories(hal-genai-example-v2 PRIVATE
            ${HAL_V2_ROOT}
            ${HAL_V2_INC}
            ${HAILORT_INC_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
        )
        if(HAL_V2_BUILD_MONOLITHIC)
            target_link_libraries(hal-genai-example-v2 PRIVATE hal pthread)
        else()
            target_link_libraries(hal-genai-example-v2 PRIVATE hal-hailo15-model hal-common pthread)
        endif()
        target_compile_options(hal-genai-example-v2 PRIVATE -Wall -Wextra -fPIC)
    endif()

    add_executable(hal-ota-test-v2 examples/ota_test_v2/ota_test_v2.cpp)
    target_include_directories(hal-ota-test-v2 PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-ota-test-v2 PRIVATE hal)
    else()
        target_link_libraries(hal-ota-test-v2 PRIVATE hal-hailo15-ota)
    endif()
    target_compile_options(hal-ota-test-v2 PRIVATE -Wall -Wextra -fPIC)

    add_executable(ne503_boot_prep examples/ne503_boot_prep/ne503_boot_prep.cpp)
    target_include_directories(ne503_boot_prep PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(ne503_boot_prep PRIVATE hal)
    else()
        target_link_libraries(ne503_boot_prep PRIVATE hal-hailo15-ota)
    endif()
    target_compile_options(ne503_boot_prep PRIVATE -Wall -Wextra -fPIC)
elseif(HAL_PLATFORM STREQUAL "stub")
    # platforms/stub mirrors platforms/hailo15 layout: media, model, dsp, mcu, io, peripherals.
    set(_HAL_V2_STUB_SOURCES
        platforms/stub/media/stub_media_impl.c
        platforms/stub/media/stub_video_impl.c
        platforms/stub/media/stub_codec_impl.c
        platforms/stub/media/stub_isp_impl.c
        platforms/stub/media/stub_osd_impl.c
        platforms/stub/media/stub_audio_impl.c
        platforms/stub/model/stub_inference_impl.c
        platforms/stub/model/stub_postprocess_impl.c
        platforms/stub/dsp/stub_dsp_impl.c
        platforms/stub/mcu/stub_mcu_impl.c
        platforms/stub/io/stub_io_impl.c
        platforms/stub/peripherals/stub_led_impl.c
        platforms/stub/peripherals/stub_rtc_impl.c
        platforms/stub/peripherals/stub_sensor_impl.c
        platforms/stub/peripherals/stub_alarm_impl.c
        platforms/stub/peripherals/stub_rs485_impl.c
        platforms/stub/peripherals/stub_lens_impl.c
        platforms/stub/peripherals/stub_env_ctrl_impl.c
        platforms/stub/peripherals/stub_ota_impl.c
    )
    if(HAL_V2_BUILD_MONOLITHIC)
        target_sources(hal PRIVATE ${_HAL_V2_STUB_SOURCES})
        target_sources(hal PRIVATE common/hal_genai_stub.c)
    else()
        target_sources(hal-common PRIVATE ${_HAL_V2_STUB_SOURCES})
        target_sources(hal-common PRIVATE common/hal_genai_stub.c)
    endif()

    add_executable(hal-inference-perf-sample examples/inference_perf_sample/inference_perf_sample.cpp)
    target_include_directories(hal-inference-perf-sample PRIVATE ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-inference-perf-sample PRIVATE hal)
    else()
        target_link_libraries(hal-inference-perf-sample PRIVATE hal-common pthread)
    endif()
    target_compile_options(hal-inference-perf-sample PRIVATE -Wall -Wextra -fPIC)

    add_executable(hal-audio-test examples/audio_test/hal_audio_test.cpp)
    target_include_directories(hal-audio-test PRIVATE ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-audio-test PRIVATE hal pthread)
    else()
        target_link_libraries(hal-audio-test PRIVATE hal-common pthread)
    endif()
    target_compile_options(hal-audio-test PRIVATE -Wall -Wextra -fPIC)

    add_executable(hal-ota-test-v2 examples/ota_test_v2/ota_test_v2.cpp)
    target_include_directories(hal-ota-test-v2 PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(hal-ota-test-v2 PRIVATE hal)
    else()
        target_link_libraries(hal-ota-test-v2 PRIVATE hal-common pthread)
    endif()
    target_compile_options(hal-ota-test-v2 PRIVATE -Wall -Wextra -fPIC)

    add_executable(ne503_boot_prep examples/ne503_boot_prep/ne503_boot_prep.cpp)
    target_include_directories(ne503_boot_prep PRIVATE ${HAL_V2_ROOT} ${HAL_V2_INC})
    if(HAL_V2_BUILD_MONOLITHIC)
        target_link_libraries(ne503_boot_prep PRIVATE hal)
    else()
        target_link_libraries(ne503_boot_prep PRIVATE hal-common pthread)
    endif()
    target_compile_options(ne503_boot_prep PRIVATE -Wall -Wextra -fPIC)
endif()

cmake_minimum_required(VERSION 3.10)
project(camera-daemon LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Always keep debug symbols for crash analysis (minimal impact on performance)
add_compile_options(-g)

# Sanitizer options (enable with -DENABLE_ASAN=ON or -DENABLE_TSAN=ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# HAL v2 (only version supported)
set(HAL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../hal_v2/include)
set(HAL_V2_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../hal_v2/include)
set(HAL_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../hal_v2/common)
set(AUTO_AF_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../hal_v2/examples/AF_demo/core)
set(HAL_COMMON_SOURCES
    ${HAL_COMMON_DIR}/hal_common.c
    ${HAL_COMMON_DIR}/hal_log.c
    ${HAL_COMMON_DIR}/crash_handler.c
    ${HAL_COMMON_DIR}/hal_postprocess_common.c
    ${HAL_COMMON_DIR}/hal_draw_cpu.cpp
    ${HAL_COMMON_DIR}/hal_draw_ops_cpu.cpp
)
# v2 headers are in subdirectories: media/, common/, model/, peripheral/
# Each header uses relative includes (e.g. "../common/hal_common.h")
set(HAL_V2_INCLUDE_DIRS
    ${HAL_V2_INC}/media
    ${HAL_V2_INC}/common
    ${HAL_V2_INC}/model
    ${HAL_V2_INC}/peripheral
    ${HAL_V2_INC}/peripheral/devices
    ${HAL_V2_INC}/dsp
)
message(STATUS "camera-daemon: using HAL v2 (include=${HAL_INCLUDE_DIR})")

# ── Protobuf + gRPC (for AI overlay Event Bus subscription) ──
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ai-runtime/proto)
set(PROTO_GEN_ARM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ai-runtime/proto_gen_arm")
set(CAMERA_PROTO_GEN_ARM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto_gen_arm")
set(HAS_GRPC FALSE)

# Cross-compiling: use pre-generated proto files + target grpc++ lib
if(CMAKE_CROSSCOMPILING AND EXISTS "${PROTO_GEN_ARM_DIR}/event.pb.cc")
    message(STATUS "Using pre-generated proto files from ${PROTO_GEN_ARM_DIR}")
    find_package(Protobuf QUIET)
    find_library(GRPC_LIB grpc++)
    find_library(GRPC_BASE_LIB grpc)

    # Fallback for cross-compilation if find_package failed or library not found by name
    if(CMAKE_CROSSCOMPILING AND NOT Protobuf_FOUND)
        find_path(Protobuf_INCLUDE_DIRS google/protobuf/message.h)
        find_library(Protobuf_LIBRARIES protobuf)
        if(Protobuf_INCLUDE_DIRS AND Protobuf_LIBRARIES)
            set(Protobuf_FOUND TRUE)
            message(STATUS "Manually found Protobuf: ${Protobuf_LIBRARIES}")
        endif()
    endif()

    if(Protobuf_FOUND AND GRPC_LIB)
        set(PROTO_GEN_DIR ${PROTO_GEN_ARM_DIR})
        set(GRPC_PROTO_SOURCES
            ${PROTO_GEN_DIR}/event.pb.cc
            ${PROTO_GEN_DIR}/event.grpc.pb.cc
            ${CAMERA_PROTO_GEN_ARM_DIR}/camera.pb.cc
            ${CAMERA_PROTO_GEN_ARM_DIR}/camera.grpc.pb.cc
            ${CAMERA_PROTO_GEN_ARM_DIR}/lens_hal.pb.cc
            ${CAMERA_PROTO_GEN_ARM_DIR}/lens_hal.grpc.pb.cc
        )
        set(HAS_GRPC TRUE)
        message(STATUS "Event Bus subscription enabled for AiOverlaySubscriber (ARM)")
    else()
        message(STATUS "gRPC/Protobuf libs not found in sysroot — AI overlay disabled (Protobuf_FOUND=${Protobuf_FOUND}, GRPC_LIB=${GRPC_LIB})")
        set(GRPC_PROTO_SOURCES "")
    endif()
else()
    # Native build: generate proto at build time
    find_package(Protobuf QUIET)
    find_package(gRPC CONFIG QUIET)

    set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
    file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

    if(NOT gRPC_FOUND)
        find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin)
        find_library(GRPC_LIB grpc++)
        if(_GRPC_CPP_PLUGIN AND GRPC_LIB)
            set(GRPC_FOUND_MANUAL TRUE)
        endif()
    endif()

    if(Protobuf_FOUND AND (gRPC_FOUND OR GRPC_FOUND_MANUAL))
        if(gRPC_FOUND)
            set(_CD_GRPC_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
            set(_CD_PROTOC $<TARGET_FILE:protobuf::protoc>)
        else()
            set(_CD_GRPC_PLUGIN ${_GRPC_CPP_PLUGIN})
            set(_CD_PROTOC ${Protobuf_PROTOC_EXECUTABLE})
        endif()

        set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../event-bus/proto/event.proto)
        set(EVENT_PB_CC  ${PROTO_GEN_DIR}/event.pb.cc)
        set(EVENT_PB_H   ${PROTO_GEN_DIR}/event.pb.h)
        set(EVENT_GRPC_CC ${PROTO_GEN_DIR}/event.grpc.pb.cc)
        set(EVENT_GRPC_H  ${PROTO_GEN_DIR}/event.grpc.pb.h)

        add_custom_command(
            OUTPUT ${EVENT_PB_CC} ${EVENT_PB_H} ${EVENT_GRPC_CC} ${EVENT_GRPC_H}
            COMMAND ${_CD_PROTOC}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/../event-bus/proto
                --cpp_out=${PROTO_GEN_DIR}
                --grpc_out=${PROTO_GEN_DIR}
                --plugin=protoc-gen-grpc=${_CD_GRPC_PLUGIN}
                ${PROTO_FILE}
            DEPENDS ${PROTO_FILE}
            COMMENT "Generating Event Bus proto for camera-daemon"
        )

        set(GRPC_PROTO_SOURCES ${EVENT_PB_CC} ${EVENT_GRPC_CC})

        # Also generate camera.proto (for gRPC CameraControlService)
        set(CAMERA_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/camera.proto)
        set(CAMERA_PB_CC  ${PROTO_GEN_DIR}/camera.pb.cc)
        set(CAMERA_PB_H   ${PROTO_GEN_DIR}/camera.pb.h)
        set(CAMERA_GRPC_CC ${PROTO_GEN_DIR}/camera.grpc.pb.cc)
        set(CAMERA_GRPC_H  ${PROTO_GEN_DIR}/camera.grpc.pb.h)

        add_custom_command(
            OUTPUT ${CAMERA_PB_CC} ${CAMERA_PB_H} ${CAMERA_GRPC_CC} ${CAMERA_GRPC_H}
            COMMAND ${_CD_PROTOC}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
                --experimental_allow_proto3_optional
                --cpp_out=${PROTO_GEN_DIR}
                --grpc_out=${PROTO_GEN_DIR}
                --plugin=protoc-gen-grpc=${_CD_GRPC_PLUGIN}
                ${CAMERA_PROTO_FILE}
            DEPENDS ${CAMERA_PROTO_FILE}
            COMMENT "Generating Camera proto for camera-daemon"
        )

        list(APPEND GRPC_PROTO_SOURCES ${CAMERA_PB_CC} ${CAMERA_GRPC_CC})
        set(HAS_GRPC TRUE)

        # Also generate lens_hal.proto (for gRPC LensHAL service)
        set(LENS_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/lens_hal.proto)
        set(LENS_PB_CC  ${PROTO_GEN_DIR}/lens_hal.pb.cc)
        set(LENS_PB_H   ${PROTO_GEN_DIR}/lens_hal.pb.h)
        set(LENS_GRPC_CC ${PROTO_GEN_DIR}/lens_hal.grpc.pb.cc)
        set(LENS_GRPC_H  ${PROTO_GEN_DIR}/lens_hal.grpc.pb.h)

        add_custom_command(
            OUTPUT ${LENS_PB_CC} ${LENS_PB_H} ${LENS_GRPC_CC} ${LENS_GRPC_H}
            COMMAND ${_CD_PROTOC}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
                --cpp_out=${PROTO_GEN_DIR}
                --grpc_out=${PROTO_GEN_DIR}
                --plugin=protoc-gen-grpc=${_CD_GRPC_PLUGIN}
                ${LENS_PROTO_FILE}
            DEPENDS ${LENS_PROTO_FILE}
            COMMENT "Generating LensHAL proto for camera-daemon"
        )

        list(APPEND GRPC_PROTO_SOURCES ${LENS_PB_CC} ${LENS_GRPC_CC})
    else()
        message(STATUS "gRPC/Protobuf not found — AI overlay Event Bus subscription disabled")
        set(GRPC_PROTO_SOURCES "")
    endif()
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/hal_loader.cpp
    src/video_source.cpp
    src/frame_router.cpp
    src/frame_watchdog.cpp
    src/osd_manager.cpp
    src/encoder_manager.cpp
    src/fd_publisher.cpp
    src/rtsp_server.cpp
    src/encoded_publisher.cpp
    src/ai_overlay_subscriber.cpp
    src/audio_service.cpp
    src/dpm_worker.cpp
    src/autofocus_controller.cpp
    src/camera_control_service.cpp
    src/lens_hal_service.cpp
    src/camera_daemon.cpp
    ${AUTO_AF_CORE_DIR}/af_calibration.cpp
    ${AUTO_AF_CORE_DIR}/af_follow.cpp
    ${AUTO_AF_CORE_DIR}/af_metric.cpp
    ${AUTO_AF_CORE_DIR}/af_scan.cpp
)

# Check if HAL common sources exist
foreach(src ${HAL_COMMON_SOURCES})
    if(NOT EXISTS ${src})
        message(STATUS "HAL common source not found: ${src}, will link dynamically")
        set(HAL_COMMON_SOURCES "")
        break()
    endif()
endforeach()

add_executable(camera-daemon ${SOURCES} ${HAL_COMMON_SOURCES} ${GRPC_PROTO_SOURCES})

target_include_directories(camera-daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HAL_INCLUDE_DIR}
    ${HAL_V2_INCLUDE_DIRS}
    ${PROTO_GEN_DIR}
    ${CAMERA_PROTO_GEN_ARM_DIR}
    ${AUTO_AF_CORE_DIR}
)

# Export symbols so dlopen'd HAL .so can resolve hal_log_message etc.
target_link_options(camera-daemon PRIVATE -rdynamic)

# Link libraries
target_link_libraries(camera-daemon PRIVATE
    dl          # dlopen/dlsym
    pthread     # std::thread, std::mutex
    rt          # shm_open (optional)
)

if(HAS_GRPC)
    if(gRPC_FOUND)
        target_link_libraries(camera-daemon PRIVATE
            gRPC::grpc++
            protobuf::libprotobuf
        )
    else()
        set(_GRPC_LINK_LIBS ${GRPC_LIB} ${Protobuf_LIBRARIES})
        if(GRPC_BASE_LIB)
            list(APPEND _GRPC_LINK_LIBS ${GRPC_BASE_LIB})
        endif()
        target_link_libraries(camera-daemon PRIVATE ${_GRPC_LINK_LIBS})
        target_include_directories(camera-daemon PRIVATE ${Protobuf_INCLUDE_DIRS})
    endif()
    target_compile_definitions(camera-daemon PRIVATE HAS_GRPC=1)
endif()

# Install
install(TARGETS camera-daemon DESTINATION /opt/aipc/bin)

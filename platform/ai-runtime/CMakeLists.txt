cmake_minimum_required(VERSION 3.14)
project(ai-runtime LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# ── Options ──────────────────────────────────────────────────────────────────
option(WITH_GRPC      "Build with gRPC support"          ON)
option(WITH_EVENT_BUS "Build with Event Bus client"      ON)

# ── Find packages ────────────────────────────────────────────────────────────
if(WITH_GRPC)
    # Cross-compiling: use QUIET and manually find libraries in sysroot
    # The host protoc version may not match target protobuf library version
    find_package(Protobuf QUIET)

    # When cross-compiling, gRPC CONFIG from sysroot won't have host plugin targets.
    # Always find the host grpc_cpp_plugin first, then fall back to gRPC CONFIG.
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

    if(NOT CMAKE_CROSSCOMPILING)
        find_package(gRPC CONFIG QUIET)
    endif()

    if(NOT gRPC_FOUND)
        find_library(GRPC_LIB grpc++)
        find_library(GRPC_BASE_LIB grpc)
        find_library(GRPC_REFLECTION_LIB grpc++_reflection)

        # For cross-compiling, manually set protobuf vars if find_package failed
        if(CMAKE_CROSSCOMPILING AND NOT Protobuf_FOUND)
            find_path(Protobuf_INCLUDE_DIRS google/protobuf/message.h)
            find_library(Protobuf_LIBRARIES protobuf)
            find_library(Protobuf_LITE_LIBRARIES protobuf-lite)
            if(Protobuf_INCLUDE_DIRS AND Protobuf_LIBRARIES)
                set(Protobuf_FOUND TRUE)
                message(STATUS "Manually found Protobuf: ${Protobuf_LIBRARIES}")
            endif()
        endif()

        if(NOT GRPC_CPP_PLUGIN OR NOT GRPC_LIB)
            message(FATAL_ERROR "gRPC not found. Install libgrpc++-dev or set gRPC_DIR.")
        endif()
        message(STATUS "Using manual gRPC: plugin=${GRPC_CPP_PLUGIN}, lib=${GRPC_LIB}")
    endif()
endif()

find_package(Threads REQUIRED)

# ── Proto generation ─────────────────────────────────────────────────────────
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)

# When cross-compiling, use pre-generated proto files to avoid host/target
# protoc version mismatch. Generate with: ./scripts/generate_proto_arm.sh
set(PROTO_GEN_ARM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto_gen_arm")

if(CMAKE_CROSSCOMPILING AND EXISTS "${PROTO_GEN_ARM_DIR}/inference.pb.cc")
    message(STATUS "Using pre-generated proto files from ${PROTO_GEN_ARM_DIR}")
    set(PROTO_GEN_DIR ${PROTO_GEN_ARM_DIR})
    set(INFERENCE_PB_CC  ${PROTO_GEN_DIR}/inference.pb.cc)
    set(INFERENCE_GRPC_CC ${PROTO_GEN_DIR}/inference.grpc.pb.cc)
    set(EVENT_PB_CC  ${PROTO_GEN_DIR}/event.pb.cc)
    set(EVENT_GRPC_CC ${PROTO_GEN_DIR}/event.grpc.pb.cc)
else()
    set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
    file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

    if(gRPC_FOUND AND NOT CMAKE_CROSSCOMPILING)
        set(_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
        set(_PROTOC $<TARGET_FILE:protobuf::protoc>)
    else()
        set(_GRPC_CPP_PLUGIN ${GRPC_CPP_PLUGIN})
        if(Protobuf_PROTOC_EXECUTABLE)
            set(_PROTOC ${Protobuf_PROTOC_EXECUTABLE})
        else()
            find_program(_PROTOC protoc REQUIRED)
        endif()
    endif()

    # inference.proto
    set(INFERENCE_PROTO ${PROTO_DIR}/inference.proto)
    set(INFERENCE_PB_CC  ${PROTO_GEN_DIR}/inference.pb.cc)
    set(INFERENCE_PB_H   ${PROTO_GEN_DIR}/inference.pb.h)
    set(INFERENCE_GRPC_CC ${PROTO_GEN_DIR}/inference.grpc.pb.cc)
    set(INFERENCE_GRPC_H  ${PROTO_GEN_DIR}/inference.grpc.pb.h)

    add_custom_command(
        OUTPUT ${INFERENCE_PB_CC} ${INFERENCE_PB_H} ${INFERENCE_GRPC_CC} ${INFERENCE_GRPC_H}
        COMMAND ${_PROTOC}
            --proto_path=${PROTO_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
            ${INFERENCE_PROTO}
        DEPENDS ${INFERENCE_PROTO}
        COMMENT "Generating C++ protobuf/gRPC for inference.proto"
    )

    # event.proto
    set(EVENT_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/../event-bus/proto/event.proto)
    set(EVENT_PB_CC  ${PROTO_GEN_DIR}/event.pb.cc)
    set(EVENT_PB_H   ${PROTO_GEN_DIR}/event.pb.h)
    set(EVENT_GRPC_CC ${PROTO_GEN_DIR}/event.grpc.pb.cc)
    set(EVENT_GRPC_H  ${PROTO_GEN_DIR}/event.grpc.pb.h)

        add_custom_command(
            OUTPUT ${EVENT_PB_CC} ${EVENT_PB_H} ${EVENT_GRPC_CC} ${EVENT_GRPC_H}
            COMMAND ${_PROTOC}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/../event-bus/proto
                --cpp_out=${PROTO_GEN_DIR}
                --grpc_out=${PROTO_GEN_DIR}
                --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
                ${EVENT_PROTO}
            DEPENDS ${EVENT_PROTO}
            COMMENT "Generating C++ protobuf/gRPC for event.proto"
        )
endif()

# ── Proto library ────────────────────────────────────────────────────────────
add_library(ai_runtime_proto STATIC
    ${INFERENCE_PB_CC} ${INFERENCE_GRPC_CC}
    ${EVENT_PB_CC} ${EVENT_GRPC_CC}
)
target_include_directories(ai_runtime_proto PUBLIC ${PROTO_GEN_DIR})
if(gRPC_FOUND AND NOT CMAKE_CROSSCOMPILING)
    target_link_libraries(ai_runtime_proto PUBLIC
        gRPC::grpc++ protobuf::libprotobuf)
else()
    find_library(GRPC_BASE_LIB grpc)
    set(_PROTO_LINK_LIBS ${GRPC_LIB} ${Protobuf_LIBRARIES})
    if(GRPC_BASE_LIB)
        list(APPEND _PROTO_LINK_LIBS ${GRPC_BASE_LIB})
    endif()
    target_link_libraries(ai_runtime_proto PUBLIC ${_PROTO_LINK_LIBS})
    target_include_directories(ai_runtime_proto PUBLIC ${Protobuf_INCLUDE_DIRS})
endif()

# ── HAL v2 (only version supported) ──────────────────────────────────────────
set(HAL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../hal_v2/include)
set(HAL_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../hal_v2/common)
set(HAL_COMMON_SOURCES
    ${HAL_COMMON_DIR}/hal_common.c
    ${HAL_COMMON_DIR}/hal_log.c
    ${HAL_COMMON_DIR}/hal_postprocess_common.c
)
message(STATUS "ai-runtime: using HAL v2 (include=${HAL_INCLUDE_DIR})")

foreach(src ${HAL_COMMON_SOURCES})
    if(NOT EXISTS ${src})
        message(STATUS "HAL common source not found: ${src}, will link dynamically")
        set(HAL_COMMON_SOURCES "")
        break()
    endif()
endforeach()

# ── Main executable ──────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/common.cpp
    src/hal_ml_loader.cpp
    src/model_manager.cpp
    src/session_manager.cpp
    src/inference_scheduler.cpp
    src/postprocess_pool.cpp
    src/fd_receiver.cpp
    src/event_bus_client.cpp
    src/config.cpp
    src/grpc_service.cpp
    src/auto_infer.cpp
)

add_executable(ai-runtime ${SOURCES} ${HAL_COMMON_SOURCES})

target_include_directories(ai-runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HAL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}/common
    ${HAL_INCLUDE_DIR}/media
    ${HAL_INCLUDE_DIR}/model
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/camera-daemon/include
)

target_link_options(ai-runtime PRIVATE -rdynamic)

target_link_libraries(ai-runtime PRIVATE
    ai_runtime_proto
    Threads::Threads
    dl
    rt
)

# ── Unit test (no gRPC dependency, only HAL stub) ────────────────────────────
add_executable(ai-runtime-test
    test/test_with_stub.cpp
    src/hal_ml_loader.cpp
    src/model_manager.cpp
    src/session_manager.cpp
    src/inference_scheduler.cpp
    src/config.cpp
    ${HAL_COMMON_SOURCES}
)

target_include_directories(ai-runtime-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HAL_INCLUDE_DIR}
    ${HAL_INCLUDE_DIR}/common
    ${HAL_INCLUDE_DIR}/model
    ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/camera-daemon/include
)

target_link_libraries(ai-runtime-test PRIVATE
    Threads::Threads
    dl
)

enable_testing()
add_test(NAME ai_runtime_unit_test COMMAND ai-runtime-test)

# ── Install ──────────────────────────────────────────────────────────────────
install(TARGETS ai-runtime DESTINATION /opt/aipc/bin)

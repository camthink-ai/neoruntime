cmake_minimum_required(VERSION 3.16)
project(af_demo_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(af-ir-follow-core STATIC core/af_ir_follow.cpp)
target_include_directories(af-ir-follow-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_compile_options(af-ir-follow-core PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
    add_executable(af-ir-follow-test tests/af_ir_follow_test.cpp)
    target_link_libraries(af-ir-follow-test PRIVATE af-ir-follow-core)
    target_compile_options(af-ir-follow-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME af-ir-follow-test COMMAND af-ir-follow-test)
endif()

# ==============================================================================
# PeriphNet - Host-Native Unit Tests
# Built separately from firmware (no ARM toolchain)
#
# Build:
#   cmake -B build_tests -S tests
#   cmake --build build_tests -j8
#   ctest --test-dir build_tests -V
# ==============================================================================

cmake_minimum_required(VERSION 3.22)
project(PeriphNetTests CXX C)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

enable_testing()

# ==============================================================================
# CppUTest (fetched from GitHub)
# ==============================================================================

include(FetchContent)

FetchContent_Declare(
    CppUTest
    GIT_REPOSITORY https://github.com/cpputest/cpputest.git
    GIT_TAG        v4.0
    GIT_SHALLOW    TRUE
)

# Disable CppUTest's own test suite and memory detection (keep it simple)
set(TESTS           OFF CACHE BOOL "" FORCE)
set(COVERAGE        OFF CACHE BOOL "" FORCE)
set(C++11           OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(CppUTest)

# ==============================================================================
# Helper: define a test executable and register with CTest
# Usage: add_cpputest(NAME <name> SOURCES <file.cpp> ...)
# ==============================================================================

function(add_cpputest)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;INCLUDES;LIBS" ${ARGN})
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE CppUTest CppUTestExt ${ARG_LIBS})
    if(ARG_INCLUDES)
        target_include_directories(${ARG_NAME} PRIVATE ${ARG_INCLUDES})
    endif()
    add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME} -v)
endfunction()

# ==============================================================================
# Tests
# ==============================================================================

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cpputest(
    NAME test_simple
    SOURCES test_simple.cpp
)

add_cpputest(
    NAME test_image_transfer
    SOURCES
        test_image_transfer.cpp
        mocks/mock_support.c
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/mocks
        ${PROJECT_ROOT}/Core/Inc
        ${PROJECT_ROOT}/Core/Src
)

# Suppress %lu warnings on 64-bit host (uint32_t is unsigned int, not unsigned long)
target_compile_options(test_image_transfer PRIVATE -Wno-format)

# TEST_MODE disables the FreeRTOS task/queue path and keeps synchronous flash ops
# so that existing tests work without needing queue/tcpip_callback mocks
target_compile_definitions(test_image_transfer PRIVATE TEST_MODE)

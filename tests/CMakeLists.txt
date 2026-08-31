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

set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ==============================================================================
# Shared/ unit tests — plain C, no framework, no network needed.
# mocks/ comes first on the include path so mocks/w25q128.h shadows the real
# driver header (which drags in the HAL).
# ==============================================================================

set(UNIT_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${PROJECT_ROOT}/Shared/Crypto
    ${PROJECT_ROOT}/Shared/Fwu
    ${PROJECT_ROOT}/Shared/Modbus
    ${PROJECT_ROOT}/Shared/NvDb
)

# nvDb is now the only authority over the medium, so every test that touches
# flash links it and drives it through its host port.
set(NVDB_CORE
    ${PROJECT_ROOT}/Shared/NvDb/nvdb.c
    ${PROJECT_ROOT}/Shared/NvDb/nvdb_layout.c
    ${PROJECT_ROOT}/Shared/NvDb/nvdb_config.c
    ${PROJECT_ROOT}/Shared/NvDb/nvdb_wear.c
    mocks/w25q128_mock.c
    mocks/nvdb_port_stub.c
)

add_executable(test_crypto
    test_crypto.c
    ${PROJECT_ROOT}/Shared/Crypto/sha256.c
    ${PROJECT_ROOT}/Shared/Crypto/hmac_sha256.c
    ${PROJECT_ROOT}/Shared/Crypto/aes128.c
    ${PROJECT_ROOT}/Shared/Crypto/aes_gcm.c
)
target_include_directories(test_crypto PRIVATE ${UNIT_INCLUDES})
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_version
    test_version.c
    ${PROJECT_ROOT}/Shared/Fwu/version.c
)
target_include_directories(test_version PRIVATE ${UNIT_INCLUDES})
add_test(NAME version COMMAND test_version)

# App/Fwu/boot_status_medium.c is application code, but it depends only on
# nvdb.h by design — so the test drives the same path the firmware does
# rather than a second implementation of it.
add_executable(test_boot_status
    test_boot_status.c
    ${NVDB_CORE}
    ${PROJECT_ROOT}/App/Fwu/boot_status_medium.c
    ${PROJECT_ROOT}/Shared/Fwu/boot_status.c
    ${PROJECT_ROOT}/Shared/Fwu/image_mgmt.c
    ${PROJECT_ROOT}/Shared/Fwu/version.c
)
target_include_directories(test_boot_status PRIVATE ${UNIT_INCLUDES})
add_test(NAME boot_status COMMAND test_boot_status)

add_executable(test_modbus_store
    test_modbus_store.c
    ${NVDB_CORE}
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_store.c
    ${PROJECT_ROOT}/Shared/Fwu/image_mgmt.c
    ${PROJECT_ROOT}/Shared/Fwu/version.c
)
target_include_directories(test_modbus_store PRIVATE ${UNIT_INCLUDES})
add_test(NAME modbus_store COMMAND test_modbus_store)

add_executable(test_modbus_frame
    test_modbus_frame.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_frame.c
)
target_include_directories(test_modbus_frame PRIVATE ${UNIT_INCLUDES})
add_test(NAME modbus_frame COMMAND test_modbus_frame)

add_executable(test_modbus_plans
    test_modbus_plans.c
    ${NVDB_CORE}
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_plans.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_store.c
    ${PROJECT_ROOT}/Shared/Fwu/image_mgmt.c
    ${PROJECT_ROOT}/Shared/Fwu/version.c
)
target_include_directories(test_modbus_plans PRIVATE ${UNIT_INCLUDES})
add_test(NAME modbus_plans COMMAND test_modbus_plans)

add_executable(test_modbus_blocks
    test_modbus_blocks.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_blocks.c
)
target_include_directories(test_modbus_blocks PRIVATE ${UNIT_INCLUDES})
add_test(NAME modbus_blocks COMMAND test_modbus_blocks)

add_executable(test_modbus_units
    test_modbus_units.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_units.c
)
target_include_directories(test_modbus_units PRIVATE ${UNIT_INCLUDES})
add_test(NAME modbus_units COMMAND test_modbus_units)

add_executable(test_modbus_compiler
    test_modbus_compiler.c
    ${NVDB_CORE}
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_compiler.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_store.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_blocks.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_units.c
    ${PROJECT_ROOT}/Shared/Fwu/image_mgmt.c
    ${PROJECT_ROOT}/Shared/Fwu/version.c
)
target_include_directories(test_modbus_compiler PRIVATE ${UNIT_INCLUDES})
# The integration suite uploads this fixture to a board; compiling it here is
# what stops a broken fixture from being discovered over HTTP.
target_compile_definitions(test_modbus_compiler PRIVATE
    MB_FIXTURE_PATH="${PROJECT_ROOT}/tests/fixtures/modbus_solis.json"
    MB_JK_FIXTURE_PATH="${PROJECT_ROOT}/tests/fixtures/modbus_jk_pb.json")
add_test(NAME modbus_compiler COMMAND test_modbus_compiler)

add_executable(test_modbus_export
    test_modbus_export.c
    ${NVDB_CORE}
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_export.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_compiler.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_config_store.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_blocks.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_units.c
    ${PROJECT_ROOT}/Shared/Modbus/modbus_decode.c
    ${PROJECT_ROOT}/Shared/Fwu/image_mgmt.c
    ${PROJECT_ROOT}/Shared/Fwu/version.c
)
target_include_directories(test_modbus_export PRIVATE ${UNIT_INCLUDES})
add_test(NAME modbus_export COMMAND test_modbus_export)

# nvDb: the core is RTOS-free precisely so it can be driven here with no
# board, over the same NOR-faithful flash mock.  mocks/nvdb_port_stub.c is the
# port; the tests call NvDb_CollectStep() themselves.
set(NVDB_SOURCES
    ${NVDB_CORE}
    ${PROJECT_ROOT}/Shared/Fwu/image_mgmt.c
    ${PROJECT_ROOT}/Shared/Fwu/version.c
)

add_executable(test_nvdb_access   test_nvdb_access.c   ${NVDB_SOURCES})
add_executable(test_nvdb_collect  test_nvdb_collect.c  ${NVDB_SOURCES})
add_executable(test_nvdb_layout   test_nvdb_layout.c   ${NVDB_SOURCES})
add_executable(test_nvdb_config   test_nvdb_config.c   ${NVDB_SOURCES})
foreach(_t access collect layout config)
    target_include_directories(test_nvdb_${_t} PRIVATE ${UNIT_INCLUDES})
    add_test(NAME nvdb_${_t} COMMAND test_nvdb_${_t})
endforeach()

# App/Net/wg_conf.c is application code, but it depends only on libc by
# design (no lwIP, no HAL) precisely so the .conf parser can be tested here.
add_executable(test_wg_conf
    test_wg_conf.c
    ${PROJECT_ROOT}/App/Net/wg_conf.c
)
target_include_directories(test_wg_conf PRIVATE
    ${UNIT_INCLUDES}
    ${PROJECT_ROOT}
    ${PROJECT_ROOT}/App/Net
)
add_test(NAME wg_conf COMMAND test_wg_conf)

# App/Pack: the battery pack module (docs/design_battery_pack.md).  pack_fsm.c
# and pack_cfg.c are libc-only APPLICATION code, on the wg_conf.c precedent
# and for the same reason — everything interesting is a pure function of
# (previous condition, group ticks, now) or of the bytes of a config, so it is
# testable here with no board, no RTOS and no flash.  §17: "defer the test
# pack type; do not defer testability".
#
# THESE TESTS ARE EXPECTED TO FAIL: the module is scaffolding, and the suite
# is the executable form of the contract it must grow into.
add_executable(test_pack_fsm
    test_pack_fsm.c
    ${PROJECT_ROOT}/App/Pack/pack_fsm.c
)
target_include_directories(test_pack_fsm PRIVATE
    ${UNIT_INCLUDES}
    ${PROJECT_ROOT}
)
add_test(NAME pack_fsm COMMAND test_pack_fsm)

add_executable(test_pack_soc
    test_pack_soc.c
    ${PROJECT_ROOT}/App/Pack/pack_soc.c
)
target_include_directories(test_pack_soc PRIVATE
    ${UNIT_INCLUDES}
    ${PROJECT_ROOT}
)
add_test(NAME pack_soc COMMAND test_pack_soc)

add_executable(test_pack_cfg
    test_pack_cfg.c
    ${PROJECT_ROOT}/App/Pack/pack_cfg.c
)
target_include_directories(test_pack_cfg PRIVATE
    ${UNIT_INCLUDES}
    ${PROJECT_ROOT}
)
add_test(NAME pack_cfg COMMAND test_pack_cfg)

# ==============================================================================
# CppUTest suite (fetched from GitHub) — OFF by default.
#
# NOTE: test_image_transfer.cpp targets the old raw-lwIP image_transfer
# (tcp_pcb/pbuf callbacks) and has not been rewritten for the netconn +
# staged-blob architecture. Enable with -DPERIPHNET_CPPUTEST=ON once updated.
# ==============================================================================

option(PERIPHNET_CPPUTEST "Build the CppUTest-based suite (stale, needs rework)" OFF)

if(PERIPHNET_CPPUTEST)
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

    function(add_cpputest)
        cmake_parse_arguments(ARG "" "NAME" "SOURCES;INCLUDES;LIBS" ${ARGN})
        add_executable(${ARG_NAME} ${ARG_SOURCES})
        target_link_libraries(${ARG_NAME} PRIVATE CppUTest CppUTestExt ${ARG_LIBS})
        if(ARG_INCLUDES)
            target_include_directories(${ARG_NAME} PRIVATE ${ARG_INCLUDES})
        endif()
        add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME} -v)
    endfunction()

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
            ${PROJECT_ROOT}/App/Fwu
            ${PROJECT_ROOT}/Shared/Fwu
            ${PROJECT_ROOT}/Shared/Crypto
    )

    # Suppress %lu warnings on 64-bit host (uint32_t is unsigned int, not unsigned long)
    target_compile_options(test_image_transfer PRIVATE -Wno-format)

    # TEST_MODE disables the FreeRTOS task/queue path and keeps synchronous flash ops
    # so that existing tests work without needing queue/tcpip_callback mocks
    target_compile_definitions(test_image_transfer PRIVATE TEST_MODE)
endif()

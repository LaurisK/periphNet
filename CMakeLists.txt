# ==============================================================================
# PeriphNet – STM32F407VET6 Firmware  (Phase 3: Bootloader + Application)
# Two targets: bootloader.elf (32KB @ 0x08000000)
#              application.elf (480KB @ 0x08008000)
# ==============================================================================

cmake_minimum_required(VERSION 3.22)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake)

project(PeriphNet VERSION 1.0.0 LANGUAGES C ASM)

# ==============================================================================
# Language standard
# ==============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# ==============================================================================
# MCU / CPU flags
# ==============================================================================

set(MCU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

# ==============================================================================
# Compile flags (shared by both targets)
# ==============================================================================

set(COMMON_FLAGS
    "${MCU_FLAGS} -Wall -fdata-sections -ffunction-sections -fno-omit-frame-pointer"
)
set(CMAKE_C_FLAGS          "${COMMON_FLAGS}")
set(CMAKE_C_FLAGS_DEBUG    "-g3 -O0")
set(CMAKE_C_FLAGS_RELEASE  "-O2")
set(CMAKE_ASM_FLAGS        "${MCU_FLAGS} -x assembler-with-cpp")

# ==============================================================================
# Global compile definitions (shared by both targets)
# ==============================================================================

add_compile_definitions(
    STM32F407xx
    USE_HAL_DRIVER
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:USE_FULL_ASSERT>
)

# ==============================================================================
# STM32F4 HAL driver sources (shared)
# ==============================================================================

file(GLOB HAL_SOURCES CONFIGURE_DEPENDS
    "Drivers/STM32F4xx_HAL_Driver/Src/*.c"
)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*template.*")

# ==============================================================================
# Include directories (shared base)
# ==============================================================================

set(BASE_INCLUDES
    .
    Core/Inc
    Shared/Crypto
    Shared/Fwu
    Shared/Drivers
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
)

# ==============================================================================
# Shared/ — first-party code compiled into BOTH targets.
# Rules: Shared depends only on HAL + libc; never on App/, bootloader/,
# FreeRTOS or lwIP. Core/ stays CubeMX-owned (regeneration-safe).
# ==============================================================================

set(SHARED_CRYPTO_SOURCES
    Shared/Crypto/sha256.c
    Shared/Crypto/hmac_sha256.c
    Shared/Crypto/aes128.c
    Shared/Crypto/aes_gcm.c
)

set(SHARED_FWU_SOURCES
    Shared/Fwu/boot_status.c
    Shared/Fwu/image_mgmt.c
    Shared/Fwu/version.c
)

set(SHARED_DRIVER_SOURCES
    Shared/Drivers/w25q128.c
)

set(SHARED_SOURCES
    ${SHARED_CRYPTO_SOURCES}
    ${SHARED_FWU_SOURCES}
    ${SHARED_DRIVER_SOURCES}
)

# Application-only Shared/ code: the Modbus config machinery is host-testable
# pure logic (libc + W25Q128 only) but must NOT link into the bootloader —
# the 32 KB BL budget is tight and it has no use for it.
set(SHARED_MODBUS_SOURCES
    Shared/Modbus/modbus_config_store.c
    Shared/Modbus/modbus_config_compiler.c
    Shared/Modbus/modbus_config_export.c
    Shared/Modbus/modbus_blocks.c
    Shared/Modbus/modbus_frame.c
    Shared/Modbus/modbus_config_plans.c
    Shared/Modbus/modbus_decode.c
    Shared/Modbus/modbus_units.c
)

# Application-only Shared/ code: the fault-context flash path.  It is a second,
# minimal driver rather than a mode of w25q128.c — the two differ in their time
# base, their error policy, their bus-recovery behaviour and whether they may
# block, which is a different driver, not a flag.  Kept out of SHARED_SOURCES
# so the 32 KB bootloader never pays for a path it cannot reach: the BL has no
# RTOS, no crash log and no fault recorder (docs/task_fault_context_flash.md
# §3.1, §4).
set(SHARED_FAULT_SOURCES
    Shared/Drivers/w25q_fault.c
)

# Application-only Shared/ code: nvDb owns the external flash address space.
# Host-testable pure logic (libc + W25Q128 + its own port) and, like the
# Modbus config machinery, kept out of the 32 KB bootloader — the BL runs
# before the application exists and learns where things are from FWU, never
# from nvDb (docs/task_nv_db.md §1.6, C20).
set(SHARED_NVDB_SOURCES
    Shared/NvDb/nvdb.c
    Shared/NvDb/nvdb_layout.c
    Shared/NvDb/nvdb_config.c
    Shared/NvDb/nvdb_wear.c
)

# ==============================================================================
# Trice ID management helpers
# ==============================================================================

set(TRICE_TOOL ${CMAKE_CURRENT_SOURCE_DIR}/tools/trice)
set(TRICE_TIL  ${CMAKE_CURRENT_SOURCE_DIR}/til.json)
set(TRICE_LI   ${CMAKE_CURRENT_SOURCE_DIR}/li.json)

add_custom_target(trice-insert
    COMMAND ${TRICE_TOOL} insert
        -src ${CMAKE_CURRENT_SOURCE_DIR}/App
        -src ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src
        -i   ${TRICE_TIL}
        -li  ${TRICE_LI}
    COMMENT "Inserting Trice IDs..."
    VERBATIM
)

# ==============================================================================
#  _____             _   _                 _
# | __  | ___  ___ _| |_| | ___  ___  ___ | | ___  ___
# | __ -|| . || . ||   _|| || . || .'|| . || || -_||  _|
# |_____||___||___||_|  |_||___||__,||___||_||___||_|
#
# Target: bootloader.elf  (32KB @ 0x08000000, no RTOS/network)
# ==============================================================================

# FWU keys: committed secrets.c holds publicly-known DEVELOPMENT keys.
# A gitignored bootloader/secrets_prod.c (same symbols) takes precedence.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/secrets_prod.c)
    set(BL_SECRETS bootloader/secrets_prod.c)
    message(STATUS "FWU keys: PRODUCTION (bootloader/secrets_prod.c)")
else()
    set(BL_SECRETS bootloader/secrets.c)
    message(STATUS "FWU keys: development defaults (bootloader/secrets.c)")
endif()

set(BL_SOURCES
    bootloader/boot_main.c
    bootloader/boot_api.c
    bootloader/boot_stm32f4xx_it.c
    # boot_status.c is Shared and compiles into both targets, so it cannot
    # call nvDb.  Each target answers boot_status_medium.h its own way: the
    # BL writes the flash directly, the application goes through nvDb.
    bootloader/boot_status_medium.c
    ${BL_SECRETS}
    Core/Startup/startup_stm32f407vetx.s
    Core/Src/gpio.c
    Core/Src/spi.c
    Core/Src/stm32f4xx_hal_msp.c
    Core/Src/system_stm32f4xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    ${SHARED_SOURCES}
)

set(BL_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld)

set(BL_LINK_FLAGS
    "${MCU_FLAGS} \
    -T${BL_LINKER_SCRIPT} \
    -Wl,-Map=bootloader.map,--cref \
    -Wl,--gc-sections \
    -Wl,--print-memory-usage \
    -Wl,--no-warn-rwx-segments \
    --specs=nano.specs"
)

add_executable(bootloader.elf
    ${BL_SOURCES}
    ${HAL_SOURCES}
)

target_include_directories(bootloader.elf PRIVATE
    ${BASE_INCLUDES}
    bootloader
)

# Bootloader does NOT define APPLICATION_BUILD → VTOR stays at 0x08000000
target_compile_definitions(bootloader.elf PRIVATE
    BOOTLOADER_BUILD
)

set_target_properties(bootloader.elf PROPERTIES
    LINK_FLAGS "${BL_LINK_FLAGS}"
)

# Post-build: generate binary + size report
add_custom_command(TARGET bootloader.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:bootloader.elf> bootloader.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bootloader.elf> bootloader.bin
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:bootloader.elf>
    COMMENT "Bootloader size:"
)

# ==============================================================================
#    _             _ _           _   _
#   /_\  _ __ _ __| (_) __ __ _| |_(_) ___  _ __
#  / _ \| '_ \ '_ \ | |/ _/ _` |  _| |/ _ \| '  \
# /_/ \_\ .__/ .__/_|_|\__\__,_|\__|_|\___/|_||_|
#        |_|  |_|
#
# Target: application.elf  (480KB @ 0x08008000, FreeRTOS + lwIP + Trice)
# ==============================================================================

# --- CubeMX Core sources (application uses full peripheral set) ---

set(APP_CORE_SOURCES
    Core/Src/main.c
    Core/Src/can.c
    Core/Src/dma.c
    Core/Src/freertos.c
    Core/Src/gpio.c
    Core/Src/i2c.c
    Core/Src/iwdg.c
    Core/Src/rng.c
    Core/Src/rtc.c
    Core/Src/sdio.c
    Core/Src/spi.c
    Core/Src/stm32f4xx_hal_msp.c
    Core/Src/stm32f4xx_hal_timebase_tim.c
    Core/Src/stm32f4xx_it.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    Core/Src/system_stm32f4xx.c
    Core/Src/usart.c
    Core/Startup/startup_stm32f407vetx.s
    ${SHARED_SOURCES}
)

# --- App/ module sources ---

file(GLOB_RECURSE APP_MODULE_SOURCES CONFIGURE_DEPENDS
    "App/*.c"
)

# nvDb must never link into the bootloader, and the bootloader must never
# include its headers: the BL has no knowledge of nvDb by design, which is
# what leaves the directory format free to evolve (docs/task_nv_db.md §1.6).
file(GLOB_RECURSE _bl_srcs CONFIGURE_DEPENDS "bootloader/*.c" "bootloader/*.h")
foreach(_f ${_bl_srcs})
    file(STRINGS "${_f}" _hits REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]nvdb")
    if(_hits)
        message(FATAL_ERROR "nvDb reached the bootloader: ${_f}: ${_hits}")
    endif()
endforeach()

# ------------------------------------------------------------------------------
# nvDb is the ONLY authority over the medium (docs/task_nv_db.md, rule 2),
# enforced the same way the secrets and Modbus-boundary rules are: greppable,
# checked at configure time, so it cannot decay back into a convention.
#
# Nothing in App/ or Shared/ may call the flash driver or name an ext-flash
# address.  Four files are allowed to, each for a stated reason:
#
#   Shared/Drivers/w25q128.c  IS the driver
#   Shared/NvDb/nvdb.c        IS the authority
#   App/Log/crash.c           writes in fault context, where the RTOS may be
#                             dead — it takes its address from nvDb through
#                             nvdb_exceptions.h and does the I/O itself, now
#                             via w25q_fault.c rather than the ordinary driver
#                             (docs/task_fault_context_flash.md)
#   App/Fwu/fwu_control.c     compares nvDb's placement against what the
#                             bootloader was built for, and refuses to arm an
#                             install if they have diverged (§1.6)
#   Shared/Fwu/image_mgmt.c   streams an HMAC over flash for the BOOTLOADER;
#                             the application only ever calls it on internal,
#                             memory-mapped flash
# ------------------------------------------------------------------------------

file(GLOB_RECURSE _medium_srcs CONFIGURE_DEPENDS "App/*.c" "App/*.h"
                                                 "Shared/*.c" "Shared/*.h")
foreach(_f ${_medium_srcs})
    if(_f MATCHES "Shared/Drivers/|Shared/NvDb/|App/Log/crash\\.c|App/Fwu/fwu_control\\.c|Shared/Fwu/image_mgmt\\.c|Shared/Fwu/bl_app_contract\\.h")
        continue()
    endif()
    file(STRINGS "${_f}" _hits REGEX "W25Q128_(Read|WritePage|Erase[A-Za-z0-9]*)\\(|EXT_FLASH_[A-Z_]*_ADDR")
    if(_hits)
        message(FATAL_ERROR
                "nvDb is the only authority over the medium: ${_f} reaches it "
                "directly: ${_hits}")
    endif()
endforeach()

# ------------------------------------------------------------------------------
# The fault-context flash path may depend on nothing that an interrupt has to
# deliver (docs/task_fault_context_flash.md §2, §5.1).  The tick is a TIM6
# interrupt the recorder's context cannot be preempted by, so a deadline
# expressed in it never expires — which is how a crash recorder came to hang
# and destroy the evidence it exists to preserve.
#
# Checked here rather than trusted, because the last convention of this kind
# decayed the same way.  It is a plain text scan, so w25q_fault.c also avoids
# these names in its comments and says what it means in words instead.
# ------------------------------------------------------------------------------

foreach(_f ${SHARED_FAULT_SOURCES} Shared/Drivers/w25q_fault.h)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}" _hits
         REGEX "HAL_GetTick|HAL_Delay|HAL_SPI_|HAL_GPIO_|osDelay|osThread|osMutex|xSemaphore|xTaskGet|vTaskDelay|taskENTER_CRITICAL|portMAX_DELAY|W25Q128_")
    if(_hits)
        message(FATAL_ERROR
                "fault-context path depends on an interrupt: ${_f}: ${_hits}")
    endif()
endforeach()

# FWU keys must never link into the application (see Key Constraints).
foreach(_app_src ${APP_MODULE_SOURCES})
    if(_app_src MATCHES "secrets")
        message(FATAL_ERROR "secrets source leaked into application: ${_app_src}")
    endif()
endforeach()

# ------------------------------------------------------------------------------
# Modbus module boundary (docs/modbus.md §2.2), enforced the same way the
# secrets guard is: greppable rules, checked at configure time.
#
#   1. Files in App/Modbus must not include App/Mqtt, App/Http, App/Can,
#      App/Data or any lwIP header.  The module knows nothing about its
#      consumers, and that is what lets the engine be rebuilt without touching
#      one.
#   2. A CONSUMER includes only App/Modbus/modbus.h.  Scheduler, engine and
#      framing are module-internal; a DRIVER is the one exception and includes
#      modbus_port.h.
#   3. Consumers may name Shared/Modbus TYPES but must not call its flash
#      accessors, so they must not include its accessor headers either.
# ------------------------------------------------------------------------------

file(GLOB_RECURSE _mb_module_srcs CONFIGURE_DEPENDS
     "App/Modbus/*.c" "App/Modbus/*.h")
foreach(_f ${_mb_module_srcs})
    file(STRINGS "${_f}" _hits REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<](App/(Mqtt|Http|Can|Data)/|lwip/)")
    if(_hits)
        message(FATAL_ERROR
                "Modbus module boundary: ${_f} includes a consumer or lwIP header: ${_hits}")
    endif()
endforeach()

file(GLOB_RECURSE _mb_consumer_srcs CONFIGURE_DEPENDS
     "App/*.c" "App/*.h")
foreach(_f ${_mb_consumer_srcs})
    if(_f MATCHES "App/Modbus/")
        continue()
    endif()
    # A driver may include modbus_port.h; nothing else module-internal is fair
    # game, and neither are Shared/Modbus's flash accessors.
    file(STRINGS "${_f}" _hits
         REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<](App/Modbus/modbus_(engine|internal|rtu)\\.h|modbus_config_(store|compiler|export|plans)\\.h)")
    if(_hits)
        message(FATAL_ERROR
                "Modbus module boundary: ${_f} reaches past modbus.h: ${_hits}")
    endif()
endforeach()

# --- Backtrace library ---

set(BACKTRACE_SOURCES
    Middlewares/Third_Party/backtrace/backtrace.c
)

# --- WireGuard-lwIP (application only) ---
# Bundles its own Curve25519 / ChaCha20-Poly1305 / BLAKE2s.  Must NEVER reach
# the 32 KB bootloader — same rule as SHARED_MODBUS_SOURCES.  The Cortex-M0
# assembly x25519 under src/crypto/cortex/ is deliberately unused; crypto.h
# selects the portable refc implementation.

set(WIREGUARD_SOURCES
    Middlewares/Third_Party/wireguard-lwip/src/crypto.c
    Middlewares/Third_Party/wireguard-lwip/src/wireguard.c
    Middlewares/Third_Party/wireguard-lwip/src/wireguardif.c
    Middlewares/Third_Party/wireguard-lwip/src/crypto/refc/blake2s.c
    Middlewares/Third_Party/wireguard-lwip/src/crypto/refc/chacha20.c
    Middlewares/Third_Party/wireguard-lwip/src/crypto/refc/chacha20poly1305.c
    Middlewares/Third_Party/wireguard-lwip/src/crypto/refc/poly1305-donna.c
    Middlewares/Third_Party/wireguard-lwip/src/crypto/refc/x25519.c
)

# wireguardif.c carries two leftover printf() calls (upstream marks the stdio
# include "TODO: Remove").  Newlib's printf mallocs a ~1 KB stdio buffer out of
# the 1.5 KB ._user_heap_stack via a non-thread-safe allocator, and these run
# from a task holding the lwIP core lock — so redirect them to a no-op sink
# instead of patching the submodule.  Object-like macro on purpose: a
# function-like -Dprintf(...) would also mangle the declaration in <stdio.h>.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/wireguard-lwip/src/wireguardif.c
    PROPERTIES COMPILE_DEFINITIONS "printf=WgPlatform_NullPrintf"
)

# --- BSP / PHY driver ---

file(GLOB_RECURSE BSP_SOURCES
    "Drivers/BSP/Components/*/*.c"
)

# --- lwIP ---

file(GLOB_RECURSE LWIP_SOURCES
    "LWIP/App/*.c"
    "LWIP/Target/*.c"
    "Middlewares/Third_Party/LwIP/src/*.c"
    "Middlewares/Third_Party/LwIP/system/*.c"
)

# --- FreeRTOS ---

file(GLOB_RECURSE FREERTOS_SOURCES
    "Middlewares/Third_Party/FreeRTOS/Source/*.c"
)
list(FILTER FREERTOS_SOURCES EXCLUDE REGEX ".*template.*")
list(FILTER FREERTOS_SOURCES EXCLUDE REGEX ".*example.*")

# --- USB Device CDC (Virtual COM Port) ---

set(USB_DEVICE_SOURCES
    USB_DEVICE/App/usb_device.c
    USB_DEVICE/App/usbd_desc.c
    USB_DEVICE/App/usbd_cdc_if.c
    USB_DEVICE/Target/usbd_conf.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c
    Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c
)

# --- Trice library ---

add_subdirectory(Middlewares/Third_Party/trice)

target_include_directories(trice PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/App
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32F4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32F4xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
)

# --- Application include directories ---

set(APP_INCLUDES
    ${BASE_INCLUDES}
    Shared/Modbus
    Shared/NvDb
    Core/ThreadSafe
    App
    USB_DEVICE/App
    USB_DEVICE/Target
    Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
    Middlewares/Third_Party/backtrace
    Middlewares/Third_Party/wireguard-lwip/src
    App/Net
    Drivers/BSP/Components/dp83848
    LWIP/App
    LWIP/Target
    Middlewares/Third_Party/LwIP/src/include
    Middlewares/Third_Party/LwIP/system
    Middlewares/Third_Party/LwIP/src/include/netif/ppp
    Middlewares/Third_Party/LwIP/src/include/lwip
    Middlewares/Third_Party/LwIP/src/include/lwip/apps
    Middlewares/Third_Party/LwIP/src/include/lwip/priv
    Middlewares/Third_Party/LwIP/src/include/lwip/prot
    Middlewares/Third_Party/LwIP/src/include/netif
    Middlewares/Third_Party/LwIP/src/include/compat/posix
    Middlewares/Third_Party/LwIP/src/include/compat/posix/arpa
    Middlewares/Third_Party/LwIP/src/include/compat/posix/net
    Middlewares/Third_Party/LwIP/src/include/compat/posix/sys
    Middlewares/Third_Party/LwIP/src/include/compat/stdc
    Middlewares/Third_Party/LwIP/system/arch
    Middlewares/Third_Party/FreeRTOS/Source/include
    Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
)

# --- Linker ---

set(APP_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/App/application.ld)

set(APP_LINK_FLAGS
    "${MCU_FLAGS} \
    -T${APP_LINKER_SCRIPT} \
    -Wl,-Map=application.map,--cref \
    -Wl,--gc-sections \
    -Wl,--print-memory-usage \
    -Wl,--no-warn-rwx-segments \
    --specs=nano.specs"
)

# --- Build target ---

add_executable(application.elf
    ${APP_CORE_SOURCES}
    ${APP_MODULE_SOURCES}
    ${SHARED_MODBUS_SOURCES}
    ${SHARED_NVDB_SOURCES}
    ${SHARED_FAULT_SOURCES}
    ${BACKTRACE_SOURCES}
    ${WIREGUARD_SOURCES}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${LWIP_SOURCES}
    ${FREERTOS_SOURCES}
    ${USB_DEVICE_SOURCES}
)

add_dependencies(application.elf trice-insert)

target_include_directories(application.elf PRIVATE ${APP_INCLUDES})

target_link_libraries(application.elf PRIVATE trice)

# APPLICATION_BUILD enables VTOR relocation to 0x08008000
# Build time as a UNIX epoch — the floor for the WireGuard monotonic clock
# (App/Net/wg_time.h). Evaluated at configure time, so it is stable across
# incremental builds and only moves when CMake re-runs.
string(TIMESTAMP WG_TIME_BUILD_EPOCH "%s" UTC)

target_compile_definitions(application.elf PRIVATE
    APPLICATION_BUILD
    WG_TIME_BUILD_EPOCH=${WG_TIME_BUILD_EPOCH}u
)

set_target_properties(application.elf PROPERTIES
    LINK_FLAGS "${APP_LINK_FLAGS}"
)

# Post-build: generate binary + sign + size report + clean Trice IDs
add_custom_command(TARGET application.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:application.elf> application.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:application.elf> application.bin
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/dfu_image_tool.py sign
            -i ${CMAKE_CURRENT_BINARY_DIR}/application.bin
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:application.elf>
    COMMENT "Application size:"
)

add_custom_command(TARGET application.elf POST_BUILD
    COMMAND ${TRICE_TOOL} clean
        -src ${CMAKE_CURRENT_SOURCE_DIR}/App
        -src ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src
        -i   ${TRICE_TIL}
        -li  ${TRICE_LI}
    COMMENT "Cleaning Trice IDs..."
    VERBATIM
)

# ==============================================================================
# FWU artifacts
#   periphnet_full.hex — BL + signed APP, factory / initial J-Link write
#   periphnet_fwu.pnfw — encrypted+authenticated blob for OTA upload
# Requires python packages: cryptography, intelhex (checked at configure time)
# ==============================================================================

execute_process(
    COMMAND python3 -c "import cryptography, intelhex"
    RESULT_VARIABLE PY_DFU_DEPS_MISSING
    OUTPUT_QUIET ERROR_QUIET
)

if(NOT PY_DFU_DEPS_MISSING)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/periphnet_full.hex
               ${CMAKE_CURRENT_BINARY_DIR}/periphnet_fwu.pnfw
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/dfu_image_tool.py full
                -i ${CMAKE_CURRENT_BINARY_DIR}/application.bin
                -o ${CMAKE_CURRENT_BINARY_DIR}/periphnet_full.hex
                -b ${CMAKE_CURRENT_BINARY_DIR}/bootloader.bin
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/dfu_image_tool.py package
                -i ${CMAKE_CURRENT_BINARY_DIR}/application.bin
                -o ${CMAKE_CURRENT_BINARY_DIR}/periphnet_fwu.pnfw
        DEPENDS bootloader.elf application.elf
        COMMENT "Generating FWU artifacts (full hex + encrypted blob)"
        VERBATIM
    )
    add_custom_target(fwu_artifacts ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/periphnet_full.hex
                ${CMAKE_CURRENT_BINARY_DIR}/periphnet_fwu.pnfw
    )
else()
    message(WARNING "python3 packages 'cryptography'/'intelhex' missing — "
                    "FWU artifacts (periphnet_full.hex / periphnet_fwu.pnfw) "
                    "will not be generated")
endif()

# ==============================================================================
# Flash targets (J-Link clone wrapper)
# ==============================================================================

add_custom_target(flash
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/flash_nokill.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/flash_application.jlink
    DEPENDS application.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Flashing application via J-Link"
)

add_custom_target(flash-all
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/flash_nokill.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/flash_both.jlink
    DEPENDS bootloader.elf application.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Flashing bootloader + application via J-Link"
)

add_custom_target(flash-bootloader
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/flash_nokill.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/flash_bootloader.jlink
    DEPENDS bootloader.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Flashing bootloader via J-Link"
)

# ==============================================================================
# Build information
# ==============================================================================

message(STATUS "")
message(STATUS "=== PeriphNet Phase 3 – Bootloader + Application ===")
message(STATUS "MCU:            STM32F407VET6")
message(STATUS "Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "Bootloader LD:  ${BL_LINKER_SCRIPT}")
message(STATUS "Application LD: ${APP_LINKER_SCRIPT}")
message(STATUS "")

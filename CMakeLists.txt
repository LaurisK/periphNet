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

# NO -fno-omit-frame-pointer.  It cost 11,832 bytes of flash and existed only
# for the R7-chain unwinder in Middlewares/Third_Party/backtrace, which is gone:
# the call path now comes from the stack LR scan in App/Log/crash.c, which needs
# no frame chain (the ZhagaFW crash_handler.c approach, commit 3130846).
set(COMMON_FLAGS
    "${MCU_FLAGS} -Wall -fdata-sections -ffunction-sections -g3 -fno-strict-aliasing"
)
set(CMAKE_C_FLAGS          "${COMMON_FLAGS}")

# EMPTY ON PURPOSE.  These used to hold the -O flags while CMAKE_BUILD_TYPE was
# unset, so neither ever applied -- nobody writes both flag sets in order to use
# neither, and the result was an accidental -O0 ship.  Optimisation now comes
# from PERIPHNET_OPT below; CMAKE_BUILD_TYPE keeps only its defines.  -g3 moved
# into COMMON_FLAGS so debug info does not depend on a build type either (it
# costs nothing in flash -- objcopy does not carry .debug_* into the .bin).
set(CMAKE_C_FLAGS_DEBUG    "")
set(CMAKE_C_FLAGS_RELEASE  "")
set(CMAKE_ASM_FLAGS        "${MCU_FLAGS} -x assembler-with-cpp")

# ==============================================================================
# Optimisation — the MIXED profile
# ==============================================================================
#
# WHY THIS IS NOT ONE -O FLAG.  Until now CMAKE_BUILD_TYPE was unset while both
# CMAKE_C_FLAGS_DEBUG and CMAKE_C_FLAGS_RELEASE were defined, so NEITHER
# applied and every image this project ever measured was built with no -O flag
# at all.  That put the default build at 99.85 % of flash with 724 bytes free.
# The whole-tree answer (-DCMAKE_BUILD_TYPE=Release) gets it to 72 %, but it
# also optimises the code that is under active development, which changes what
# the debugger shows and what a stack high-water mark reads — the two things
# this project diagnoses field failures with.
#
# So optimisation is chosen PER BUCKET, not per build:
#
#   OPT_VENDOR  third-party and generated code — the HAL, lwIP, FreeRTOS, the
#               USB device library, the BSP PHY driver, wireguard-lwip, trice.
#               Nobody steps through it and nobody is going to fix it, so it
#               is compiled for size.  It is also the majority of the image,
#               which is why this bucket is where the flash comes back.
#
#   OPT_APP     first-party App/, Shared/, Core/ and bootloader/ — the code
#               that gets debugged.  -Og, which is the level GCC documents as
#               "optimise without interfering with debugging": no inlining of
#               everything, stack frames and locals stay recognisable, and the
#               LR-scan backtrace in App/Log/crash.c still resolves.
#
#   OPT_HOT     Shared/Crypto — AES-128, GCM, SHA-256, HMAC.  Pure computation
#               on the OTA install path (the bootloader streams the whole image
#               through it three times), tested against NIST vectors, and small
#               enough that -O2 costs little.  Speed, not size.
#
# PROFILES.  PERIPHNET_OPT selects the set; `mixed` is the default.
#
#   debug   everything -O0          — what this project shipped by accident
#   mixed   vendor -Os, app -Og     — the default
#   size    everything -Os          — for when flash is the only question
#   speed   everything -O2          — fastest, least debuggable
#
# CMAKE_BUILD_TYPE NO LONGER PICKS AN OPTIMISATION LEVEL.  It still selects the
# DEBUG / USE_FULL_ASSERT defines below, but the -O flags come from here, so
# setting it can no longer silently do nothing (and can no longer silently
# undo a bucket).  Per-source options are appended after CMAKE_C_FLAGS and the
# per-config flags on the command line, and GCC honours the LAST -O it sees —
# that ordering is what makes the buckets authoritative.
#
# -fno-strict-aliasing IS NOT OPTIONAL HERE.  At -O0 the aliasing rules are
# unenforced; from -Og upward GCC assumes objects of different types do not
# overlap.  This firmware reads flash records, Modbus frames, .pnfw manifests
# and JSON straight out of byte buffers, and the crash recorder walks a stack
# as words.  Turning that assumption on across a tree written without it is
# how a build starts miscompiling one function in a way no test reproduces.
# The flag costs almost nothing and it is bought once, here, for every bucket.
#
# NOT DONE, DELIBERATELY: -flto.  The image depends on weak HAL callbacks being
# overridden at link time, on __attribute__((section)) placement for
# .app_header / .bl_api / .ccmram, and on a .bin whose HMAC is patched in
# place afterwards.  LTO is a separate piece of work with its own verification,
# not a flag to add alongside this one.

set(PERIPHNET_OPT "mixed" CACHE STRING
    "Optimisation profile: debug | mixed | size | speed")
set_property(CACHE PERIPHNET_OPT PROPERTY STRINGS debug mixed size speed)

if(PERIPHNET_OPT STREQUAL "debug")
    set(OPT_APP    "-O0")
    set(OPT_VENDOR "-O0")
    set(OPT_HOT    "-O0")
elseif(PERIPHNET_OPT STREQUAL "mixed")
    set(OPT_APP    "-Og")
    set(OPT_VENDOR "-Os")
    set(OPT_HOT    "-O2")
elseif(PERIPHNET_OPT STREQUAL "size")
    set(OPT_APP    "-Os")
    set(OPT_VENDOR "-Os")
    set(OPT_HOT    "-Os")
elseif(PERIPHNET_OPT STREQUAL "speed")
    set(OPT_APP    "-O2")
    set(OPT_VENDOR "-O2")
    set(OPT_HOT    "-O2")
else()
    message(FATAL_ERROR
            "PERIPHNET_OPT must be debug, mixed, size or speed "
            "(got '${PERIPHNET_OPT}')")
endif()

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

# Application-only Shared/ code: THE JSON module (docs/task_json_module.md).
# The clamped writer exists because the `n += snprintf(&buf[n], CAP - n, ...)`
# idiom it replaces underflows CAP - n into a ~4 GB size limit at an address
# already past the end of the buffer; the reader replaces three private
# tokenizers, none of which handled escapes.  Kept out of SHARED_SOURCES for
# the usual reason: the 32 KB bootloader parses no JSON and emits none.
set(SHARED_JSON_SOURCES
    Shared/Json/json_write.c
    Shared/Json/json_read.c
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

# The bootloader is first-party except for the HAL it drags in, so it takes the
# OPT_APP baseline here and the HAL/crypto buckets are applied to those sources
# further down (source-level options come last on the command line, so they win
# over this one).  Its 32 KB ceiling makes this the target that benefits most.
target_compile_options(bootloader.elf PRIVATE ${OPT_APP})

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

# Same rule for the JSON module: the bootloader parses no JSON and emits none,
# and the 32 KB budget has no room to discover otherwise by accident
# (docs/task_json_module.md §3.3).
foreach(_f ${_bl_srcs})
    file(STRINGS "${_f}" _hits REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]json")
    if(_hits)
        message(FATAL_ERROR "JSON reached the bootloader: ${_f}: ${_hits}")
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

# --- Pack module boundaries (docs/design_battery_pack.md §9) ------------------
# Seven greppable rules.  The fourth is the load-bearing one: a lock is a
# CONTEXT decision, and a pack type does not know its context statically -- the
# same pack_pylontech.c code is reachable from an ISR and from a task.  So a
# type is not allowed to hold one, and PackType_Publish is the single place
# that chooses.  Making that mechanical is what stops the class of error
# recurring rather than merely fixing this instance of it.

# 1/2. The core, the FSM and the parser may not reach a protocol or a consumer.
foreach(_f App/Pack/pack.c App/Pack/pack_fsm.c App/Pack/pack_cfg.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}" _hits
             REGEX "^[ 	]*#[ 	]*include[ 	]*[\"<](App/(Mqtt|Http|Can|Data|Modbus)/|lwip/|modbus|pylontech)")
        if(_hits)
            message(FATAL_ERROR
                    "Pack boundary: ${_f} must not know a protocol or a consumer: ${_hits}")
        endif()
    endif()
endforeach()

# 3. pack_fsm.c and pack_soc.c are libc-only: they are what tests/ links, and
#    an RTOS or HAL call would take them out of reach of the host build.
#    pack_soc.c especially -- it is the SOC estimator, and its arithmetic
#    should be provable on a laptop rather than only observable on a roof.
foreach(_pure App/Pack/pack_fsm.c App/Pack/pack_soc.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pure}")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${_pure}" _hits
             REGEX "HAL_|osThread|osTimer|osMessage|osKernel|xQueue|xTask|vTask|taskENTER_CRITICAL|TRice|NvDb_|W25Q128_")
        if(_hits)
            message(FATAL_ERROR
                    "${_pure} must stay libc-only (host-tested): ${_hits}")
        endif()
    endif()
endforeach()

# 3c. The embedded web UI must contain no C-escaped quote inside its string
#     literal.  `\'` in a C double-quoted string is just `'`, so a JS string
#     written as class=\'x\' arrives at the browser as class='x' -- string,
#     bare identifier, string -- which is a SyntaxError that kills the WHOLE
#     script, blanking every card on the page including the firmware ones.
#     Observed on 2026-08-27.  Single-token unquoted attributes avoid it
#     entirely; this makes the alternative fail loudly.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/App/Http/http_server.c")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/App/Http/http_server.c" _hits
         REGEX "^[ \t]*\"[^\"]*\\\\'")
    if(_hits)
        message(FATAL_ERROR
                "Web UI: C-escaped quote in the HTML literal reaches the "
                "browser unescaped and breaks the whole script: ${_hits}")
    endif()
endif()

# 4. A TYPE may not take a lock.  PackType_Publish chooses the context.
file(GLOB _pack_types "${CMAKE_CURRENT_SOURCE_DIR}/App/Pack/pack_jkbms.c"
                      "${CMAKE_CURRENT_SOURCE_DIR}/App/Pack/pack_pylontech.c")
foreach(_f ${_pack_types})
    file(STRINGS "${_f}" _hits
         REGEX "taskENTER_CRITICAL|taskDISABLE_INTERRUPTS|__disable_irq|portENTER_CRITICAL")
    if(_hits)
        message(FATAL_ERROR
                "Pack type ${_f} must not lock -- PackType_Publish owns that: ${_hits}")
    endif()
endforeach()

# 5. No floating point crosses the API.
foreach(_f App/Pack/pack.h App/Pack/pack_type.h)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}" _hits
             REGEX "^[ 	]*(float|double)[ 	]+[A-Za-z_]")
        if(_hits)
            message(FATAL_ERROR "No float may cross the pack API: ${_f}: ${_hits}")
        endif()
    endif()
endforeach()

# 6. Consumers see pack.h and nothing else from the module.
file(GLOB_RECURSE _pack_consumers "${CMAKE_CURRENT_SOURCE_DIR}/App/*.c"
                                  "${CMAKE_CURRENT_SOURCE_DIR}/App/*.h")
foreach(_f ${_pack_consumers})
    if(_f MATCHES "App/Pack/")
        continue()
    endif()
    file(STRINGS "${_f}" _hits
         REGEX "^[ 	]*#[ 	]*include[ 	]*[\"<](App/Pack/)?pack_(type|fsm|cfg)\.h")
    if(_hits)
        message(FATAL_ERROR
                "Pack module boundary: ${_f} reaches past pack.h: ${_hits}")
    endif()
endforeach()

# --- Cluster module boundaries (docs/design_battery_cluster.md §4) ------------
# SIX RULES, split by WHAT IS CHECKED.  Includes and RTOS calls are source
# facts and are checked with a line-anchored grep; floating point and
# Pack_Command are OBJECT-FILE facts and are checked after the link, because a
# source grep cannot tell a rule from a sentence describing the rule -- and
# cluster.h's own preamble contains the words "float", "double" and
# "Pack_Command".  The object-file check is also strictly stronger: it catches
# a `(float)` cast and a `1.0f` literal, which the pack module's
# declaration-anchored regex misses entirely.

# 1/6b. Nothing in App/Cluster reaches a consumer, a protocol, lwIP or the HAL.
file(GLOB _clu_srcs CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/App/Cluster/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/App/Cluster/*.h")
foreach(_f ${_clu_srcs})
    file(STRINGS "${_f}" _hits
         REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<](App/(Can|Http|Gw|Data|Modbus)/|lwip/|stm32f4xx_hal)")
    if(_hits)
        message(FATAL_ERROR
                "Cluster boundary: ${_f} must not know a consumer, a protocol "
                "or the HAL: ${_hits}")
    endif()
endforeach()

# 2. Consumers see cluster.h and nothing else from the module.
file(GLOB_RECURSE _clu_consumers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/App/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/App/*.h")
foreach(_f ${_clu_consumers})
    if(_f MATCHES "App/Cluster/")
        continue()
    endif()
    file(STRINGS "${_f}" _hits
         REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<](App/Cluster/)?cluster_(calc|cfg)\\.h")
    if(_hits)
        message(FATAL_ERROR
                "Cluster module boundary: ${_f} reaches past cluster.h: ${_hits}")
    endif()
endforeach()

# 3. THE PURE CORE is libc-only -- and, unlike pack_fsm.c, may not call Pack_
#    either: a call into the live pack module is a call into a lock, and would
#    make the core non-pure and untestable at once.  Its entire input is the
#    sClusterPackIn array the caller fills.
foreach(_pure App/Cluster/cluster_calc.c App/Cluster/cluster_cfg.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_pure}")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/${_pure}" _hits
             REGEX "HAL_|osThread|osTimer|osMessage|osKernel|xQueue|xTask|vTask|taskENTER_CRITICAL|TRice|NvDb_|W25Q128_|Pack_[A-Z]")
        if(_hits)
            message(FATAL_ERROR
                    "${_pure} must stay libc-only and Pack_-free: ${_hits}")
        endif()
    endif()
endforeach()

# 5. NO LOCK AND NO CRITICAL SECTION anywhere in the module.  That is what
#    makes Cluster_GetOutput legal from the RTOS timer task.  __DMB() is a
#    BARRIER, NOT A LOCK, and is deliberately absent from this pattern --
#    without saying so, "no lock" reads as "no synchronisation of any kind",
#    which is how a plain s_gen++ between two struct writes survives every
#    debug build and fails once at -O2.  __disable_irq is inline asm with no
#    symbol, so it stays a source grep rather than an object-file one.
foreach(_f ${_clu_srcs})
    file(STRINGS "${_f}" _hits
         REGEX "osMutex|vPortEnterCritical|taskENTER_CRITICAL|portENTER_CRITICAL|taskDISABLE_INTERRUPTS|__disable_irq")
    if(_hits)
        message(FATAL_ERROR
                "Cluster: no lock and no critical section (${_f}): ${_hits}")
    endif()
endforeach()

# 4/6. Floating point and Pack_Command, ON THE OBJECT FILES.  With
#      -mfloat-abi=hard a float op emits a VFP mnemonic and a double op emits
#      an __aeabi_d* reference, so the pair catches both; a comment produces
#      no relocation, so the Pack_Command check is EXACT where a grep cannot
#      be.  Run as a post-build step over App/Cluster's objects.
get_filename_component(_arm_bin_dir "${CMAKE_OBJDUMP}" DIRECTORY)
set(CLUSTER_PURITY_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_cluster_purity.cmake")

# -Wdouble-promotion / -Wfloat-conversion / -Wfloat-equal, SCOPED to the two
# pure sources and never global: lwIP, FreeRTOS, the HAL, trice and
# wireguard-lwip share CMAKE_C_FLAGS and are not ours to fix.
set(CLUSTER_PURE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Cluster/cluster_calc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Cluster/cluster_cfg.c
)

# --- CAN module boundary (docs/design_can_bridge.md §5) -----------------------
# ONE FILE OWNS THE bxCAN CELLS.  The class of bug this prevents has already
# happened once: App/Can/bms_reader.c re-initialised CAN2 and defined the only
# weak HAL_CAN_RxFifo0MsgPendingCallback, which is a link-level monopoly — it
# blocked pack_pylontech outright (docs/design_battery_pack.md §16 item 5) and
# would have blocked the bridge too.  A second definition of that callback is
# not a merge conflict, it is a silent choice made by link order.
#
# So: nothing in App/ may name a CAN HAL function or a CAN handle except
# App/Can/can_bus.c.  Consumers subscribe, send and read counters through
# can_bus.h.  Core/ is exempt — CubeMX owns can.c and the IRQ handlers.

file(GLOB_RECURSE _can_srcs CONFIGURE_DEPENDS "App/*.c" "App/*.h")
foreach(_f ${_can_srcs})
    if(_f MATCHES "App/Can/can_bus\\.c")
        continue()
    endif()
    file(STRINGS "${_f}" _hits REGEX "HAL_CAN_[A-Za-z]|hcan1|hcan2")
    if(_hits)
        message(FATAL_ERROR
                "CAN module boundary: only App/Can/can_bus.c may own the bxCAN "
                "cells: ${_f}: ${_hits}")
    endif()
endforeach()

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

# The lwIP MQTT client is vendor code and stays on disk, but nothing links it
# any more: MQTT was removed from the project when the Solis path became a
# Modbus TCP gateway to solis_modbus, which supplies the HA entities instead
# (docs/design_solis_modbus_link.md §9.1).  The glob above is recursive, so it
# has to be dropped by name rather than by not listing it.
list(REMOVE_ITEM LWIP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/LwIP/src/apps/mqtt/mqtt.c"
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
    Shared/Json
    Shared/Modbus
    Shared/NvDb
    Core/ThreadSafe
    App
    USB_DEVICE/App
    USB_DEVICE/Target
    Middlewares/ST/STM32_USB_Device_Library/Core/Inc
    Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc
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
    ${SHARED_JSON_SOURCES}
    ${SHARED_MODBUS_SOURCES}
    ${SHARED_NVDB_SOURCES}
    ${SHARED_FAULT_SOURCES}
    ${WIREGUARD_SOURCES}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${LWIP_SOURCES}
    ${FREERTOS_SOURCES}
    ${USB_DEVICE_SOURCES}
)

add_dependencies(application.elf trice-insert)

# ------------------------------------------------------------------------------
# The optimisation buckets (see PERIPHNET_OPT at the top of this file)
#
# OPT_APP is the target baseline: everything in App/, Shared/, Core/ compiles
# at the level chosen for code that is still debugged.  The two lists below
# then override it per source.  GCC honours the LAST -O on the command line and
# CMake emits source-level options after target-level ones, so an entry in
# OPT_VENDOR_SOURCES or SHARED_CRYPTO_SOURCES wins over the baseline without
# either list having to know about the other.
#
# These are COMPILE_OPTIONS on the source, which is a different property from
# the COMPILE_DEFINITIONS on wireguardif.c and from the -Werror=switch /
# float-purity options set on the cluster and enum-name sources below -- those
# carry no -O, so the baseline still reaches them.  Nothing here clobbers
# anything there.

target_compile_options(application.elf PRIVATE ${OPT_APP})

# Third-party and generated code.  Not debugged, not ours to fix, and the
# majority of the image.  Core/ is NOT here: CubeMX generates it but main.c,
# stm32f4xx_it.c and the MSP are stepped through like any other first-party
# file.
set(OPT_VENDOR_SOURCES
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${LWIP_SOURCES}
    ${FREERTOS_SOURCES}
    ${USB_DEVICE_SOURCES}
    ${WIREGUARD_SOURCES}
)
set_source_files_properties(${OPT_VENDOR_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${OPT_VENDOR}"
)

# trice is its own static library target, so it takes the bucket as a target
# option rather than per source.
target_compile_options(trice PRIVATE ${OPT_VENDOR})

# TWO VENDOR WARNINGS THAT ONLY EXIST ONCE THE OPTIMISER RUNS.  Both were
# analysed when this profile landed and both are false positives; they are
# silenced per file, with the reasoning here, so that a clean build stays a
# meaningful signal instead of two lines everyone learns to scroll past.
#
#  * x25519.c -Wstringop-overread on `mul(z2, x2, a24, 1)`.  `mul`'s third
#    parameter is declared `const fe b` (limb_t[8]) but the function only ever
#    reads b[0..nb-1], and every caller passes the matching nb -- here a24 is
#    limb_t[1] with nb = 1.  GCC checks the DECLARED bound, not the used one.
#    Passing a shorter array with an explicit length is the upstream idiom and
#    it is correct; changing the signature would mean patching the submodule.
#
#  * triceDoubleBuffer.c -Wmaybe-uninitialized on `readBuf`.  It is assigned
#    under `if (tLen32)` and read under a second `if (tLen32)` on the same
#    local, which nothing between them can modify (its address is never taken;
#    the critical-section macros are barriers, not writes).  The warning only
#    appears after TriceOut is inlined into TriceTransfer, which is the
#    classic shape of a conservative -Wmaybe-uninitialized.
#
# Both are scoped to the one file.  Never global: a real uninitialised read in
# App/ or Shared/ is exactly the kind of bug -Og is worth having.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/wireguard-lwip/src/crypto/refc/x25519.c
    PROPERTIES COMPILE_OPTIONS "${OPT_VENDOR};-Wno-stringop-overread"
)
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/trice/src/triceDoubleBuffer.c
    TARGET_DIRECTORY trice
    PROPERTIES COMPILE_OPTIONS "-Wno-maybe-uninitialized"
)

# The hot bucket: the OTA install path streams the whole image through GCM
# three times (CRC pass, verify pass, program pass) and the bootloader HMACs it
# again at every boot.  Directory-scoped rather than target-scoped on purpose
# -- the bootloader wants this more than the application does.
set_source_files_properties(${SHARED_CRYPTO_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${OPT_HOT}"
)

# ------------------------------------------------------------------------------
# -Werror=switch, SCOPED to the files that carry an enum->string accessor
#
# Each of these files answers "what is this enumerator called" with a switch
# whose fallback sits AFTER the switch rather than in a `default:` -- a
# `default:` satisfies -Wswitch and so defeats the whole mechanism.  With the
# error scoped here, adding an enumerator without giving it a name FAILS THE
# BUILD instead of shipping a "?" that an operator finds in the field, which
# is precisely how http_server.c's private crash-type table came to report
# StackOverflow as "Unknown" (docs/task_http_server_restructure.md §7.1).
#
# NEVER GLOBAL.  lwIP, FreeRTOS, the HAL, trice and wireguard-lwip share
# CMAKE_C_FLAGS and are not ours to fix; a global -Werror=switch would fail
# the build on vendor code.  Measured 2026-09-04: these six files produce
# zero -Wswitch warnings today, so this costs nothing until it fires.
#
# This is a deliberate, documented departure from `C coding standard.md`'s
# "every switch has a default" -- the standard's intent is that an unhandled
# case must not pass silently, and a compile-time failure serves that intent
# better than a runtime placeholder.
set(ENUM_NAME_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Pack/pack_cfg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Log/crash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Modbus/modbus.c
    # Nine accessors, instances fifteen through twenty-three
    # (docs/design_battery_cluster.md constraint 12).
    ${CMAKE_CURRENT_SOURCE_DIR}/App/Cluster/cluster_cfg.c
)
set_source_files_properties(${ENUM_NAME_SOURCES}
    TARGET_DIRECTORY application.elf
    PROPERTIES COMPILE_OPTIONS "-Werror=switch"
)

# The cluster's two pure sources additionally refuse floating point at the
# WARNING level, which catches the readable cases early; the object-file check
# below is what makes it a rule rather than a habit.  cluster_cfg.c is in both
# lists, so its options are spelled once, together.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/App/Cluster/cluster_cfg.c
    TARGET_DIRECTORY application.elf
    PROPERTIES COMPILE_OPTIONS
    "-Werror=switch;-Wdouble-promotion;-Wfloat-conversion;-Wfloat-equal;-Werror=double-promotion;-Werror=float-conversion"
)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/App/Cluster/cluster_calc.c
    TARGET_DIRECTORY application.elf
    PROPERTIES COMPILE_OPTIONS
    "-Wdouble-promotion;-Wfloat-conversion;-Wfloat-equal;-Werror=double-promotion;-Werror=float-conversion"
)

# THE OBJECT-FILE CHECKS (constraints 4 and 6).  No VFP mnemonic, no
# __aeabi_[df] reference, no Pack_Command relocation may come out of
# App/Cluster.  A comment produces no relocation, so this is exact where a
# source grep -- which would fire on cluster.h's own preamble -- cannot be.
add_custom_command(TARGET application.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DOBJDIR=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/application.elf.dir/App/Cluster
            -DOBJDUMP=${CMAKE_OBJDUMP}
            -DNM=${_arm_bin_dir}/arm-none-eabi-nm
            -P ${CLUSTER_PURITY_SCRIPT}
    COMMENT "Cluster purity: no float, no Pack_Command in App/Cluster objects"
    VERBATIM
)

target_include_directories(application.elf PRIVATE ${APP_INCLUDES})

# ------------------------------------------------------------------------------
# CubeIDE include-path parity
#
# CMake is canonical, but the Eclipse project builds the SAME application from
# the SAME sources with its OWN copy of the include list, kept in `.cproject`.
# That file is gitignored (machine-local, and CubeMX rewrites it), so nothing
# keeps the two lists in step and the drift only ever shows up as a header not
# found — which is exactly how `Shared/NvDb` went missing until the first
# CubeIDE build after nvDb landed.
#
# WARNING, never FATAL: a fresh clone legitimately has no `.cproject` at all,
# and CMake must not care.  This just makes the drift say so at configure time
# instead of waiting for someone to open the IDE.
# ------------------------------------------------------------------------------

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.cproject")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.cproject" _cproj)
    set(_missing_inc "")
    foreach(_inc ${APP_INCLUDES})
        string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" _rel "${_inc}")
        # `..` is the project root, spelled differently by the two tools
        if(_rel STREQUAL "." OR _rel STREQUAL "")
            continue()
        endif()
        if(NOT _cproj MATCHES "value=\"\.\./${_rel}\"")
            list(APPEND _missing_inc "${_rel}")
        endif()
    endforeach()
    if(_missing_inc)
        string(REPLACE ";" "\n    " _missing_pretty "${_missing_inc}")
        message(WARNING
                "CubeIDE (.cproject) is missing include paths that the CMake "
                "build has:\n    ${_missing_pretty}\n"
                "The Eclipse Debug/Release build will fail on a missing header. "
                "Add them under Project > Properties > C/C++ Build > Settings > "
                "MCU GCC Compiler > Include paths, for BOTH configurations.")
    endif()
endif()

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
# Web UI artifact
#   periphnet_ui.pnui — the gzipped web page, uploaded to POST /api/ui
#
# SEPARATE FROM THE FIRMWARE ON PURPOSE.  The page lives in external flash
# (nvdbUser_webUi), not in the image, so it ships and updates as its own file:
# a UI change needs no firmware cycle and no confirm, and a firmware update
# does not replace the page.  The image carries only a fallback page that says
# how to install this one.  Needs no python packages and no keys — a UI blob is
# not executable — so it is generated unconditionally, unlike the FWU blobs.
# ==============================================================================

set(WEB_UI_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/App/Http/web/index.html)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/periphnet_ui.pnui
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/dfu_image_tool.py ui
            -i ${WEB_UI_SOURCE}
            -o ${CMAKE_CURRENT_BINARY_DIR}/periphnet_ui.pnui
    DEPENDS ${WEB_UI_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/dfu_image_tool.py
    COMMENT "Packaging web UI (gzip -> .pnui)"
    VERBATIM
)

add_custom_target(web_ui ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/periphnet_ui.pnui
)

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
message(STATUS "Optimisation:   ${PERIPHNET_OPT}  (app ${OPT_APP}, vendor ${OPT_VENDOR}, crypto ${OPT_HOT})")
message(STATUS "Bootloader LD:  ${BL_LINKER_SCRIPT}")
message(STATUS "Application LD: ${APP_LINKER_SCRIPT}")
message(STATUS "")

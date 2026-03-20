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

file(GLOB HAL_SOURCES
    "Drivers/STM32F4xx_HAL_Driver/Src/*.c"
)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*template.*")

# ==============================================================================
# Include directories (shared base)
# ==============================================================================

set(BASE_INCLUDES
    .
    Core/Inc
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
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

set(BL_SOURCES
    bootloader/boot_main.c
    bootloader/boot_api.c
    bootloader/boot_stm32f4xx_it.c
    Core/Startup/startup_stm32f407vetx.s
    Core/Src/gpio.c
    Core/Src/spi.c
    Core/Src/stm32f4xx_hal_msp.c
    Core/Src/system_stm32f4xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    Core/Src/w25q128.c
    Core/Src/boot_status.c
    Core/Src/image_mgmt.c
    Core/Src/version.c
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

target_include_directories(bootloader.elf PRIVATE ${BASE_INCLUDES})

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
    Core/Src/w25q128.c
    Core/Src/boot_status.c
    Core/Src/image_mgmt.c
    Core/Src/version.c
    Core/Startup/startup_stm32f407vetx.s
    application/app_info.c
)

# --- App/ module sources ---

file(GLOB_RECURSE APP_MODULE_SOURCES
    "App/*.c"
)

# --- Backtrace library ---

set(BACKTRACE_SOURCES
    Middlewares/Third_Party/backtrace/backtrace.c
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
    Core/ThreadSafe
    App
    Middlewares/Third_Party/backtrace
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

set(APP_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/application/application.ld)

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
    ${BACKTRACE_SOURCES}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${LWIP_SOURCES}
    ${FREERTOS_SOURCES}
)

add_dependencies(application.elf trice-insert)

target_include_directories(application.elf PRIVATE ${APP_INCLUDES})

target_link_libraries(application.elf PRIVATE trice)

# APPLICATION_BUILD enables VTOR relocation to 0x08008000
target_compile_definitions(application.elf PRIVATE
    APPLICATION_BUILD
)

set_target_properties(application.elf PROPERTIES
    LINK_FLAGS "${APP_LINK_FLAGS}"
)

# Post-build: generate binary + size report + clean Trice IDs
add_custom_command(TARGET application.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:application.elf> application.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:application.elf> application.bin
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

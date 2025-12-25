# ==============================================================================
# PeriphNet – STM32F407VET6 Firmware  (Phase 1: Foundation)
# Single application target, full 512 KB flash at 0x08000000
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
# CubeMX Core sources (hand-picked to exclude legacy app code)
# ==============================================================================

set(CORE_SOURCES
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
    Core/Startup/startup_stm32f407vetx.s
)

# ==============================================================================
# Application sources (App/)
# ==============================================================================

file(GLOB_RECURSE APP_SOURCES
    "App/*.c"
)

# ==============================================================================
# Backtrace library
# ==============================================================================

set(BACKTRACE_SOURCES
    Middlewares/Third_Party/backtrace/backtrace.c
)

# ==============================================================================
# STM32F4 HAL driver sources
# ==============================================================================

file(GLOB HAL_SOURCES
    "Drivers/STM32F4xx_HAL_Driver/Src/*.c"
)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*template.*")

# ==============================================================================
# BSP / PHY driver
# ==============================================================================

file(GLOB_RECURSE BSP_SOURCES
    "Drivers/BSP/Components/*/*.c"
)

# ==============================================================================
# lwIP
# ==============================================================================

file(GLOB_RECURSE LWIP_SOURCES
    "LWIP/App/*.c"
    "LWIP/Target/*.c"
    "Middlewares/Third_Party/LwIP/src/*.c"
    "Middlewares/Third_Party/LwIP/system/*.c"
)

# ==============================================================================
# FreeRTOS
# ==============================================================================

file(GLOB_RECURSE FREERTOS_SOURCES
    "Middlewares/Third_Party/FreeRTOS/Source/*.c"
)
list(FILTER FREERTOS_SOURCES EXCLUDE REGEX ".*template.*")
list(FILTER FREERTOS_SOURCES EXCLUDE REGEX ".*example.*")

# ==============================================================================
# Compile definitions
# ==============================================================================

add_compile_definitions(
    STM32F407xx
    USE_HAL_DRIVER
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:USE_FULL_ASSERT>
)

# ==============================================================================
# Compile flags
# ==============================================================================

set(COMMON_FLAGS
    "${MCU_FLAGS} -Wall -fdata-sections -ffunction-sections -fno-omit-frame-pointer"
)
set(CMAKE_C_FLAGS          "${COMMON_FLAGS}")
set(CMAKE_C_FLAGS_DEBUG    "-g3 -O0")
set(CMAKE_C_FLAGS_RELEASE  "-O2")
set(CMAKE_ASM_FLAGS        "${MCU_FLAGS} -x assembler-with-cpp")

# ==============================================================================
# Trice library (embedded trace)
# ==============================================================================

add_subdirectory(Middlewares/Third_Party/trice)

# triceConfig.h lives in App/; also expose Core/Inc and FreeRTOS headers
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
# Include directories
# ==============================================================================

set(APP_INCLUDES
    .
    Core/Inc
    Core/ThreadSafe
    App
    Middlewares/Third_Party/backtrace
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
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

# ==============================================================================
# Linker flags  (single-image, full 512 KB flash)
# ==============================================================================

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/STM32F407VETX_FLASH.ld)

set(LINK_FLAGS
    "${MCU_FLAGS} \
    -T${LINKER_SCRIPT} \
    -Wl,-Map=application.map,--cref \
    -Wl,--gc-sections \
    -Wl,--print-memory-usage \
    -Wl,--no-warn-rwx-segments \
    --specs=nano.specs"
)

# ==============================================================================
# Target: application.elf
# ==============================================================================

add_executable(application.elf
    ${CORE_SOURCES}
    ${APP_SOURCES}
    ${BACKTRACE_SOURCES}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${LWIP_SOURCES}
    ${FREERTOS_SOURCES}
)

add_dependencies(application.elf trice-insert)

target_include_directories(application.elf PRIVATE ${APP_INCLUDES})

target_link_libraries(application.elf PRIVATE trice)

set_target_properties(application.elf PROPERTIES
    LINK_FLAGS "${LINK_FLAGS}"
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
# Flash target (uses flash_nokill.sh wrapper for clone J-Link)
# ==============================================================================

add_custom_target(flash
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/flash_nokill.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/flash_application.jlink
    DEPENDS application.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Flashing application via J-Link"
)

# ==============================================================================
# Build information
# ==============================================================================

message(STATUS "")
message(STATUS "=== PeriphNet Phase 1 – Foundation ===")
message(STATUS "MCU:        STM32F407VET6")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Linker:     ${LINKER_SCRIPT}")
message(STATUS "")

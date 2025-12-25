# ==============================================================================
# PeriphNet - STM32F407VET6 Firmware CMake Build
# ==============================================================================

cmake_minimum_required(VERSION 3.22)

# Set toolchain file before project()
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake)

# Project definition
project(PeriphNet
    VERSION 1.0.0
    LANGUAGES C ASM
)

# ==============================================================================
# Build Configuration
# ==============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# MCU Configuration
set(MCU_FAMILY STM32F4)
set(MCU_MODEL STM32F407xx)
set(MCU_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/STM32F407VETX_FLASH.ld)

# CPU and FPU settings
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

# ==============================================================================
# Source Files
# ==============================================================================

# Application source files
file(GLOB_RECURSE APP_SOURCES
    "Core/Src/*.c"
    "Core/ThreadSafe/*.c"
)

# Startup file
set(STARTUP_FILE
    Core/Startup/startup_stm32f407vetx.s
)

# HAL Driver sources
file(GLOB HAL_SOURCES
    "Drivers/STM32F4xx_HAL_Driver/Src/*.c"
)

# BSP Component sources (Ethernet PHY driver)
file(GLOB_RECURSE BSP_SOURCES
    "Drivers/BSP/Components/*/*.c"
)

# LWIP sources
file(GLOB_RECURSE LWIP_SOURCES
    "LWIP/App/*.c"
    "LWIP/Target/*.c"
    "Middlewares/Third_Party/LwIP/src/*.c"
    "Middlewares/Third_Party/LwIP/system/*.c"
)

# FreeRTOS sources
file(GLOB_RECURSE FREERTOS_SOURCES
    "Middlewares/Third_Party/FreeRTOS/Source/*.c"
)

# Exclude template and example files
list(FILTER FREERTOS_SOURCES EXCLUDE REGEX ".*template.*")
list(FILTER FREERTOS_SOURCES EXCLUDE REGEX ".*example.*")

# ==============================================================================
# Include Directories
# ==============================================================================

set(INCLUDE_DIRS
    # Core includes
    Core/Inc
    Core/ThreadSafe

    # CMSIS
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include

    # HAL
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy

    # BSP Components (Ethernet PHY)
    Drivers/BSP/Components/dp83848

    # LWIP
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

    # FreeRTOS
    Middlewares/Third_Party/FreeRTOS/Source/include
    Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
)

# ==============================================================================
# Compiler Definitions
# ==============================================================================

add_compile_definitions(
    # MCU definitions
    ${MCU_MODEL}
    USE_HAL_DRIVER

    # Debug build
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:USE_FULL_ASSERT>
)

# ==============================================================================
# Compiler Flags
# ==============================================================================

set(COMMON_FLAGS "${CPU_FLAGS} -Wall -fdata-sections -ffunction-sections")

# C Flags
set(CMAKE_C_FLAGS "${COMMON_FLAGS}")
set(CMAKE_C_FLAGS_DEBUG "-g3 -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3")

# ASM Flags
set(CMAKE_ASM_FLAGS "${CPU_FLAGS} -x assembler-with-cpp")

# ==============================================================================
# Linker Flags
# ==============================================================================

set(LINKER_FLAGS
    "${CPU_FLAGS} \
    -T${MCU_LINKER_SCRIPT} \
    -Wl,-Map=${PROJECT_NAME}.map,--cref \
    -Wl,--gc-sections \
    -Wl,--print-memory-usage \
    -Wl,--allow-multiple-definition \
    --specs=nano.specs \
    -lc \
    -lm"
)

set(CMAKE_EXE_LINKER_FLAGS "${LINKER_FLAGS}")

# ==============================================================================
# Executable Target
# ==============================================================================

add_executable(${PROJECT_NAME}.elf
    ${APP_SOURCES}
    ${STARTUP_FILE}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${LWIP_SOURCES}
    ${FREERTOS_SOURCES}
)

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    ${INCLUDE_DIRS}
)

# ==============================================================================
# Post-build: Generate Binary and Hex Files
# ==============================================================================

# Generate .hex file
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMENT "Generating HEX file: ${PROJECT_NAME}.hex"
)

# Generate .bin file
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMENT "Generating BIN file: ${PROJECT_NAME}.bin"
)

# Generate extended listing
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJDUMP} -h -S $<TARGET_FILE:${PROJECT_NAME}.elf> > ${PROJECT_NAME}.list
    COMMENT "Generating listing file: ${PROJECT_NAME}.list"
)

# Print size information
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Firmware size:"
)

# ==============================================================================
# Flash target (optional - using st-flash)
# ==============================================================================

add_custom_target(flash
    COMMAND st-flash --reset write ${PROJECT_NAME}.bin 0x08000000
    DEPENDS ${PROJECT_NAME}.elf
    COMMENT "Flashing ${PROJECT_NAME}.bin to STM32F407VET6"
)

# ==============================================================================
# Print build information
# ==============================================================================

message(STATUS "")
message(STATUS "=== PeriphNet Build Configuration ===")
message(STATUS "MCU Family:      ${MCU_FAMILY}")
message(STATUS "MCU Model:       ${MCU_MODEL}")
message(STATUS "Build Type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "Linker Script:   ${MCU_LINKER_SCRIPT}")
message(STATUS "C Compiler:      ${CMAKE_C_COMPILER}")
message(STATUS "")

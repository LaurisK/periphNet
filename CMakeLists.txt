# ==============================================================================
# PeriphNet - STM32F407VET6 Firmware CMake Build
# Dual Build: Bootloader + Application
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

# CPU and FPU settings
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

# ==============================================================================
# Common Source Files (shared by bootloader and application)
# ==============================================================================

# Startup file
set(STARTUP_FILE
    Core/Startup/startup_stm32f407vetx.s
)

# HAL Driver sources (minimal for bootloader)
set(BOOTLOADER_HAL_SOURCES
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c"
)

# HAL Driver sources (full set for application)
set(APP_HAL_SOURCES
    ${BOOTLOADER_HAL_SOURCES}
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_can.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_eth.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c_ex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_iwdg.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc_ex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c"
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c"
)

# Common driver sources (shared by bootloader and application)
set(COMMON_DRIVER_SOURCES
    "Core/Src/w25q128.c"
    "Core/Src/boot_status.c"
    "Core/Src/image_mgmt.c"
    "Core/Src/version.c"
)

# Minimal system files for bootloader
set(BOOTLOADER_SOURCES
    bootloader/boot_main.c
    bootloader/boot_api.c
    bootloader/boot_stm32f4xx_it.c
    Core/Src/system_stm32f4xx.c
    Core/Src/stm32f4xx_hal_msp.c
    Core/Src/spi.c
    Core/Src/gpio.c
    ${BOOTLOADER_HAL_SOURCES}
    ${COMMON_DRIVER_SOURCES}
    ${STARTUP_FILE}
)

# ==============================================================================
# Application Sources
# ==============================================================================

# Application source files
file(GLOB_RECURSE APP_SOURCES
    "Core/Src/*.c"
    "Core/ThreadSafe/*.c"
    "application/*.c"
)

# Explicitly add DMA init (STM32CubeMX generated)
list(APPEND APP_SOURCES "Core/Src/dma.c")

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

# Common includes for bootloader
set(BOOTLOADER_INCLUDES
    Core/Inc
    Drivers/CMSIS/Device/ST/STM32F4xx/Include
    Drivers/CMSIS/Include
    Drivers/STM32F4xx_HAL_Driver/Inc
    Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
    bootloader
)

# Full includes for application
set(APP_INCLUDES
    Core/Inc
    Core/ThreadSafe
    application
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
# Compiler Definitions
# ==============================================================================

add_compile_definitions(
    ${MCU_MODEL}
    USE_HAL_DRIVER
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:USE_FULL_ASSERT>
)

# ==============================================================================
# Compiler Flags
# ==============================================================================

set(COMMON_FLAGS "${CPU_FLAGS} -Wall -fdata-sections -ffunction-sections")

set(CMAKE_C_FLAGS "${COMMON_FLAGS}")
set(CMAKE_C_FLAGS_DEBUG "-g3 -O0")
set(CMAKE_C_FLAGS_RELEASE "-O3")

set(CMAKE_ASM_FLAGS "${CPU_FLAGS} -x assembler-with-cpp")

# ==============================================================================
# TARGET 1: BOOTLOADER (32KB at 0x08000000)
# ==============================================================================

set(BOOTLOADER_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/bootloader/bootloader.ld)

set(BOOTLOADER_LINKER_FLAGS
    "${CPU_FLAGS} \
    -T${BOOTLOADER_LINKER_SCRIPT} \
    -Wl,-Map=bootloader.map,--cref \
    -Wl,--gc-sections \
    -Wl,--print-memory-usage \
    -Wl,--no-warn-rwx-segments \
    --specs=nano.specs"
)

add_executable(bootloader.elf
    ${BOOTLOADER_SOURCES}
)

target_include_directories(bootloader.elf PRIVATE
    ${BOOTLOADER_INCLUDES}
)

set_target_properties(bootloader.elf PROPERTIES
    LINK_FLAGS "${BOOTLOADER_LINKER_FLAGS}"
)

# Bootloader post-build
add_custom_command(TARGET bootloader.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:bootloader.elf> bootloader.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bootloader.elf> bootloader.bin
    COMMAND ${CMAKE_OBJDUMP} -h -S $<TARGET_FILE:bootloader.elf> > bootloader.list
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:bootloader.elf>
    COMMENT "Bootloader size:"
)

# ==============================================================================
# Third-Party Middleware Modules
# ==============================================================================

# Trice library (embedded trace system)
add_subdirectory(Middlewares/Third_Party/trice)

# Provide triceConfig.h and FreeRTOS includes to Trice library
target_include_directories(trice PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/application
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
)

# ==============================================================================
# TARGET 2: APPLICATION (480KB at 0x08008000)
# ==============================================================================

# Trice ID management
set(TRICE_TOOL ${CMAKE_CURRENT_SOURCE_DIR}/tools/trice)
set(TRICE_TIL ${CMAKE_CURRENT_SOURCE_DIR}/til.json)
set(TRICE_LI ${CMAKE_CURRENT_SOURCE_DIR}/li.json)

# Pre-build: Insert Trice IDs into source code
add_custom_target(trice-insert
    COMMAND ${TRICE_TOOL} insert
        -src ${CMAKE_CURRENT_SOURCE_DIR}/application
        -src ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src
        -i ${TRICE_TIL}
        -li ${TRICE_LI}
    COMMENT "Inserting Trice IDs into source code..."
    VERBATIM
)

set(APPLICATION_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/application/application.ld)

set(APPLICATION_LINKER_FLAGS
    "${CPU_FLAGS} \
    -T${APPLICATION_LINKER_SCRIPT} \
    -Wl,-Map=application.map,--cref \
    -Wl,--gc-sections \
    -Wl,--print-memory-usage \
    -Wl,--no-warn-rwx-segments \
    --specs=nano.specs"
)

add_executable(application.elf
    ${APP_SOURCES}
    ${STARTUP_FILE}
    ${APP_HAL_SOURCES}
    ${BSP_SOURCES}
    ${LWIP_SOURCES}
    ${FREERTOS_SOURCES}
)

# Make application build depend on trice-insert
add_dependencies(application.elf trice-insert)

target_include_directories(application.elf PRIVATE
    ${APP_INCLUDES}
)

# Link against Trice library
target_link_libraries(application.elf PRIVATE trice)

# Define APPLICATION_BUILD for VTOR relocation
target_compile_definitions(application.elf PRIVATE
    APPLICATION_BUILD
)

set_target_properties(application.elf PROPERTIES
    LINK_FLAGS "${APPLICATION_LINKER_FLAGS}"
)

# Application post-build
add_custom_command(TARGET application.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:application.elf> application.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:application.elf> application.bin
    COMMAND ${CMAKE_OBJDUMP} -h -S $<TARGET_FILE:application.elf> > application.list
    COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:application.elf>
    COMMENT "Application size:"
)

# Post-build: Clean Trice IDs from source code
add_custom_command(TARGET application.elf POST_BUILD
    COMMAND ${TRICE_TOOL} clean
        -src ${CMAKE_CURRENT_SOURCE_DIR}/application
        -src ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src
        -i ${TRICE_TIL}
        -li ${TRICE_LI}
    COMMENT "Cleaning Trice IDs from source code..."
    VERBATIM
)

# ==============================================================================
# Flash Targets
# ==============================================================================

# Find JLinkExe
find_program(JLINK_EXE
    NAMES JLinkExe
    PATHS
        /opt/SEGGER/JLink
        /opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.externaltools.jlink.linux64_*/tools/bin
        $ENV{HOME}/SEGGER/JLink
)

if(JLINK_EXE)
    # Flash bootloader only
    add_custom_target(flash-bootloader
        COMMAND ${JLINK_EXE} -device STM32F407VE -if SWD -speed 4000
                -CommanderScript ${CMAKE_SOURCE_DIR}/flash_bootloader.jlink
        DEPENDS bootloader.elf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Flashing bootloader.bin to 0x08000000 via J-Link"
    )

    # Flash application only
    add_custom_target(flash-application
        COMMAND ${JLINK_EXE} -device STM32F407VE -if SWD -speed 4000
                -CommanderScript ${CMAKE_SOURCE_DIR}/flash_application.jlink
        DEPENDS application.elf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Flashing application.bin to 0x08008000 via J-Link"
    )

    # Flash both bootloader AND application
    add_custom_target(flash-both
        COMMAND ${JLINK_EXE} -device STM32F407VE -if SWD -speed 4000
                -CommanderScript ${CMAKE_SOURCE_DIR}/flash_both.jlink
        DEPENDS bootloader.elf application.elf
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Flashing bootloader + application via J-Link"
    )

    # Default flash target (flash both)
    add_custom_target(flash
        DEPENDS flash-both
    )

    message(STATUS "J-Link found: ${JLINK_EXE}")
else()
    message(WARNING "J-Link not found. Flash targets will not be available.")
endif()

# ==============================================================================
# Build Information
# ==============================================================================

message(STATUS "")
message(STATUS "=== PeriphNet Dual-Build Configuration ===")
message(STATUS "MCU Family:      ${MCU_FAMILY}")
message(STATUS "MCU Model:       ${MCU_MODEL}")
message(STATUS "Build Type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "Bootloader:")
message(STATUS "  - Linker Script: ${BOOTLOADER_LINKER_SCRIPT}")
message(STATUS "  - Flash Addr:    0x08000000 (32KB max)")
message(STATUS "")
message(STATUS "Application:")
message(STATUS "  - Linker Script: ${APPLICATION_LINKER_SCRIPT}")
message(STATUS "  - Flash Addr:    0x08008000 (480KB max)")
message(STATUS "")

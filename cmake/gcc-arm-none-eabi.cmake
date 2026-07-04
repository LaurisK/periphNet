# ==============================================================================
# ARM GCC Toolchain File for CMake
# ==============================================================================

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ARM)

# Toolchain paths
# Try to find arm-none-eabi-gcc in common locations
find_program(ARM_CC arm-none-eabi-gcc
    PATHS
        # CubeIDE bundled GCC 12.3 — matches the newlib used by CubeIDE (preferred)
        /opt/st/stm32cubeide_1.17.0/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.linux64_1.1.0.202410170702/tools/bin
        /usr/bin
        /usr/local/bin
        $ENV{HOME}/.local/bin
        /opt/gcc-arm-none-eabi/bin
    NO_DEFAULT_PATH
)

if(NOT ARM_CC)
    # Fallback to system PATH
    find_program(ARM_CC arm-none-eabi-gcc)
endif()

if(NOT ARM_CC)
    message(FATAL_ERROR "ARM GCC toolchain not found! Please install arm-none-eabi-gcc")
endif()

# Get toolchain directory
get_filename_component(ARM_TOOLCHAIN_DIR ${ARM_CC} DIRECTORY)

# Set compilers
set(CMAKE_C_COMPILER ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-gcc)
set(CMAKE_AR ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-ar)
set(CMAKE_OBJCOPY ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-objdump)
set(CMAKE_SIZE ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-size)
set(CMAKE_GDB ${ARM_TOOLCHAIN_DIR}/arm-none-eabi-gdb)

# Prevent CMake from testing the compiler (cross-compilation)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

# Search for programs only in build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# Search for libraries and headers only in target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Don't use RPATH
set(CMAKE_SKIP_RPATH TRUE)

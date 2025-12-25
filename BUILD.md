# Build Instructions

## Prerequisites

- ARM GCC Toolchain: `arm-none-eabi-gcc` (version 13.2.1 or later)
- CMake 3.22 or later
- Make or Ninja build system
- ST-Link tools (optional, for flashing)

## Build Methods

### Option 1: CMake (Recommended for development)

**Configure:**
```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

**Build:**
```bash
make -j4
```

**Output files:**
- `PeriphNet.elf` - ELF executable with debug symbols
- `PeriphNet.bin` - Raw binary (for flashing)
- `PeriphNet.hex` - Intel HEX format
- `PeriphNet.map` - Memory map
- `PeriphNet.list` - Disassembly listing

**Build types:**
- `Debug` - Full debug symbols, no optimization (default)
- `Release` - Optimized build with -O3

**Flash to device (requires st-flash):**
```bash
make flash
```

Or manually:
```bash
st-flash --reset write PeriphNet.bin 0x08000000
```

**Clean build:**
```bash
rm -rf build/*
cd build
cmake ..
make -j4
```

### Option 2: STM32CubeIDE (Eclipse-based)

The project can still be built using STM32CubeIDE:

1. Open STM32CubeIDE
2. File → Import → Existing Projects into Workspace
3. Select this directory
4. Build the project (Ctrl+B)
5. Debug/Flash using STM32CubeIDE tools

**Output:** `Debug/PeriphNet.elf`

## Project Files

**Keep in version control:**
- `CMakeLists.txt` - CMake build configuration
- `cmake/gcc-arm-none-eabi.cmake` - ARM GCC toolchain file
- `PeriphNet.ioc` - STM32CubeMX project (for code generation)
- Source code in `Core/`, `LWIP/`, `Middlewares/`, `Drivers/`
- Linker scripts: `STM32F407VETX_FLASH.ld`

**Ignored (in .gitignore):**
- `build/` - CMake build outputs
- `Debug/` - STM32CubeIDE build outputs
- `.cproject`, `.project`, `.settings/` - IDE-specific files

## Firmware Size

Current size (with FreeRTOS + lwIP):
```
Memory region         Used Size  Region Size  %age Used
          CCMRAM:           0 B        64 KB      0.00%
             RAM:       56184 B       128 KB     42.86%
           FLASH:      120668 B       512 KB     23.02%
```

**Breakdown:**
- Code (text):    118 KB
- Data (data):    144 bytes
- BSS (bss):       55 KB
- **Total:**      176 KB

**Remaining:**
- Flash: 391 KB (76%)
- RAM: 72 KB (57%)

## Troubleshooting

**ARM GCC not found:**
```bash
# Install on Ubuntu/Debian
sudo apt-get install gcc-arm-none-eabi

# Or download from ARM website
# https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain
```

**CMake version too old:**
```bash
# Install latest CMake
sudo apt-get install cmake
```

**Build errors after STM32CubeMX code regeneration:**
```bash
# Clean and rebuild
rm -rf build
mkdir build && cd build
cmake ..
make -j4
```

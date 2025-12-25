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

**Flash to device:**

Using J-Link (default method):
```bash
make flash
```

Or manually with J-Link:
```bash
JLinkExe -device STM32F407VE -if SWD -speed 4000 -CommanderScript flash_jlink.jlink
```

Using ST-Link (alternative):
```bash
make flash-stlink
# Or manually:
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

## Hardware Flashing

### J-Link Connection

The STM32F407VET6 board is connected via J-Link debugger.

**Connection details:**
- Debugger: SEGGER J-Link V9 (S/N: 59600182)
- Interface: SWD (Serial Wire Debug)
- Target: STM32F407VE (Cortex-M4, 512KB Flash, 192KB RAM)
- Speed: 4000 kHz

**Flashing process:**
1. Build firmware: `make`
2. Flash: `make flash`
3. Device automatically erases, programs, verifies, and runs

**Flash script:** `flash_jlink.jlink`
- Erases entire chip
- Programs firmware to 0x08000000
- Verifies programmed data
- Resets and runs application

**Manual flashing with JLinkExe:**
```bash
JLinkExe -device STM32F407VE -if SWD -speed 4000 -CommanderScript flash_jlink.jlink
```

**Typical flash output:**
```
Erasing device... (8.5s)
Downloading file... (0.7s)
Verify successful.
Program & Verify speed: 210 KB/s
```

### Alternative: OpenOCD (if J-Link not available)

Create `openocd.cfg`:
```
source [find interface/jlink.cfg]
transport select swd
source [find target/stm32f4x.cfg]

init
reset halt
flash write_image erase build/PeriphNet.bin 0x08000000
verify_image build/PeriphNet.bin 0x08000000
reset run
shutdown
```

Flash with:
```bash
openocd -f openocd.cfg
```

## Troubleshooting

**J-Link not detected:**
```bash
# Check USB connection
lsusb | grep SEGGER

# Check permissions (add user to plugdev group)
sudo usermod -a -G plugdev $USER
# Log out and back in

# Install udev rules
sudo apt-get install segger-jlink-udev-rules
```

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

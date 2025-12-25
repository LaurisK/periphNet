# PeriphNet - Quick Start Guide

## 🚀 Build and Flash (TL;DR)

```bash
# Build firmware
cd /home/laurynas/Projects/PeriphNet
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j4

# Flash to device (J-Link)
make flash
```

**That's it!** Device will be erased, programmed, verified, and running in ~10 seconds.

---

## 📁 Project Structure

```
PeriphNet/
├── Core/             STM32 application code (HAL init, peripherals)
├── LWIP/             lwIP configuration and port
├── Middlewares/      FreeRTOS and lwIP source
├── Drivers/          STM32 HAL and BSP drivers
├── build/            CMake build outputs (gitignored)
├── Debug/            STM32CubeIDE outputs (gitignored)
│
├── CMakeLists.txt    Main build configuration
├── BUILD.md          Detailed build documentation
├── CLAUDE.md         AI assistant guidance
└── MILESTONE_PLAN.md Development roadmap
```

---

## 🔧 Common Commands

### Building

```bash
# Debug build (default)
mkdir build && cd build
cmake ..
make -j4

# Release build (optimized)
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4

# Clean rebuild
rm -rf build/*
mkdir -p build && cd build
cmake .. && make -j4

# Check available targets
make help
```

### Flashing

```bash
# Flash via J-Link (auto-detects device)
make flash

# Manual flash with J-Link
JLinkExe -device STM32F407VE -if SWD -speed 4000 \
         -CommanderScript flash_jlink.jlink
```

### Code Generation (STM32CubeMX)

```bash
# 1. Open PeriphNet.ioc in STM32CubeMX
# 2. Make changes to peripherals/middleware
# 3. Generate code (keeps user code intact)
# 4. Rebuild:
rm -rf build && mkdir build && cd build
cmake .. && make -j4
```

---

## 📊 Current Firmware Status

**Built:** ✅ (Milestone 0 Complete)

**Size:**
- Flash: 118 KB / 512 KB (23%) - plenty of room!
- RAM:    55 KB / 128 KB (43%) - good headroom

**Includes:**
- ✅ FreeRTOS v10.x
- ✅ lwIP TCP/IP stack
- ✅ STM32 HAL drivers
- ✅ Ethernet PHY driver (DP83848)
- ✅ All configured peripherals (CAN, SPI, I2C, UART, etc.)

**What it does (currently):**
- Initializes hardware
- Starts FreeRTOS scheduler
- Configures lwIP stack
- *Milestone 1 (HTTP server) - TODO*

---

## 🔌 Hardware Connection

**Debugger:** SEGGER J-Link V9 (S/N: 59600182)
**Interface:** SWD (Serial Wire Debug)
**Target:** STM32F407VET6
**Speed:** 4000 kHz

**Pins:**
- SWDIO - PA13
- SWCLK - PA14
- GND
- VTref (3.3V)

---

## 🛠️ Development Workflow

### Option 1: CMake (Recommended)

```bash
# 1. Edit source code in any editor (VS Code, Vim, etc.)
# 2. Build
make -j4

# 3. Flash
make flash

# 4. Debug (if needed)
arm-none-eabi-gdb build/PeriphNet.elf
(gdb) target extended-remote :2331  # J-Link GDB server
```

### Option 2: STM32CubeIDE

```bash
# 1. Open project in STM32CubeIDE
# 2. Modify .ioc file for peripheral changes
# 3. Generate code
# 4. Build in IDE
# 5. Debug/Flash using IDE tools
```

**Both methods work!** CMake and STM32CubeIDE coexist peacefully.

---

## 📈 Next Milestone

**Milestone 1: Ethernet + Basic HTTP Server**

**Goal:** Serve "Hello World v1.0.0" webpage

**Tasks:**
- [ ] Configure lwIP DHCP
- [ ] Implement minimal HTTP server
- [ ] Create static HTML page with system info
- [ ] Test: `curl http://192.168.0.XXX/`

**ETA:** 3-4 days

See `MILESTONE_PLAN.md` for full roadmap.

---

## 🆘 Quick Troubleshooting

**Build fails:**
```bash
# Clean everything
rm -rf build
# Regenerate and rebuild
mkdir build && cd build && cmake .. && make -j4
```

**Flash fails:**
```bash
# Check J-Link connection
lsusb | grep SEGGER

# Check permissions
sudo usermod -a -G plugdev $USER
# Log out and back in
```

**Code generation changes not reflected:**
```bash
# STM32CubeMX only regenerates .ioc files
# Must rebuild with CMake or CubeIDE
rm -rf build && mkdir build && cd build
cmake .. && make -j4
```

---

## 📚 Documentation Files

- `BUILD.md` - Comprehensive build instructions
- `CLAUDE.md` - Project architecture and guidelines for AI
- `MILESTONE_PLAN.md` - Development roadmap (8 milestones)
- `firmware_update_architecture.md` - Bootloader design
- `stm32_industrial_fw_project_plan.md` - Overall project plan

---

## 🎯 Key Features Planned

1. ✅ **M0:** CMake build system
2. ⏳ **M1:** Ethernet + HTTP server
3. ⏳ **M2:** Bootloader ↔ Application jump
4. ⏳ **M3:** External flash + UID validation
5. ⏳ **M4:** HTTP firmware upload
6. ⏳ **M5:** Firmware verification
7. ⏳ **M6:** Bootloader API
8. ⏳ **M7:** Installation logic
9. ⏳ **M8:** End-to-end OTA update

**End Goal:** Upload new firmware via HTTP, bootloader installs it, device boots into updated firmware showing "Hello World v2.0.0"

---

**Last Updated:** 2024-12-26
**Status:** Milestone 0 Complete ✅

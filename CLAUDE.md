# CLAUDE.md

## Project Overview

**PeriphNet** is an STM32F407VET6 firmware project. Long-term goal: RS485/Modbus-RTU to Ethernet/MQTT bridge for Solis inverter + Home Assistant, with dual-image OTA bootloader.

**Current phase:** Phase 1 Foundation — single-image build (no bootloader), FreeRTOS + lwIP + Trice, HTTP server with firmware upload/download to external SPI flash.

## Build and Flash

```bash
# First time
cmake -B build -S .

# Build
cmake --build build -j8

# Flash (uses clone J-Link wrapper to avoid popup hang)
./flash_nokill.sh flash_application.jlink

# One-liner
cmake --build build -j8 && ./flash_nokill.sh flash_application.jlink
```

**Prerequisites:** ARM GCC toolchain, CMake 3.22+, JLinkExe

**Build output:** `build/application.elf` (~144 KB flash, ~98 KB RAM)

**Clean rebuild:**
```bash
cmake --build build --target clean && cmake -B build -S . && cmake --build build -j8
```

## Testing

**Device IP:** 10.42.0.203 (DHCP on 10.42.0.x subnet)

```bash
# Check device is alive
ping 10.42.0.203
curl http://10.42.0.203/

# Upload firmware image to external flash (480KB max, ~5.5s)
curl -X POST -H "Content-Type: application/octet-stream" \
  -H "Content-Length: $(stat -c%s firmware.bin)" \
  --data-binary @firmware.bin http://10.42.0.203/api/firmware/upload

# Download stored image
curl http://10.42.0.203/api/firmware/download -o downloaded.bin

# Check status
curl http://10.42.0.203/api/firmware/status

# Verify data integrity
md5sum firmware.bin downloaded.bin
```

**Trice UART output** (USART3 PD8/TX, 460800 baud, DMA):
```bash
./tools/trice log -p COM -args "/dev/ttyUSB0:460800" -i ./til.json -li ./li.json
```

## Hardware

- **MCU:** STM32F407VET6 (512KB Flash, 128KB SRAM, 168MHz)
- **External Flash:** W25Q64 (8MB, SPI2 at 21MHz) — JEDEC 0xEF/0x40/0x17
- **EEPROM:** AT24C02BN (256 bytes, I2C)
- **Ethernet PHY:** DP83848IVV (RMII)
- **Trice:** USART3 PD8/TX PD9/RX, DMA1_Stream3, 460800 baud
- **IWDG:** ~16.4s timeout (PRESCALER_128, RELOAD 4095)
- **TIM14:** Software watchdog, 12s pre-IWDG warning

## Project Structure

```
PeriphNet/
  App/                          # Application code
    app_freertos.c/h            # FreeRTOS task init, default task, trice task
    system.c/h                  # Reset cause, KickIwdg(), System_Init()
    triceConfig.h               # Trice configuration (USART3 DMA)
    Log/crash.c/h               # Cortex-M4 crash handler + backtrace
    Http/
      http_server.c/h           # HTTP request dispatcher (port 80)
      image_transfer.c/h        # Upload/download to external SPI flash
  Core/                         # CubeMX generated + shared drivers
    Inc/                        # Headers (main.h, w25q128.h, etc.)
    Src/                        # Sources (main.c, usart.c, w25q128.c, etc.)
    Startup/                    # startup_stm32f407vetx.s
  Drivers/                      # STM32 HAL + CMSIS (vendor)
  LWIP/                         # lwIP integration (CubeMX generated)
  Middlewares/Third_Party/
    FreeRTOS/                   # RTOS
    LwIP/                       # TCP/IP stack
    trice/                      # Trice library (git submodule, uartDma branch)
    backtrace/                  # Cortex-M4 FP unwinder
  CMakeLists.txt                # Build system (single application target)
  STM32F407VETX_FLASH.ld        # Linker script (full 512KB flash)
  flash_nokill.sh               # J-Link clone flash wrapper
  flash_application.jlink       # J-Link script
  til.json / li.json            # Trice ID database (auto-managed by build)
```

**Files grouped by module/functionality, NOT by file type.** Keep .c and .h together.

## FreeRTOS Tasks

| Task | Stack | Priority | Role |
|------|-------|----------|------|
| defaultTask | 1024 words | osPriorityNormal (24) | Init, heartbeat (1s), IWDG kick (100ms), button faults |
| trice | 256 words | osPriorityNormal+1 (25) | TriceTransfer() every 10ms |
| tcpip_thread | 2048 bytes | 24 | lwIP TCP/IP processing |
| EthIf | 1024 bytes | 3 | Ethernet frame receive |

## HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Status page (HTML) |
| `/api/firmware/upload` | POST | Upload binary to external flash (Content-Length required, 480KB max) |
| `/api/firmware/download` | GET | Download stored image from external flash |
| `/api/firmware/status` | GET | JSON status: `{status, bytes_transferred, total_bytes, progress, error}` |

**Upload architecture:** Synchronous flash writes in tcpip_thread context. Lazy 4KB sector erase before first write to each sector. `tcp_recved()` called after each pbuf to maintain TCP flow. `KickIwdg()` called before each sector erase.

## Coding Standards

### Naming Conventions

- **Structures**: `s` + PascalCase (`sAppInfo`, `sUploadSession`)
- **Unions**: `u` + PascalCase (`uDataConverter`)
- **Function pointers**: `f` + PascalCase (`fVerifyImage`)
- **Enums**: `e` + PascalCase for the type, all-caps for values

### Trice Usage

```c
#include "trice.h"
TRice("Message: %d\n", value);  // Standard trace
```

**DO NOT** manually specify IDs — `trice insert` adds them automatically during build.

**TRICE CANNOT BE USED IN lwIP CALLBACKS** (tcpip_thread context). Trice uses PRIMASK critical sections that conflict with lwIP. Use only in FreeRTOS tasks, ISRs, and fault handlers.

### Trice Build Integration

Automatic via CMake:
1. Pre-build: `trice insert` adds IDs to `App/` and `Core/Src/`
2. Compile with IDs embedded
3. Post-build: `trice clean` removes IDs (keeps git clean)

## External Flash Layout

```
0x00000000  Reserved (4KB)
0x00001000  Firmware image area (480KB)
```

W25Q64: 4KB sectors, 64KB blocks, 256-byte pages. Sector erase ~45ms, page write ~0.7ms.

## Key Constraints

- **tcpip_thread stack is only 2048 bytes** — never put large locals (>100B) on it; heap-allocate instead
- Flash writes in tcpip_thread context block TCP processing (~45ms per sector erase) — acceptable with lazy erase
- IWDG must be kicked at least every 16.4s — `KickIwdg()` in default task (100ms) and upload path
- Trice config in `App/triceConfig.h`, DMA overrides in `Core/Src/usart.c`
- Third-party libraries go in `Middlewares/Third_Party/` (STM32CubeMX convention)

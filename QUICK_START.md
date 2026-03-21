# PeriphNet — Quick Start

## What it does

STM32F407VET6 board running FreeRTOS + lwIP with:

- **Web UI** at `http://periphnet.local` — firmware upload/download/install, crash log viewer
- **OTA firmware update** — upload signed binary via browser or curl, bootloader installs on reboot
- **Dual-image bootloader** — HMAC-SHA256 signed images, 4 boot attempts with auto-rollback
- **Crash storage** — fault dumps saved to external flash, viewable via web UI or API
- **Trice logging** — dual output over UART (460800 baud) and UDP broadcast (port 17001)
- **mDNS** — board discoverable as `periphnet.local` on the local network

## Build & Flash

```bash
# Prerequisites: arm-none-eabi-gcc, cmake 3.22+, JLinkExe, python3 (for signing)

cmake -B build -S .
cmake --build build -j8

# Flash app only (daily development)
./flash_nokill.sh flash_application.jlink

# Flash bootloader + app (first time or after BL changes)
./flash_nokill.sh flash_both.jlink

# One-liner
cmake --build build -j8 && ./flash_nokill.sh flash_application.jlink
```

Build output: `build/application.bin` (~168 KB, auto-signed with HMAC-SHA256).

Clean rebuild:
```bash
rm -rf build && cmake -B build -S . && cmake --build build -j8
```

## Access the board

```bash
# Via mDNS (preferred)
curl http://periphnet.local/

# Via IP (DHCP, check your router)
curl http://10.42.0.203/
```

Open the URL in a browser to get the web UI with firmware update controls and crash log.

## HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI (HTML) |
| `/api/firmware/status` | GET | JSON: running version, staged version, transfer state |
| `/api/firmware/upload` | POST | Upload binary to external flash (octet-stream, 480 KB max) |
| `/api/firmware/download` | GET | Download staged image |
| `/api/firmware/install` | POST | Validate staged image, arm FWU flag, reboot |
| `/api/firmware/staged` | DELETE | Erase staged image |
| `/api/crash/latest` | GET | JSON: last crash (registers, backtrace, task list) |
| `/api/crash/latest` | DELETE | Clear stored crash log |

## OTA Update (curl)

```bash
# Upload
curl -X POST -H "Content-Type: application/octet-stream" \
  -H "Content-Length: $(stat -c%s build/application.bin)" \
  --data-binary @build/application.bin http://periphnet.local/api/firmware/upload

# Check status
curl http://periphnet.local/api/firmware/status

# Install (board reboots)
curl -X POST http://periphnet.local/api/firmware/install
```

## Trice Logging

```bash
# UART (USB-to-serial on USART3 PD8/TX, 460800 baud)
trice log -p COM -args "/dev/ttyUSB0:460800" -i ./til.json -li ./li.json

# UDP (no cable needed, broadcasts on port 17001)
trice log -p UDP4 -args ":17001" -i ./til.json -li ./li.json
```

## Trigger a test crash

Press BTN1/BTN2/BTN3 on the board to trigger HardFault/UsageFault/BusFault.
After reboot, view the crash dump at `http://periphnet.local/api/crash/latest` or in the web UI.

## Unit Tests

```bash
cmake -B build_tests -S tests
cmake --build build_tests -j8
ctest --test-dir build_tests -V
```

Test sources in `tests/`, mocks for lwIP/FreeRTOS/W25Q128 in `tests/mocks/`.

## Hardware

- **MCU:** STM32F407VET6 (512 KB flash, 128 KB SRAM, 168 MHz)
- **External Flash:** W25Q64 (8 MB, SPI2 at 21 MHz)
- **Ethernet PHY:** DP83848IVV (RMII)
- **Debugger:** J-Link via SWD
- **Trice UART:** USART3 PD8/TX, DMA1_Stream3, 460800 baud

## Build Sizes

| Target | Flash | RAM | Limit |
|--------|-------|-----|-------|
| Bootloader | ~22 KB (66%) | ~2 KB | 32 KB |
| Application | ~168 KB (34%) | ~99 KB | 480 KB |

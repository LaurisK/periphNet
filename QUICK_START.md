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

Open the URL in a browser to get the web UI with image management + firmware update controls and crash log.

## HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI (HTML) |
| `/api/image/upload` | POST | Upload `.pnfw` blob (octet-stream; optional `X-Filename` header) |
| `/api/image/info` | GET | JSON: stored image name, version, size, CRC32, transfer state |
| `/api/image/download` | GET | Download stored blob |
| `/api/image` | DELETE | Erase stored image |
| `/api/fwu/status` | GET | JSON: running/golden version, confirmed, attempts, last result |
| `/api/fwu/install` | POST | Arm FWU flag (uses stored image), reboot |
| `/api/fwu/confirm` | POST | Confirm running FW; promotes stored image → golden |
| `/api/fwu/verify` | GET | Authenticate running image via BL HMAC |
| `/api/crash/latest` | GET | JSON: last crash (registers, backtrace, task list) |
| `/api/crash/latest` | DELETE | Clear stored crash log |

## OTA Update (curl)

```bash
# Upload the encrypted blob (plaintext .bin is rejected)
curl -X POST -H "X-Filename: periphnet_fwu.pnfw" \
  --data-binary @build/periphnet_fwu.pnfw http://periphnet.local/api/image/upload

# Check what is stored / FWU state
curl http://periphnet.local/api/image/info
curl http://periphnet.local/api/fwu/status

# Install (board reboots), then confirm within 3 boots
curl -X POST http://periphnet.local/api/fwu/install
curl -X POST http://periphnet.local/api/fwu/confirm
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

## USB DFU Flashing (no J-Link needed)

The STM32F407 has a built-in ROM bootloader that supports USB DFU on the USB OTG FS port (PA11/PA12). This lets you flash firmware over USB without a J-Link debugger.

### Prerequisites

```bash
# Install dfu-util
sudo apt install dfu-util        # Debian/Ubuntu
# or
sudo pacman -S dfu-util          # Arch
# or
brew install dfu-util            # macOS
```

### Enter DFU mode

1. Set **BOOT0 = HIGH** (hold the BOOT0 button or move the BOOT0 jumper to 1)
2. Press and release **RESET**
3. Release BOOT0

Verify the board is in DFU mode:
```bash
dfu-util -l
```
You should see a device with `[0483:df11]` — that's the STM32 system bootloader.

### Flash firmware

```bash
# Flash bootloader + application (first time or after BL changes)
dfu-util -a 0 -s 0x08000000:leave -D build/bootloader.bin
dfu-util -a 0 -s 0x08008000:leave -D build/application.bin

# Flash application only (daily development)
dfu-util -a 0 -s 0x08008000:leave -D build/application.bin

# One-liner: build + flash app
cmake --build build -j8 && dfu-util -a 0 -s 0x08008000:leave -D build/application.bin
```

The `:leave` suffix tells the bootloader to jump to the flashed application after download completes — the board starts running immediately without a manual reset.

### Notes

- **Address matters:** the bootloader lives at `0x08000000`, the application at `0x08008000`. Flashing to the wrong address will brick the board (recoverable by re-entering DFU mode).
- **Linux permissions:** if `dfu-util` can't find the device, either run with `sudo` or add a udev rule:
  ```bash
  echo 'SUBSYSTEMS=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", MODE="0666"' \
    | sudo tee /etc/udev/rules.d/99-stm32-dfu.rules
  sudo udevadm control --reload-rules
  ```
- **STM32CubeProgrammer** is an alternative to dfu-util — select "USB" as the connection type and set the start address accordingly.

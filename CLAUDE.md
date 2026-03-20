# CLAUDE.md

## Project Overview

**PeriphNet** is an STM32F407VET6 firmware project. Long-term goal: RS485/Modbus-RTU to Ethernet/MQTT bridge for Solis inverter + Home Assistant, with dual-image OTA bootloader.

**Current phase:** Phase 3 — Bootloader + Application dual build with FWU infrastructure (Zhaga pattern). Crypto stubs (HMAC, AES-GCM) in place for future implementation.

## Build and Flash

```bash
# First time
cmake -B build -S .

# Build both targets
cmake --build build -j8

# Flash application only (development)
./flash_nokill.sh flash_application.jlink

# Flash both bootloader + application
./flash_nokill.sh flash_both.jlink

# Flash bootloader only
./flash_nokill.sh flash_bootloader.jlink

# One-liner (build + flash app)
cmake --build build -j8 && ./flash_nokill.sh flash_application.jlink
```

**Prerequisites:** ARM GCC toolchain, CMake 3.22+, JLinkExe

**Build output:**
- `build/bootloader.elf` / `.bin` — ~13 KB flash, ~1.7 KB RAM (32 KB limit)
- `build/application.elf` / `.bin` — ~137 KB flash, ~98 KB RAM (480 KB limit)

**Clean rebuild:**
```bash
rm -rf build && cmake -B build -S . && cmake --build build -j8
```

## Testing

**Device IP:** 10.42.0.203 (DHCP on 10.42.0.x subnet)

```bash
ping 10.42.0.203
curl http://10.42.0.203/
curl http://10.42.0.203/api/firmware/status

# Upload firmware to external flash (~5.5s for 480KB)
curl -X POST -H "Content-Type: application/octet-stream" \
  -H "Content-Length: $(stat -c%s firmware.bin)" \
  --data-binary @firmware.bin http://10.42.0.203/api/firmware/upload

# Download and verify round-trip
curl http://10.42.0.203/api/firmware/download -o downloaded.bin
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

## Memory Architecture

### Internal Flash (512KB)

```
0x0800_0000  ┌───────────────────┐
             │ Bootloader        │ 32KB (Sectors 0-1)
             │  .isr_vector      │
             │  .text, .rodata   │
0x0800_7F00  │  .bl_api (256B)   │ ← sBootloaderApi function pointer table
0x0800_8000  ├───────────────────┤
             │ Application       │ 480KB (Sectors 2-7)
             │  .isr_vector      │ Vector table (0x188 bytes)
0x0800_8200  │  .app_header      │ ← sAppInfo (version, HMAC, features)
0x0800_8300  │  .text, .rodata   │
0x0808_0000  └───────────────────┘
```

### External Flash (W25Q64 — 8MB SPI)

```
0x0000_0000  ┌───────────────────┐
             │ Boot Status (4KB) │ sBootStatus: FWU flags, staged image metadata
0x0000_1000  ├───────────────────┤
             │ FWU Image (480KB) │ Staged firmware for update
0x0007_9000  ├───────────────────┤
             │ Golden Image      │ 480KB factory fallback (future)
0x000F_1000  ├───────────────────┤
             │ Free              │ ~7.5MB
             └───────────────────┘
```

## Project Structure

```
PeriphNet/
  App/                            # Application modules
    app_freertos.c/h              # FreeRTOS task init, default task, trice task
    system.c/h                    # Reset cause, KickIwdg(), System_Init()
    triceConfig.h                 # Trice configuration (USART3 DMA)
    Log/crash.c/h                 # Cortex-M4 crash handler + backtrace
    Http/
      http_server.c/h             # HTTP request dispatcher (port 80)
      image_transfer.c/h          # Upload/download to external SPI flash
  Core/                           # CubeMX generated + shared drivers
    Inc/
      bl_app_contract.h           # BL↔APP contract: addresses, magics, API struct
      dfu_types.h                 # FWU types: sFwVer, sAppInfo, sBootStatus, eFwuRes
      version.h                   # Version formatting & compatibility checking
      boot_status.h               # Boot status API (ext flash flags)
      image_mgmt.h                # Image validation API
      w25q128.h                   # External flash driver
    Src/
      version.c                   # ver_toString(), ver_checkCompatibility()
      boot_status.c               # Boot flag read/write with NOR bit-clearing
      image_mgmt.c                # ImgMgmt_Validate(), CRC32, flash read helpers
      w25q128.c                   # W25Q64/128 SPI driver
      main.c, gpio.c, spi.c ...  # CubeMX peripherals
    Startup/startup_stm32f407vetx.s
  bootloader/                     # Bootloader (32KB, no RTOS/network)
    boot_main.c                   # Entry point, boot flow, jump-to-app
    boot_api.c                    # API table at 0x08007F00 (.bl_api section)
    boot_stm32f4xx_it.c           # Minimal ISRs (faults → while(1), SysTick)
    bootloader.ld                 # Linker: 0x08000000, 32KB + BL_API region
  application/                    # Application metadata + linker
    app_info.c                    # sAppInfo const in .app_header section
    application.ld                # Linker: 0x08008000, 480KB + APP_HEADER region
  Drivers/                        # STM32 HAL + CMSIS (vendor)
  LWIP/                           # lwIP integration (CubeMX)
  Middlewares/Third_Party/
    FreeRTOS/                     # RTOS
    LwIP/                         # TCP/IP stack
    trice/                        # Trice library (git submodule, uartDma branch)
    backtrace/                    # Cortex-M4 FP unwinder
  CMakeLists.txt                  # Dual-target build (bootloader.elf + application.elf)
  flash_nokill.sh                 # J-Link clone flash wrapper
  flash_both.jlink                # Flash BL + APP
  flash_application.jlink         # Flash APP only
  flash_bootloader.jlink          # Flash BL only
```

**Files grouped by module/functionality, NOT by file type.** Keep .c and .h together. Third-party libraries go in `Middlewares/Third_Party/`.

## Firmware Version (Zhaga Pattern)

### Version Structure

```c
typedef struct {
    uint8_t  deviceType;   /* 'P' = PeriphNet                          */
    uint8_t  target;       /* 'v' release, 'd' dev, 'l' local          */
    uint16_t major;
    uint8_t  minor;
    uint8_t  patch;
    uint16_t hwId;         /* hardware variant (0 = generic)            */
} __attribute__((packed)) sFwVer;   /* 8 bytes */

typedef struct {
    sFwVer  ver;
    uint8_t reserved[24];
} __attribute__((packed)) sFwVerArea;  /* 32 bytes, padded for flash alignment */
```

**String format:** `Pv1.2.3` or `Pv1.2.3_ABCD` (with hwId). Use `ver_toString()`.

### Application Info Header (sAppInfo at 0x08008200)

```c
typedef struct {
    uint32_t    magic;                      /* 0x41505049 "APPI"             */
    sFwVerArea  fw_version;                 /* 32B firmware version          */
    uint32_t    image_size;                 /* binary size (0xFFFFFFFF=raw)  */
    uint8_t     image_hmac[32];             /* HMAC-SHA256 (stub: 0xFF)     */
    uint32_t    features;                   /* APP_FEATURE_* flags           */
    uint32_t    min_bl_version;             /* minimum BL API version        */
    uint32_t    reserved[8];
} sAppInfo;   /* 112 bytes, placed by linker in .app_header section */
```

Current version defined in `application/app_info.c`. The `image_size` and `image_hmac` are placeholders for a future post-build patching tool.

### Version Compatibility (ver_checkCompatibility)

- Device type must match
- Hardware ID must match
- Local-target (`'l'`) builds skip version ordering (allow any update)
- Otherwise incoming version must be strictly newer (major.minor.patch)

## Firmware Update (FWU) Architecture

### Boot Status (sBootStatus in ext flash sector 0)

Uses **NOR flash bit-clearing semantics** — erased state is 0xFF (all 1s), individual bits can be cleared to 0 without erase.

```c
typedef union {
    uint32_t word;              /* erased = 0xFFFFFFFF                   */
    struct {
        uint32_t fwu_requested  : 1;  /* 0 = FWU requested by APP       */
        uint32_t confirmed      : 1;  /* 0 = APP confirmed healthy      */
        uint32_t boot_attempt_0 : 1;  /* 0 = 1st unconfirmed boot used  */
        uint32_t boot_attempt_1 : 1;  /* 0 = 2nd                        */
        uint32_t boot_attempt_2 : 1;  /* 0 = 3rd                        */
        uint32_t boot_attempt_3 : 1;  /* 0 = 4th → triggers rollback    */
    } bits;
} sBootFlags;
```

The `sBootStatus` header contains: magic (`"BOOT"`), staged image metadata (size, CRC32, version), header CRC32, and flags. Flags are **outside the CRC** so they can be modified independently.

### Bootloader Boot Flow

```
1. Init hardware (GPIO, SPI)
2. Init external flash (W25Q64)
3. Ensure boot status sector has valid header
4. Read FWU action from flags:
   ├─ fwu_install:  validate staged image → install (stub) → clear flags
   ├─ fwu_rollback: boot attempts exhausted → rollback (stub)
   └─ fwu_none:     if unconfirmed → consume boot attempt
5. Validate internal application (magic + size check)
6. Jump to application at 0x08008000
```

### FWU Update Flow (Application → Bootloader)

```
Application side:
  1. Receive firmware via HTTP POST /api/firmware/upload → ext flash
  2. Read sAppInfo from staged image → extract version
  3. Check version compatibility (ver_checkCompatibility)
  4. Call BootStatus_RequestFwu(size, crc, version) — arms FWU flag
  5. Reboot (NVIC_SystemReset)

Bootloader side:
  1. Detect fwu_requested flag
  2. Validate staged image (ImgMgmt_Validate: magic + size + HMAC stub)
  3. Check version compatibility
  4. Install: erase app sectors → copy ext flash → internal flash (stub)
  5. Clear flags → jump to new application
```

### Boot Attempt Counter & Rollback

After FWU install, the new image is **unconfirmed**. Each boot without confirmation consumes one of 4 boot attempts. If all 4 are exhausted, the bootloader triggers a rollback.

The application must call `BootStatus_ConfirmApp()` after successful boot to mark itself healthy. This clears the `confirmed` flag bit (NOR bit-clear, no erase needed).

### Image Validation (ImgMgmt_Validate)

1. Read `sAppInfo` from image at offset 0x200
2. Check `magic == APP_INFO_MAGIC` (0x41505049)
3. Check `image_size` is valid (non-zero, not exceeding 480KB)
4. HMAC-SHA256 verification (**stub: always passes**)

Works on both internal flash (memory-mapped) and external SPI flash.

## Bootloader API (sBootloaderApi at 0x08007F00)

Function pointer table placed in `.bl_api` linker section. API version 2.

| Function | Signature | Status |
|----------|-----------|--------|
| `get_bl_version` | `void (uint32_t *major, *minor, *patch)` | Implemented |
| `verify_internal_app` | `int (void)` → BL_OK / BL_WRONG_MAGIC | Implemented |
| `verify_hmac` | `bool (data, size, expected[32])` | **Stub** (always true) |
| `decrypt_blob` | `eFwuRes (data, size)` | **Stub** (always OK) |

**Application usage:**
```c
const sBootloaderApi *bl = (const sBootloaderApi *)BL_API_TABLE_ADDR;
if (bl->magic == BL_API_MAGIC) {
    uint32_t maj, min, pat;
    bl->get_bl_version(&maj, &min, &pat);
}
```

**Note:** BL API functions must not rely on bootloader global state (RAM overwritten by application). Current functions use only stack, const data, and memory-mapped flash reads.

## Boot Status API (boot_status.h)

Shared code compiled into both bootloader and application.

| Function | Description |
|----------|-------------|
| `BootStatus_Read(status)` | Read boot status from ext flash |
| `BootStatus_Write(status)` | Erase sector + write full status |
| `BootStatus_EnsureValid()` | Create default header if corrupt |
| `BootStatus_RequestFwu(size, crc, ver)` | Arm FWU flag with staged image metadata |
| `BootStatus_ConfirmApp()` | Confirm healthy boot (NOR bit-clear) |
| `BootStatus_ConsumeBootAttempt()` | Use one boot attempt (BL calls this) |
| `BootStatus_IsUnconfirmed()` | Check if APP confirmation is pending |
| `BootStatus_GetFwuAction()` | Determine BL action: install/rollback/none |
| `BootStatus_ClearFlags()` | Reset all flags (rewrite with 0xFFFFFFFF) |

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
| `/api/firmware/status` | GET | JSON: `{status, bytes_transferred, total_bytes, progress, error}` |

**Upload architecture:** Synchronous flash writes in tcpip_thread context. Lazy 4KB sector erase before first write to each sector. `tcp_recved()` called after each pbuf to maintain TCP flow. `KickIwdg()` called before each sector erase.

## Coding Standards

### Naming Conventions

- **Structures**: `s` + PascalCase (`sAppInfo`, `sBootStatus`)
- **Unions**: `u` + PascalCase (`sBootFlags` uses union internally)
- **Function pointers**: `f` + PascalCase (`fVerifyHmac`, `fDecryptBlob`)
- **Enums**: `e` + PascalCase for type (`eFwuRes`, `eFwTarget`)

### Trice Usage

```c
#include "trice.h"
TRice("Message: %d\n", value);
```

**DO NOT** manually specify IDs — `trice insert` adds them automatically during build.

**TRICE CANNOT BE USED IN lwIP CALLBACKS** (tcpip_thread context). Use only in FreeRTOS tasks, ISRs, and fault handlers.

## Key Constraints

- **Bootloader must fit in 32KB** — no FreeRTOS, no lwIP, no Trice. Monitor size.
- **Application starts at 0x08008000** — VTOR relocation via `APPLICATION_BUILD` define in `system_stm32f4xx.c`
- **BL API at 0x08007F00** — fixed address, function pointers must not use BL globals
- **tcpip_thread stack is only 2048 bytes** — heap-allocate large structs
- **IWDG must be kicked every 16.4s** — `KickIwdg()` in default task + upload path
- **NOR flash bit-clearing** — boot flags can be modified without sector erase (1→0 only)
- **Crypto stubs** — HMAC and AES-GCM not yet implemented. Image validation currently checks magic + size only.

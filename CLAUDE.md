# CLAUDE.md

## Project Overview

**PeriphNet** is an STM32F407VET6 firmware project. Long-term goal: RS485/Modbus-RTU to Ethernet/MQTT bridge for Solis inverter + Home Assistant, with dual-image OTA bootloader.

**Current phase:** Encrypted FWU pipeline (Zhaga pattern, extended). Firmware is distributed only as encrypted+authenticated `.pnfw` blobs; the bootloader does streaming AES-128-GCM decrypt + HMAC verify during install, with confirm/rollback via a golden image. HMAC, AES-128 and GCM are real, NIST-vector-tested implementations (not stubs).

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
- `build/bootloader.elf` / `.bin` — ~23 KB flash, ~2.7 KB RAM (32 KB limit)
- `build/application.elf` / `.bin` — ~229 KB flash, ~105 KB RAM (480 KB limit);
  the `.bin` is signed in-place (IMAGE_SIZE + HMAC patched) after every build
- `build/periphnet_full.hex` — BL + signed APP combined, factory/initial J-Link write
- `build/periphnet_fwu.pnfw` — encrypted+authenticated blob, the ONLY artifact
  used for OTA (needs python3 `cryptography` + `intelhex` packages)

**FWU keys:** `bootloader/secrets.c` holds committed DEVELOPMENT keys (matching
the defaults in `tools/dfu_image_tool.py`). For production: create gitignored
`bootloader/secrets_prod.c` (auto-picked by CMake) and export
`DFU_AES_KEY`/`DFU_HMAC_KEY` for the build tool. Keys are known ONLY to the
build process and the bootloader — never stored in ext flash, never linked
into the application.

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

# Full OTA cycle (blob only — plaintext .bin uploads are rejected)
curl -X POST --data-binary @build/periphnet_fwu.pnfw \
  http://10.42.0.203/api/firmware/upload
curl -X POST http://10.42.0.203/api/firmware/install   # arms FWU + reboots
# ...device reboots, BL installs, new FW comes up UNCONFIRMED...
curl http://10.42.0.203/api/firmware/status            # check health/version
curl -X POST http://10.42.0.203/api/firmware/confirm   # REQUIRED within 3 boots,
                                                       # also promotes staged→golden

# Download staged blob / verify round-trip
curl http://10.42.0.203/api/firmware/download -o downloaded.pnfw
md5sum build/periphnet_fwu.pnfw downloaded.pnfw
```

**IMPORTANT:** without `confirm`, the bootloader rolls back to the golden
image after 3 unconfirmed boots. Local-target (`'l'`) builds are exempt from
attempt counting, so JLink dev flashing is unaffected.

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

Both blob areas hold encrypted `.pnfw` blobs — firmware is never stored in
plaintext outside internal flash. Each area is self-describing (cleartext
manifest + trailing CRC32), so no metadata lives in the boot status.

```
0x0000_0000  ┌───────────────────┐
             │ Boot Status (4KB) │ sBootStatus v3: flags + last_fwu_result
0x0000_1000  ├───────────────────┤
             │ Staged Blob       │ 488KB — uploaded .pnfw awaiting install
0x0007_B000  ├───────────────────┤
             │ Golden Blob       │ 488KB — last CONFIRMED image (encrypted),
0x000F_5000  ├───────────────────┤          rollback target
             │ (gap)             │
0x000F_8000  ├───────────────────┤
             │ Crash Log (4KB)   │
0x000F_9000  ├───────────────────┤
             │ Free              │ ~7.4MB
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
      http_server.c/h             # HTTP server task (netconn API, port 80)
      image_transfer.c/h          # FWU staging/control domain logic (no lwIP)
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

### FWU Blob (.pnfw) Format

```
[0x00] sFwuManifest (64B)   cleartext: magic "PNFW", format, fw_version,
                            image_size, blob_size — authenticated as GCM AAD
[0x40] GCM nonce (12B)
[0x4C] ciphertext           AES-128-GCM over the SIGNED plaintext binary
[...]  GCM tag (16B)
[...]  CRC32 (4B)           over everything before it — keyless transfer check
```

blob_size = image_size + 96. The app treats blobs as opaque: it checks only
manifest + CRC32; all crypto (tag, HMAC, version gate) is bootloader-side.

### Boot Status (sBootStatus v3 in ext flash sector 0)

Uses **NOR flash bit-clearing semantics** — erased state is 0xFF (all 1s), individual bits can be cleared to 0 without erase.

```c
typedef union {
    uint32_t word;              /* erased = 0xFFFFFFFF                   */
    struct {
        uint32_t fwu_requested  : 1;  /* 0 = FWU requested by APP       */
        uint32_t confirmed      : 1;  /* 0 = outside actor confirmed    */
        uint32_t boot_attempt_0 : 1;  /* 0 = 1st unconfirmed boot used  */
        uint32_t boot_attempt_1 : 1;  /* 0 = 2nd                        */
        uint32_t boot_attempt_2 : 1;  /* 0 = 3rd → triggers rollback    */
    } bits;
} sBootFlags;
```

`sBootStatus` v3 is minimal: magic (`"BOOT"`), version, `last_fwu_result`
(eFwuRes of the last install/rollback, `FWU_NO_RESULT` if none — exposed via
`/api/firmware/status` so BL-side failures are diagnosable), header CRC32
(verified on read), and flags. Flags are **outside the CRC** so they can be
bit-cleared independently. Staged/golden metadata lives in the blob
manifests, not here.

### Bootloader Boot Flow

```
1. Init hardware (GPIO, SPI)
2. Init external flash (W25Q64)
3. Ensure boot status sector has valid header
4. Read FWU action from flags:
   ├─ fwu_install:  streaming blob install from staged area
   │     pass 0: manifest sanity + whole-blob CRC32
   │     pass 1: stream GCM decrypt (discarded) → verify tag + plaintext
   │             HMAC + capture decrypted sAppInfo; version gate (skipped
   │             if internal app header invalid — blank device accepts
   │             any authentic image)
   │     pass 2: erase sectors 2-7 → decrypt again → program → verify
   │     success → FinishFwu(FWU_OK, unconfirmed) + consume 1st attempt
   │     failure → FinishFwu(result, pre-confirmed) → boot old app
   ├─ fwu_rollback: same install from GOLDEN area (no version gate),
   │     erase staged manifest, FinishFwu(FWU_ROLLBACK, pre-confirmed)
   └─ fwu_none:     if unconfirmed → consume boot attempt
                    (local-target 'l' builds exempt)
5. Validate internal application (magic + size + HMAC-SHA256)
6. Jump to application at 0x08008000
```

Internal flash is never touched before the image authenticates (pass 1), so
a power cut mid-install retries cleanly on next boot.

### FWU Update Flow

```
Build:    application.bin → sign (HMAC) → package → periphnet_fwu.pnfw

Operator: POST /api/firmware/upload  (blob → staged area; independent of FWU)
          POST /api/firmware/install (arms fwu_requested flag → reboot)
          ...BL installs, new FW boots UNCONFIRMED...
          verify health via /api/firmware/status, MQTT, etc.
          POST /api/firmware/confirm (REQUIRED — clears confirmed bit AND
                                      promotes staged blob → golden area)
```

Transfer and FWU are fully independent: the staged blob is persistent and
self-describing (rescanned at boot), installing needs no upload session, and
an upload alone never triggers an install.

### Boot Attempt Counter & Rollback

After FWU install the new image is **unconfirmed** (install consumes attempt
1 of 3). Each further unconfirmed boot consumes another. When all 3 are
gone, the BL installs the **golden blob** (last confirmed image, kept
encrypted in ext flash), marks it pre-confirmed, and erases the staged
manifest so the failed image can't be re-installed by accident.

The app never self-confirms — `POST /api/firmware/confirm` is the outside
actor's job. Local-target (`'l'`) builds skip attempt counting entirely
(developer owns the device; JLink flashing stays friction-free).

## Bootloader API (sBootloaderApi at 0x08007F00)

Function pointer table placed in `.bl_api` linker section. API version 3.
All crypto uses real, NIST-vector-tested implementations.

| Function | Signature | Notes |
|----------|-----------|-------|
| `get_bl_version` | `void (uint32_t *major, *minor, *patch)` | |
| `verify_internal_app` | `int (void)` → BL_OK / BL_WRONG_MAGIC | |
| `verify_hmac` | `bool (data, size, expected[32])` | HMAC-SHA256, RAM buffer |
| `decrypt_blob` | `eFwuRes (data, size)` | AES-128-GCM, RAM blob |
| `verify_image_hmac` | `eFwuRes (addr, is_external, size, expected[32])` | streaming; app-usable for internal flash only (BL SPI globals unavailable to app) |

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
| `BootStatus_Read(status)` | Read + validate (magic, version, header CRC) |
| `BootStatus_Write(status)` | Erase sector + write full status |
| `BootStatus_EnsureValid()` | Create default header if corrupt |
| `BootStatus_RequestFwu()` | Arm FWU flag (NOR bit-clear, no metadata) |
| `BootStatus_ConfirmApp()` | Confirm healthy boot (NOR bit-clear) |
| `BootStatus_ConsumeBootAttempt()` | Use one boot attempt (BL calls this) |
| `BootStatus_IsUnconfirmed()` | Check if actor confirmation is pending |
| `BootStatus_GetFwuAction()` | Determine BL action: install/rollback/none |
| `BootStatus_FinishFwu(result, pre_confirmed)` | Record result + fresh flags |
| `BootStatus_GetFlags(flags)` / `BootStatus_AttemptsRemaining()` | Flag inspection |

## FreeRTOS Tasks

| Task | Stack | Priority | Role |
|------|-------|----------|------|
| defaultTask | 1024 words | osPriorityNormal (24) | Init, heartbeat (1s), IWDG kick (100ms), reboot/promotion jobs, button faults |
| http | 1024 words | osPriorityNormal (24) | HTTP server (netconn API, sequential connections) |
| trice | 256 words | osPriorityNormal+1 (25) | TriceTransfer() every 10ms |
| tcpip_thread | 2048 bytes | 24 | lwIP TCP/IP processing |
| EthIf | 1024 bytes | 3 | Ethernet frame receive |

## HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web UI (upload/install/confirm/delete + crash log) |
| `/api/firmware/upload` | POST | Upload `.pnfw` blob to staged area (Content-Length required) |
| `/api/firmware/download` | GET | Download staged blob (still encrypted) |
| `/api/firmware/install` | POST | Arm FWU flag + reboot (needs valid staged blob) |
| `/api/firmware/confirm` | POST | Outside actor confirms running FW; promotes staged→golden |
| `/api/firmware/verify` | GET | Authenticate RUNNING image via BL HMAC (no FWU state change) |
| `/api/firmware/staged` | DELETE | Erase staged blob manifest |
| `/api/firmware/status` | GET | JSON: status, running/staged/golden versions, confirmed, attempts_remaining, last_fwu_result, progress, error |
| `/api/crash/latest` | GET/DELETE | Crash log read / clear |

**Server architecture:** dedicated `http` task using the lwIP **netconn API**
(one connection at a time; 10s recv/send timeouts so dead clients can't stall
it). `App/Http/http_server.c` owns all HTTP parsing/JSON; `image_transfer.c`
is protocol-agnostic domain logic (blobs, flash, boot status) with an
`img_upload_begin/write/finish` streaming API. Headers are read through a
byte-stream cursor, so TCP segmentation cannot break parsing; `Expect:
100-continue` is answered (no curl stall). Upload does synchronous flash
writes with lazy 4KB sector erase; on completion the blob is validated
(manifest + CRC32) and becomes the persistent staged image. tcpip_thread is
never blocked by firmware transfers (MQTT keepalives unaffected). Golden
promotion runs in defaultTask; the W25Q128 driver serializes SPI access with
a mutex (skipped in BL and fault-handler context).

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

**TRICE CANNOT BE USED IN lwIP CALLBACKS** (tcpip_thread context — e.g. MQTT client callbacks). Use only in FreeRTOS tasks (the netconn-based HTTP task is fine), ISRs, and fault handlers.

## Key Constraints

- **Bootloader must fit in 32KB** — no FreeRTOS, no lwIP, no Trice. Currently ~23KB. Monitor size.
- **Application starts at 0x08008000** — VTOR relocation via `APPLICATION_BUILD` define in `system_stm32f4xx.c`
- **BL API at 0x08007F00** — fixed address, function pointers must not use BL globals
- **tcpip_thread stack is only 2048 bytes** — heap-allocate large structs
- **IWDG must be kicked every 16.4s** — `KickIwdg()` in default task + upload/scan/promotion paths
- **NOR flash bit-clearing** — boot flags can be modified without sector erase (1→0 only)
- **FWU keys are build+BL only** — never store keys in ext flash, never link `secrets.c` into the application, never expose key material through the BL API
- **OTA accepts only .pnfw blobs** — plaintext binaries are rejected at upload (manifest check); plaintext exists only in `build/` and internal flash
- **Confirm or roll back** — non-local builds must be confirmed via `POST /api/firmware/confirm` within 3 boots of an install, otherwise the BL restores the golden image
- **No raw lwIP callbacks for app code** — the HTTP server uses the netconn API in its own task; if raw callbacks are ever needed again, remember the recv-callback contract (return ERR_OK after consuming a pbuf, or tcp_abort + ERR_ABRT — anything else makes lwIP re-deliver a freed pbuf)

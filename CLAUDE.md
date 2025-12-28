# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**PeriphNet** is an STM32F407VET6-based industrial firmware project implementing:
- RS485 (Modbus RTU) to Ethernet (MQTT) bridge for Solis inverter integration with Home Assistant
- Dual-image bootloader with OTA firmware updates via external flash
- FreeRTOS-based architecture with lwIP TCP/IP stack
- Trice tracing over TCP/IP

## Development Status

### 📅 Latest Updates (Dec 27, 2024)

**Milestone 2 Phase 2 Completed:** External Flash Driver Verified
- ✅ Fixed GPIO Pin 6 configuration issue (ANALOG → OUTPUT)
- ✅ Removed dead code from http_server.c and boot_main.c
- ✅ Enhanced flash test with detailed step-by-step debugging
- ✅ **Root cause analysis:** Chip is W25Q64 (8MB), not W25Q128 (16MB)
- ✅ Updated driver to support both W25Q64 (0x17) and W25Q128 (0x18)
- ✅ All flash tests passing: Init, JEDEC ID, Write/Read, Erase
- ✅ HTTP test interface working at http://10.42.0.203/
- ✅ Verified compatibility with STM32CubeMX auto-generation

**Build Status:**
- Bootloader: 13.9 KB / 32 KB (43.4% used)
- Application: 125.0 KB / 480 KB (26.0% used)
- Both verified and running on hardware

**Next Step:** Implement Phase 3 - BL-APP Contract (API + Headers)

### ✅ Milestone 1: Ethernet + HTTP Server (COMPLETED - Dec 26, 2024)

**Achievements:**
- ✅ STM32F407VET6 board configured and running
- ✅ FreeRTOS integrated and operational
- ✅ lwIP TCP/IP stack integrated with RMII Ethernet
- ✅ DHCP client functional (obtains IP: 10.42.0.203)
- ✅ ARP, ICMP (ping) responding correctly
- ✅ HTTP server serving web pages on port 80
- ✅ CMake build system configured and working
- ✅ J-Link flash automation operational

**Key Fixes Applied:**
- Increased FreeRTOS heap from 15KB → 40KB (configTOTAL_HEAP_SIZE)
- Increased EthIf thread stack from 350 → 1024 words
- Fixed TCP error handling (added tcp_err callback)
- Resolved buffer overflow in HTTP response handling
- Fixed network initialization timing issues

**Current Configuration:**
- Build: CMake + ARM GCC toolchain
- Flash: 121KB / 512KB (23.6%)
- RAM: 89KB / 128KB (68.0%)
- Network: DHCP on 10.42.0.x subnet
- HTTP: Simple HTML page at http://10.42.0.203

### 🔄 Milestone 2: Bootloader-Application Separation (IN PROGRESS)

**Current Status (Dec 27, 2024):**
- ✅ Phase 1: Complete - Dual build system working
- ✅ Phase 2: Complete - Flash driver verified on actual hardware
- ⏳ Phase 3: Next - Implement BL-APP contract
- ⏸️ Phase 4: Pending - Update mechanism

**Phase 1: Dual Build System & Boot Jump** ✅ COMPLETED
- ✅ Split build into bootloader + application CMake targets
- ✅ Bootloader linker script (0x08000000, 32KB max)
- ✅ Application linker script (0x08008000, 480KB max)
- ✅ Bootloader jumps to application at 0x08008000
- ✅ Application relocates VTOR and runs FreeRTOS/lwIP
- ✅ Simple "Hello from Bootloader" → "Hello from Application" flow

**Phase 2: Common External Flash Driver** ✅ COMPLETED (Dec 27, 2024)
- ✅ W25Qxx SPI driver in `Core/Src/` (shared code)
- ✅ Supports both W25Q64 (8MB) and W25Q128 (16MB)
- ✅ **Actual chip detected: W25Q64 (8MB)** - JEDEC ID: 0xEF/0x40/0x17
- ✅ Basic read/write/erase operations verified
- ✅ Compiled into both bootloader AND application
- ✅ HTTP-based flash test with step-by-step debugging
- ✅ Full test suite: Init, JEDEC ID, Write/Read cycle, Sector erase
- ✅ Test results: http://10.42.0.203/ - All 4 tests PASSING

**Phase 3: BL-APP Contract (API + Headers)** (NEXT)
- ⏸️ Bootloader API table structure at 0x08007F00
- ⏸️ Application info header at 0x08008200
- ✅ Shared header: `Core/Inc/bl_app_contract.h` (created, needs implementation)
- ⏸️ CRC32 calculation functions
- ⏸️ Application can call bootloader API to verify images

**Phase 4: Update Mechanism** (FUTURE)
- ⏸️ Update status structure in external flash (0x00000000)
- ⏸️ Bootloader reads update flag on boot
- ⏸️ Firmware verification and installation
- ⏸️ Golden image fallback support

### 📋 Milestone 3: OTA Firmware Updates (FUTURE)

**Goals:**
- HTTP firmware upload endpoint
- Firmware metadata extraction
- Download to external flash
- Verification via bootloader API
- Scheduled installation support
- Golden image fallback mechanism

### 📋 Milestone 4: Modbus RTU Bridge (FUTURE)

**Goals:**
- RS485 driver implementation
- Modbus RTU protocol stack
- Solis inverter register mapping
- MQTT client integration
- Data publishing to Home Assistant

## Build and Flash Instructions

**Build system: CMake + ARM GCC** ✅
**Flash tool: J-Link (with clone popup workaround)** ✅

### Prerequisites

- ARM GCC toolchain installed
- CMake 3.10+
- J-Link software installed (JLinkExe)
- STM32F407VET6 board connected via J-Link

### Complete Workflow

**All commands run from project root: `/home/laurynas/Projects/PeriphNet`**

**Quick Reference:**
```bash
# First time setup
cmake -B build -S .

# Daily workflow
cmake --build build -j8                           # Build
./flash_nokill.sh flash_application.jlink        # Flash

# Or one-liner
cmake --build build -j8 && ./flash_nokill.sh flash_application.jlink
```

#### 1. Configure (First Time Only)

```bash
# From project root
cmake -B build -S .
```

**Options:**
- `-B build` - Build directory
- `-S .` - Source directory (current directory)

#### 2. Build Firmware

**Build both bootloader and application:**
```bash
# From project root
cmake --build build -j8
```

**Build specific targets:**
```bash
# Build bootloader only
cmake --build build --target bootloader.elf -j8

# Build application only
cmake --build build --target application.elf -j8

# Clean build
cmake --build build --target clean
```

**Build output files** (in `build/` directory):
- `bootloader.bin` - Bootloader binary (32KB max)
- `bootloader.elf` - Bootloader with debug symbols
- `application.bin` - Application binary (480KB max)
- `application.elf` - Application with debug symbols

#### 3. Flash to Board

**⚠️ IMPORTANT: Clone J-Link Popup Issue**

This project uses a clone J-Link which shows a popup warning that blocks flashing. Use the provided wrapper script to avoid manual intervention.

**Flash both bootloader and application (recommended):**
```bash
# From project root
./flash_nokill.sh flash_both.jlink
```

**Flash application only (during development):**
```bash
# From project root
./flash_nokill.sh flash_application.jlink
```

**Flash bootloader only (rare):**
```bash
# From project root
./flash_nokill.sh flash_bootloader.jlink
```

**Timing:**
- Full flash (both): ~11 seconds
- Application only: ~10 seconds
- Bootloader only: ~10 seconds

**What the script does:**
1. Runs JLinkExe with specified .jlink script
2. Monitors output for "Verify successful"
3. Kills process after completion (avoids popup hang)
4. Returns success/failure status

**Alternative (not recommended):**
```bash
# Direct JLinkExe usage - will hang on popup, requires manual click
JLinkExe -CommandFile flash_both.jlink
```

### J-Link Script Files

The project includes three J-Link command scripts:

**`flash_both.jlink`** - Flash both bootloader and application
```
si SWD                                      # Select SWD interface
speed 4000                                  # 4MHz speed
device STM32F407VE                          # Target MCU
r                                           # Reset
h                                           # Halt
erase                                       # Full chip erase
loadfile build/bootloader.bin 0x08000000    # Flash bootloader at 0x08000000
loadfile build/application.bin 0x08008000   # Flash application at 0x08008000
verifybin build/bootloader.bin 0x08000000   # Verify bootloader
verifybin build/application.bin 0x08008000  # Verify application
r                                           # Reset
go                                          # Run
exit
```

**`flash_application.jlink`** - Flash application only
```
erase 0x08008000 0x0807FFFF                 # Erase application sectors only
loadfile build/application.bin 0x08008000
verifybin build/application.bin 0x08008000
```

**`flash_bootloader.jlink`** - Flash bootloader only
```
erase                                       # Full chip erase
loadfile build/bootloader.bin 0x08000000
verifybin build/bootloader.bin 0x08000000
```

### Typical Development Cycle

**All commands from project root:**

```bash
# 1. Make code changes
vim Core/Src/http_server.c

# 2. Build
cmake --build build -j8

# 3. Flash application only (faster during development)
./flash_nokill.sh flash_application.jlink

# 4. Test
ping 10.42.0.203
curl http://10.42.0.203/
```

**One-liner for quick iterations:**
```bash
cmake --build build -j8 && ./flash_nokill.sh flash_application.jlink
```

### Build Output Details

**Bootloader:**
- Size limit: 32 KB (sectors 0-1)
- Current size: ~14 KB
- Location: 0x08000000
- No network stack (minimal code)

**Application:**
- Size limit: 480 KB (sectors 2-7)
- Current size: ~125 KB
- Location: 0x08008000
- Full network stack (FreeRTOS + lwIP)

**Memory usage check (from project root):**
```bash
arm-none-eabi-size build/bootloader.elf
arm-none-eabi-size build/application.elf
```

### Troubleshooting

**Build fails:**
- Check ARM GCC toolchain: `arm-none-eabi-gcc --version`
- Clean and rebuild: `cmake --build build --target clean && cmake -B build -S . && cmake --build build -j8`

**Flash fails:**
- Check J-Link connection: `JLinkExe` (should connect)
- Verify USB cable connected
- Check board power
- Try manual reset button on board

**Device doesn't boot after flash:**
- Reflash both: `./flash_nokill.sh flash_both.jlink`
- Check bootloader LED sequence (3 blinks = bootloader running)
- Verify network: `ping 10.42.0.203`

**Clone J-Link popup appears:**
- Use `flash_nokill.sh` instead of direct JLinkExe
- See `FLASH_CLONE_JLINK.md` for details

### Documentation

- `FLASH_CLONE_JLINK.md` - J-Link clone popup solution details
- `CLAUDE.md` - This file (project overview and instructions)
- `README.md` - Project README (if exists)

## Memory Architecture

### Internal Flash (512KB)
```
0x0800 0000  ┌──────────────┐
             │ Bootloader   │ 32KB (Sectors 0-1)
0x0800 8000  ├──────────────┤
             │ Application  │ 480KB (Sectors 2-7)
0x0808 0000  └──────────────┘
```

**Critical constraints:**
- Bootloader MUST fit in 32KB (Sectors 0-1)
- Application starts at 0x08008000
- Bootloader API table located at 0x08007F00
- Application max size: 480KB

### External Flash (W25Q128 - 16MB SPI)
```
0x0000 0000  ┌──────────────┐
             │ FWU Status   │ 4KB
0x0000 1000  ├──────────────┤
             │ FWU Image    │ 480KB (downloaded firmware)
0x0007 9000  ├──────────────┤
             │ Golden Image │ 480KB (factory/known-good)
0x000F 1000  ├──────────────┤
             │ Undefined    │ ~15MB (future use)
             └──────────────┘
```

### EEPROM (AT24C02BN - 256 bytes I2C)
- Boot counter, hardware ID, factory config, network backup

## Bootloader & Application Separation

This firmware uses a **separation of concerns** architecture for OTA updates:

### Bootloader (32KB, no network stack)
- Reads update flag from external flash
- Verifies and installs firmware from external flash to internal flash
- Supports golden image fallback
- Exposes verification API at fixed address (0x08007F00) for application use
- Minimal, safety-critical code only

### Application (480KB, with network stack)
- Downloads firmware via Ethernet (HTTP/TFTP)
- Calls bootloader API to verify image before update
- Writes firmware to external flash
- Sets update flag and reboots

### Bootloader API (0x08007F00)
```c
typedef struct {
    uint32_t magic;  // 0x424C4150 ("BLAP")
    uint32_t version;

    int (*verify_image)(uint32_t ext_flash_addr, uint32_t size, uint32_t expected_crc);
    uint32_t (*calculate_crc32)(uint32_t addr, uint32_t size, bool is_external_flash);
    void (*get_bootloader_version)(uint32_t *major, uint32_t *minor, uint32_t *patch);
    int (*verify_internal_app)(void);
} bootloader_api_t;
```

**Application usage pattern:**
```c
const bootloader_api_t *bl_api = (const bootloader_api_t*)0x08007F00;
int result = bl_api->verify_image(EXT_FLASH_FWU_IMG_ADDR, size, expected_crc);
if (result == 0) {
    write_update_status(...);
    NVIC_SystemReset();
}
```

## BL-APP Contract Architecture

### Build Strategy

**Separate builds within same project:**
- Bootloader and application are separate CMake targets
- Share common code from `Core/` directory (drivers, utilities)
- Each has its own linker script and memory region
- Common drivers (SPI, flash, CRC) compiled for both targets

### Bidirectional Communication Contract

**1. Bootloader → Application (Function Pointer Table)**

Bootloader exposes API at fixed address 0x08007F00 for application to call:
- `verify_image()` - Verify firmware in external flash
- `calculate_crc32()` - Calculate CRC32 of memory regions
- `get_bootloader_version()` - Query bootloader version
- Encryption/decryption functions (secrets stay in BL only!)

**2. Application → Bootloader (Application Info Header)**

Application provides metadata at fixed address 0x08008200 (right after vector table):

```
Memory Layout:
0x08008000: Vector table start (STM32F407: ~392 bytes for 98 vectors)
0x08008200: Application info header (app_info_t structure)
0x08008xxx: Application code continues...
```

**Application info structure (example - to be defined):**
- Declared in: `Core/Inc/bl_app_contract.h` (bootloader declares the type)
- Defined in: `application/src/app_info.c` (application provides instance)
- Contains: Firmware name, version, build timestamp, features, CRC, magic number
- Bootloader reads this during boot to validate application

**Key principle:**
- Bootloader declares the contract (struct definitions, addresses)
- Application implements/provides the data
- Both include the same contract header: `Core/Inc/bl_app_contract.h`

### Critical Addresses

```
0x08000000: Bootloader start
0x08007F00: Bootloader API table (256 bytes reserved)
0x08008000: Application start (vector table)
0x08008200: Application info header (app_info_t)
```

### Encryption/Security Notes

- Encryption keys: ONLY in bootloader, NEVER exposed to application
- Application can request decryption via BL API, but never accesses keys directly
- Shared "magic number" for application authentication (not encryption key)
- Bootloader validates application's magic number during boot

## Project Structure

**Current structure (STM32CubeMX autogenerated):**
```
PeriphNet/
├── Core/                       # Common code (HAL, drivers, utilities)
│   ├── Inc/                    # Shared headers
│   ├── Src/                    # Shared source files
│   └── Startup/                # Startup assembly
├── Drivers/                    # STM32 HAL and CMSIS
│   ├── STM32F4xx_HAL_Driver/
│   └── CMSIS/
├── PeriphNet.ioc               # STM32CubeMX project file
├── STM32F407VETX_FLASH.ld      # Current linker script
└── STM32F407VETX_RAM.ld
```

**Target structure (to be implemented with CMake):**
```
PeriphNet/
├── Core/                       # Common code shared between BL and APP
│   ├── Inc/
│   │   ├── bl_app_contract.h   # BL-APP interface contract
│   │   └── ...                 # Other shared headers
│   ├── Src/
│   │   ├── drivers/            # Shared drivers (SPI, flash, CRC, etc.)
│   │   └── ...
│   └── Startup/
├── Drivers/                    # STM32 HAL (shared by BL and APP)
│   ├── STM32F4xx_HAL_Driver/
│   └── CMSIS/
├── bootloader/                 # Bootloader-specific code
│   ├── src/
│   │   ├── boot_main.c
│   │   ├── boot_api.c          # Implements BL API at 0x08007F00
│   │   └── crypto.c            # Encryption/decryption (BL only)
│   ├── inc/
│   │   └── boot_config.h
│   ├── linker/
│   │   └── bootloader.ld       # 32KB at 0x08000000
│   └── tests/
├── application/                # Application-specific code
│   ├── src/
│   │   ├── app_main.c
│   │   ├── app_info.c          # Defines app_info_t instance
│   │   ├── hal/                # App-specific HAL
│   │   ├── middleware/         # FreeRTOS, lwIP, MQTT, Modbus
│   │   ├── app/                # Application logic
│   │   │   ├── modbus_mqtt_bridge/
│   │   │   ├── firmware_update/
│   │   │   └── ...
│   │   └── utils/
│   ├── inc/
│   ├── linker/
│   │   └── application.ld      # 480KB at 0x08008000
│   └── config/
├── tests/                      # Unit and integration tests
│   ├── unit/                   # CppUTest
│   ├── integration/
│   └── mocks/
├── third_party/                # External dependencies
│   ├── FreeRTOS/
│   ├── lwIP/
│   ├── trice/
│   ├── CppUTest/
│   └── MQTT_client/
└── tools/                      # Build scripts, utilities
    └── finalize_binary.py      # Post-build: calculate CRCs
```

## Hardware Configuration

**Target:** STM32F407VET6 industrial board
- MCU: STM32F407VET6 (512KB Flash, 192KB RAM, 168MHz)
- External Flash: W25Q128 (16MB, SPI)
- EEPROM: AT24C02BN (256 bytes, I2C)
- Ethernet PHY: DP83848IVV
- Interfaces: Ethernet, RS485, RS232, 2x CAN, SD card, RTC

**Peripheral mapping (STM32CubeIDE .ioc file):**
- Check `PeriphNet.ioc` for current pin assignments and peripheral configuration

**Current demo firmware on board:**
- IP Address: 192.168.0.135 (DHCP assigned)
- MAC Address: 10:06:1c:a7:9e:d7
- Running: HTTP web server (port 80)
- Status: First-time setup page (password configuration)
- This is factory demo firmware - will be replaced with custom firmware

## Key Architectural Constraints

1. **Bootloader must be network-free**: No lwIP, no MQTT, no FreeRTOS - keeps it simple and reliable
2. **Application uses bootloader API for verification**: Never duplicate crypto/verification code
3. **External flash layout is fixed**: Changing offsets requires bootloader update
4. **Vector table relocation**: Application must relocate VTOR to 0x08008000
5. **API table is fixed at 0x08007F00**: This is a hard contract between bootloader and application
6. **Golden image fallback**: Always maintain a known-good firmware in external flash

## Coding Standards

### Naming Conventions

**Data type naming follows Hungarian notation prefix:**

- **Structures**: `s` + PascalCase
  ```c
  typedef struct {
      uint32_t magic;
      uint32_t version;
  } sAppInfo;
  ```

- **Unions**: `u` + PascalCase
  ```c
  typedef union {
      uint32_t word;
      uint8_t bytes[4];
  } uDataConverter;
  ```

- **Function pointers**: `f` + PascalCase
  ```c
  typedef int (*fVerifyImage)(uint32_t addr, uint32_t size, uint32_t crc);
  ```

**Examples:**
- ❌ `app_info_t`, `bootloader_api_t`, `update_status_t`
- ✅ `sAppInfo`, `sBootloaderApi`, `sUpdateStatus`

**Note:** Code examples in this documentation may use different naming styles for illustration purposes. Actual implementation must follow the naming convention above.

## Development Approach

- **Test-Driven Development (TDD)** with CppUTest
- **Modular architecture**: Each HAL driver, middleware component independently testable
- **Trice tracing**: Use Trice over TCP/IP for runtime diagnostics (not RTT/UART)
- All third-party code goes in `third_party/` directory

### Important Notes

**Code examples in this documentation are guidance only:**
- Struct definitions, function signatures, and addresses shown are reference examples
- Actual implementation details are to be defined during development
- The architectural principles and memory layout are the key constraints

**Shared code compilation:**
- Drivers in `Core/Src/drivers/` must be position-independent
- No global state that assumes specific memory layout
- Use separate compile flags for bootloader vs application builds
- Monitor bootloader size - must stay under 32KB

**Build system constraints:**
- Bootloader must be buildable independently (separate target)
- Application must be buildable independently (separate target)
- Post-build scripts needed to calculate CRCs and finalize binaries
- Linker scripts enforce strict memory regions

## Firmware Update HTTP API

The device implements an HTTP-based REST API for firmware updates. No special software needed - works with standard tools (curl, wget, browser).

### API Endpoints

```
POST   /api/firmware/upload       Upload firmware binary (auto-verify)
GET    /api/firmware/status       Get current firmware update status
POST   /api/firmware/install      Install staged firmware immediately
POST   /api/firmware/schedule     Schedule firmware install for later
DELETE /api/firmware/staged       Cancel staged firmware update
```

### POST /api/firmware/upload

**Upload firmware binary to device.**

**Request:**
- Method: `POST`
- Content-Type: `application/octet-stream`
- Body: Raw firmware binary (.bin file)
- Optional header: `Content-Length` (firmware size)

**Process:**
1. Streams firmware to external flash (0x00001000)
2. Extracts metadata from firmware (reads app_info header at offset 0x200)
3. Verifies integrity (CRC, magic numbers)
4. Calls bootloader API for verification
5. Marks as staged (not active) if valid

**Response:**
Same structure as `GET /api/firmware/status` (see below)

**Example:**
```bash
# Upload firmware using curl
curl -X POST \
  --data-binary @firmware.bin \
  http://192.168.0.135/api/firmware/upload

# Using wget
wget --post-file=firmware.bin \
  http://192.168.0.135/api/firmware/upload -O -
```

### GET /api/firmware/status

**Get current firmware update status.**

**Response JSON:**
```json
{
  "status": "verifiedValid",
  "size": 245760,
  "version": "1.2.3",
  "scheduledInstall": null
}
```

**Status field values:**
- `noFile` - No staged firmware available
- `uploading` - Firmware upload in progress
- `verifying` - Uploaded firmware being verified
- `verifiedValid` - Firmware verified successfully, ready to install
- `verifiedInvalid` - Firmware verification failed
- `deploying` - Firmware installation in progress (device will reboot)

**Fields:**
- `status` (string): Current update status
- `size` (number): Firmware size in bytes (0 if noFile)
- `version` (string): Firmware version extracted from binary (null if noFile)
- `scheduledInstall` (string|null): ISO 8601 timestamp if scheduled, null otherwise

**Example:**
```bash
curl http://192.168.0.135/api/firmware/status
```

### POST /api/firmware/install

**Install staged firmware immediately (triggers reboot).**

**Request:**
- Method: `POST`
- No body required

**Process:**
1. Validates staged firmware exists and is verified
2. Sets update_requested flag in external flash
3. Responds with success
4. Reboots device after 3 seconds

**Response:**
```json
{
  "status": "deploying",
  "message": "Device will reboot in 3 seconds",
  "rebootIn": 3
}
```

**Example:**
```bash
curl -X POST http://192.168.0.135/api/firmware/install
```

### POST /api/firmware/schedule

**Schedule firmware installation for later.**

**Request:**
- Method: `POST`
- Content-Type: `application/json`

**Body (Option 1 - specific time):**
```json
{
  "installAt": "2024-01-15T02:00:00Z"
}
```

**Body (Option 2 - delay in seconds):**
```json
{
  "delaySeconds": 3600
}
```

**Response:**
```json
{
  "status": "verifiedValid",
  "size": 245760,
  "version": "1.2.3",
  "scheduledInstall": "2024-01-15T02:00:00Z"
}
```

**Example:**
```bash
# Schedule for specific time
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"installAt":"2024-01-15T02:00:00Z"}' \
  http://192.168.0.135/api/firmware/schedule

# Schedule for 1 hour from now
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"delaySeconds":3600}' \
  http://192.168.0.135/api/firmware/schedule
```

### DELETE /api/firmware/staged

**Cancel staged firmware update.**

**Request:**
- Method: `DELETE`
- No body required

**Process:**
1. Erases staged firmware from external flash
2. Clears update status block
3. Cancels any scheduled install

**Response:**
```json
{
  "status": "noFile",
  "size": 0,
  "version": null,
  "scheduledInstall": null
}
```

**Example:**
```bash
curl -X DELETE http://192.168.0.135/api/firmware/staged
```

### Complete Update Workflow

**Typical usage:**

```bash
# 1. Upload firmware (device auto-extracts version, verifies)
curl -X POST --data-binary @periphernet_v1.2.3.bin \
  http://192.168.0.135/api/firmware/upload

# Response:
# {
#   "status": "verifiedValid",
#   "size": 245760,
#   "version": "1.2.3",
#   "scheduledInstall": null
# }

# 2. Check status (optional)
curl http://192.168.0.135/api/firmware/status

# 3a. Install immediately
curl -X POST http://192.168.0.135/api/firmware/install

# OR 3b. Schedule for later (e.g., 2 AM)
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"installAt":"2024-01-15T02:00:00Z"}' \
  http://192.168.0.135/api/firmware/schedule
```

**Simple web interface (optional):**
Access `http://192.168.0.135/update` in browser for drag-and-drop upload with GUI.

### Firmware Metadata Extraction

Device extracts metadata from uploaded firmware binary:

1. Reads application info header at offset 0x200 (0x08008200 - 0x08008000)
2. Validates magic number (APP_INFO_MAGIC)
3. Extracts version, size, CRC from sAppInfo structure
4. Verifies integrity using embedded CRC
5. Calls bootloader API for additional verification

This allows upload endpoint to accept just the raw binary - all metadata comes from the file itself.

## Update Flow

### Device-Side Update Flow

1. **Application receives firmware** via HTTP POST `/api/firmware/upload`
2. **Streams to external flash** at 0x00001000
3. **Extracts metadata** from app_info header (offset 0x200 in binary)
4. **Verifies integrity** using CRC from app_info
5. **Calls bootloader API** to verify image
6. **Sets staged flag** in external flash status block (update_requested = 0)
7. **Returns status** with version and verification result

When install is triggered (immediate or scheduled):

8. **Sets update flag** in external flash status block (update_requested = 1)
9. **Reboots**
10. **Bootloader reads update flag**
11. **Verifies image** in external flash
12. **Erases application sectors** (2-7)
13. **Programs internal flash** from external flash
14. **Clears update flag**
15. **Jumps to new application** at 0x08008000

## Update Status Block (External Flash 0x00000000)

```c
#define UPDATE_STATUS_MAGIC 0x46575550  // "FWUP"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t update_requested;   // 1 = pending, 0 = none
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t image_offset;       // Usually 0x1000
    uint8_t  image_sha256[32];
    uint32_t app_version;
    uint32_t reserved[16];
    uint32_t header_crc32;
} update_status_t;
```

## Key Technologies

- **RTOS:** FreeRTOS
- **TCP/IP Stack:** lwIP
- **MQTT:** TBD (to be selected)
- **Modbus:** Custom or adapted implementation
- **Tracing:** Trice (TCP/IP transport)
- **Toolchain:** ARM GCC
- **Testing:** CppUTest (manual execution)
- **Build:** CMake (planned migration from STM32CubeIDE)

## Linker Script Notes

When modifying linker scripts:

**Bootloader (`bootloader/linker/bootloader.ld`):**
- FLASH origin: 0x08000000, length: 32K
- Reserve 256 bytes at 0x08007F00 for API table (use KEEP directive)
- Must fit all bootloader code in 32KB - monitor size carefully
- Example section placement:
  ```ld
  .bl_api 0x08007F00 : {
      KEEP(*(.bl_api))
  } > BOOTLOADER
  ```

**Application (`application/linker/application.ld`):**
- FLASH origin: 0x08008000, length: 480K
- Section order is critical:
  1. `.isr_vector` at 0x08008000 (MUST be first, ~392 bytes)
  2. `.app_header` at 0x08008200 (app_info_t structure)
  3. `.text` and other sections follow
- Example section placement:
  ```ld
  .isr_vector 0x08008000 : {
      KEEP(*(.isr_vector))
  } > FLASH

  .app_header 0x08008200 : {
      KEEP(*(.app_header))
  } > FLASH
  ```

**Vector table size (STM32F407VET6):**
- Initial SP + Reset + 15 system exceptions + 82 external IRQs = 99 entries
- 99 × 4 bytes = 396 bytes (0x18C)
- Safe padding: 512 bytes (0x200) → app_info at 0x08008200

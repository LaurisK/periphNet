# PeriphNet First Milestone Plan

## Goal: "Hello World v2 via OTA Update"

**Objective:** Upload new firmware over Ethernet, bootloader installs it, device boots and displays "Hello World v2.0.0" webpage with system information.

**This proves:**
- ✅ Bootloader validates and jumps to application
- ✅ External flash driver works (W25Q128)
- ✅ Bootloader-Application contract works (UID validation)
- ✅ Ethernet + HTTP stack works
- ✅ Firmware upload via HTTP works
- ✅ Verification works
- ✅ Installation from external flash → internal flash works
- ✅ New application boots successfully

---

## Milestone 0: Development Environment Setup

**Goal:** Can build and flash firmware to STM32F407VET6

**Tasks:**
- [ ] CMake build system configured for STM32F407
- [ ] ARM GCC toolchain working
- [ ] Can flash via ST-Link (or other debugger)
- [ ] Basic LED blink demo compiles and runs

**Test:**
```bash
cd PeriphNet
mkdir build && cd build
cmake ..
make
st-flash write blink.bin 0x08000000
```

**Deliverable:** LED blinking at 1Hz

**Success Criteria:**
- ✅ Code compiles without errors
- ✅ Can flash to device
- ✅ LED blinks visibly

---

## Milestone 1: Ethernet + Basic HTTP Server

**Goal:** Serve static "Hello World" webpage over Ethernet

**Why first?** Webpage becomes our primary debug/status interface for all subsequent milestones.

**Tasks:**

### 1.1 Configure lwIP Stack
- [ ] Use STM32CubeMX to configure Ethernet peripheral (DP83848 PHY)
- [ ] Enable lwIP middleware in .ioc file
- [ ] Configure DHCP client (like demo firmware)
- [ ] Generate code

### 1.2 Implement Minimal HTTP Server
- [ ] Use lwIP httpd or implement simple HTTP server
- [ ] Serve static page at `GET /`
- [ ] Display system information

### 1.3 Create Hello World Page Template
```html
<!DOCTYPE html>
<html>
<head><title>PeriphNet</title></head>
<body>
    <h1>Hello World v1.0.0</h1>
    <table>
        <tr><td>Device:</td><td>STM32F407VET6</td></tr>
        <tr><td>IP Address:</td><td>192.168.0.XXX</td></tr>
        <tr><td>MAC Address:</td><td>XX:XX:XX:XX:XX:XX</td></tr>
        <tr><td>Uptime:</td><td>XXX seconds</td></tr>
    </table>
</body>
</html>
```

**Test:**
```bash
# Device should acquire IP via DHCP
# Check router DHCP leases or use network scanner
nmap -sn 192.168.0.0/24

# Access webpage
curl http://192.168.0.XXX/
# Or open in browser
```

**Deliverable:** Webpage accessible via browser showing "Hello World v1.0.0"

**Success Criteria:**
- ✅ Device acquires IP via DHCP
- ✅ Responds to HTTP GET on port 80
- ✅ Webpage displays correctly in browser
- ✅ Shows correct MAC address and IP

**Estimated Effort:** 3-4 days

---

## Milestone 2: Bootloader ↔ Application Jump

**Goal:** Bootloader validates and jumps to application; application reports boot status via HTTP

**Tasks:**

### 2.1 Create Project Structure
```
PeriphNet/
├── Core/                    # Shared code
├── Drivers/                 # STM32 HAL
├── bootloader/
│   ├── src/
│   │   └── boot_main.c
│   ├── inc/
│   ├── linker/
│   │   └── bootloader.ld    # 32KB @ 0x08000000
│   └── CMakeLists.txt
└── application/
    ├── src/
    │   └── app_main.c       # From M1
    ├── linker/
    │   └── application.ld   # 480KB @ 0x08008000
    └── CMakeLists.txt
```

### 2.2 Implement Minimal Bootloader
```c
// bootloader/src/boot_main.c

void bootloader_main(void) {
    // Initialize minimal HAL
    HAL_Init();
    SystemClock_Config();

    // Check if valid application exists at 0x08008000
    uint32_t app_stack = *(uint32_t*)APP_START_ADDRESS;
    uint32_t app_reset = *(uint32_t*)(APP_START_ADDRESS + 4);

    // Basic validation
    if ((app_stack & 0x2FF00000) == 0x20000000 &&  // Stack in RAM
        (app_reset & 0xFF000000) == 0x08000000) {  // Reset in flash

        jump_to_application(APP_START_ADDRESS);
    }

    // If invalid, hang with LED error pattern
    while(1) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        HAL_Delay(100);  // Fast blink = error
    }
}

void jump_to_application(uint32_t app_address) {
    uint32_t app_stack = *(uint32_t*)app_address;
    uint32_t app_reset = *(uint32_t*)(app_address + 4);

    // Deinit peripherals
    HAL_DeInit();

    // Disable interrupts
    __disable_irq();

    // Set stack pointer
    __set_MSP(app_stack);

    // Relocate vector table
    SCB->VTOR = app_address;

    // Jump to application
    void (*app_entry)(void) = (void(*)(void))app_reset;
    app_entry();
}
```

### 2.3 Update Application with Boot Information
```c
// application/src/app_main.c

// Global variable to track boot source
typedef enum {
    BOOT_SOURCE_UNKNOWN,
    BOOT_SOURCE_BOOTLOADER,
    BOOT_SOURCE_DIRECT
} eBootSource;

eBootSource g_bootSource;

void detect_boot_source(void) {
    // Check if we're running from application address
    uint32_t pc = (uint32_t)__get_PC();

    if (pc >= 0x08008000) {
        // Check if bootloader exists and is valid
        uint32_t bl_stack = *(uint32_t*)0x08000000;
        if ((bl_stack & 0x2FF00000) == 0x20000000) {
            g_bootSource = BOOT_SOURCE_BOOTLOADER;
        } else {
            g_bootSource = BOOT_SOURCE_DIRECT;
        }
    } else {
        g_bootSource = BOOT_SOURCE_UNKNOWN;
    }
}

int main(void) {
    // Relocate vector table
    SCB->VTOR = APP_START_ADDRESS;

    // Initialize HAL
    HAL_Init();
    SystemClock_Config();

    // Detect boot source
    detect_boot_source();

    // Start network and HTTP server (from M1)
    // ...
}
```

### 2.4 Update Webpage to Show Boot Information
```html
<h1>Hello World v1.0.0</h1>
<table>
    <tr><td>Device:</td><td>STM32F407VET6</td></tr>
    <tr><td>Boot Source:</td><td>Bootloader</td></tr>
    <tr><td>Application Base:</td><td>0x08008000</td></tr>
    <tr><td>Bootloader Present:</td><td>YES</td></tr>
    <tr><td>IP Address:</td><td>192.168.0.XXX</td></tr>
</table>
```

**Build and Flash:**
```bash
# Build both binaries
cd build
make bootloader
make application

# Flash bootloader
st-flash write bootloader/bootloader.bin 0x08000000

# Flash application
st-flash write application/application.bin 0x08008000

# Or combine into single image
srec_cat bootloader.bin -binary -offset 0x08000000 \
         application.bin -binary -offset 0x08008000 \
         -o combined.hex -intel
st-flash --format ihex write combined.hex
```

**Test:**
```bash
# Access webpage
curl http://192.168.0.XXX/

# Should show:
# Boot Source: Bootloader
# Bootloader Present: YES
```

**Deliverable:** Device boots through bootloader, webpage confirms bootloader launch

**Success Criteria:**
- ✅ Bootloader validates application and jumps
- ✅ Application relocates vector table correctly
- ✅ Webpage shows "Boot Source: Bootloader"
- ✅ Can debug both bootloader and application

**Estimated Effort:** 2-3 days

---

## Milestone 3: External Flash + BL-APP UID Contract

**Goal:** External flash driver working; bootloader writes MCU UID to flash; application validates it and displays flash info on webpage

**Tasks:**

### 3.1 Define BL-APP Contract
```c
// Core/Inc/bl_app_contract.h

#define BL_APP_CONTRACT_H

// Memory addresses
#define APP_START_ADDRESS       0x08008000
#define APP_INFO_ADDRESS        0x08008200
#define BL_API_ADDRESS          0x08007F00

#define EXT_FLASH_STATUS_ADDR   0x00000000
#define EXT_FLASH_UID_ADDR      0x00000100  // MCU UID stored here
#define EXT_FLASH_FWU_IMG_ADDR  0x00001000

// Magic numbers
#define UID_BLOCK_MAGIC         0x55494442  // "UIDB"

// MCU UID block in external flash
typedef struct {
    uint32_t magic;              // Must be UID_BLOCK_MAGIC
    uint32_t uid[3];             // STM32 96-bit unique ID
    uint32_t crc32;              // CRC32 of magic + uid
} sUidBlock;
```

### 3.2 Implement W25Q128 Driver
```c
// Core/Src/drivers/w25q128.c

#define W25Q128_JEDEC_ID        0xEF4018  // Winbond W25Q128

typedef struct {
    uint8_t manufacturer;   // 0xEF = Winbond
    uint8_t memoryType;     // 0x40 = Q series
    uint8_t capacity;       // 0x18 = 128Mbit
} sJedecId;

void w25q128_init(void);
uint32_t w25q128_read_jedec_id(void);
void w25q128_read(uint32_t address, uint8_t *buffer, uint32_t length);
void w25q128_write(uint32_t address, const uint8_t *buffer, uint32_t length);
void w25q128_erase_sector(uint32_t address);
```

**Driver implementation:**
- Use SPI peripheral configured in .ioc file
- Implement commands: READ (0x03), WRITE (0x02), ERASE (0x20), RDID (0x9F)
- Add write enable/disable, status polling

### 3.3 Bootloader: Write UID to External Flash
```c
// bootloader/src/boot_main.c

void read_mcu_uid(uint32_t uid[3]) {
    uid[0] = *(uint32_t*)0x1FFF7A10;
    uid[1] = *(uint32_t*)0x1FFF7A14;
    uid[2] = *(uint32_t*)0x1FFF7A18;
}

void validate_or_write_uid(void) {
    sUidBlock uid_block;

    // Read UID block from external flash
    w25q128_read(EXT_FLASH_UID_ADDR, (uint8_t*)&uid_block, sizeof(sUidBlock));

    // Check if valid
    uint32_t calc_crc = crc32_calculate((uint8_t*)&uid_block,
                                         sizeof(sUidBlock) - 4);

    if (uid_block.magic != UID_BLOCK_MAGIC ||
        uid_block.crc32 != calc_crc) {

        // Invalid or missing - write MCU UID
        sUidBlock new_uid_block;
        new_uid_block.magic = UID_BLOCK_MAGIC;
        read_mcu_uid(new_uid_block.uid);
        new_uid_block.crc32 = crc32_calculate((uint8_t*)&new_uid_block,
                                               sizeof(sUidBlock) - 4);

        // Erase sector and write
        w25q128_erase_sector(EXT_FLASH_UID_ADDR);
        w25q128_write(EXT_FLASH_UID_ADDR, (uint8_t*)&new_uid_block,
                      sizeof(sUidBlock));
    }
}

void bootloader_main(void) {
    HAL_Init();
    SystemClock_Config();

    // Initialize SPI for external flash
    MX_SPI_Init();
    w25q128_init();

    // Validate or write UID
    validate_or_write_uid();

    // Continue with application validation and jump
    // ...
}
```

### 3.4 Application: Read and Validate UID
```c
// application/src/flash_info.c

typedef struct {
    bool uidValid;
    uint32_t mcuUid[3];
    uint32_t flashUid[3];
    sJedecId jedecId;
} sFlashInfo;

sFlashInfo g_flashInfo;

void read_flash_info(void) {
    // Read MCU UID
    g_flashInfo.mcuUid[0] = *(uint32_t*)0x1FFF7A10;
    g_flashInfo.mcuUid[1] = *(uint32_t*)0x1FFF7A14;
    g_flashInfo.mcuUid[2] = *(uint32_t*)0x1FFF7A18;

    // Read JEDEC ID
    uint32_t jedec = w25q128_read_jedec_id();
    g_flashInfo.jedecId.manufacturer = (jedec >> 16) & 0xFF;
    g_flashInfo.jedecId.memoryType = (jedec >> 8) & 0xFF;
    g_flashInfo.jedecId.capacity = jedec & 0xFF;

    // Read UID block from external flash
    sUidBlock uid_block;
    w25q128_read(EXT_FLASH_UID_ADDR, (uint8_t*)&uid_block, sizeof(sUidBlock));

    // Validate
    uint32_t calc_crc = crc32_calculate((uint8_t*)&uid_block,
                                         sizeof(sUidBlock) - 4);

    if (uid_block.magic == UID_BLOCK_MAGIC &&
        uid_block.crc32 == calc_crc) {

        // Copy flash UID
        memcpy(g_flashInfo.flashUid, uid_block.uid, 12);

        // Validate matches MCU UID
        g_flashInfo.uidValid = (memcmp(g_flashInfo.mcuUid,
                                        g_flashInfo.flashUid, 12) == 0);
    } else {
        g_flashInfo.uidValid = false;
    }
}
```

### 3.5 Update Webpage to Display Flash Info
```html
<h1>Hello World v1.0.0</h1>

<h2>System Information</h2>
<table>
    <tr><td>Device:</td><td>STM32F407VET6</td></tr>
    <tr><td>Boot Source:</td><td>Bootloader</td></tr>
    <tr><td>IP Address:</td><td>192.168.0.XXX</td></tr>
    <tr><td>Uptime:</td><td>XXX seconds</td></tr>
</table>

<h2>MCU Unique ID</h2>
<table>
    <tr><td>UID[0]:</td><td>0x12345678</td></tr>
    <tr><td>UID[1]:</td><td>0x9ABCDEF0</td></tr>
    <tr><td>UID[2]:</td><td>0x11223344</td></tr>
</table>

<h2>External Flash (W25Q128)</h2>
<table>
    <tr><td>JEDEC ID:</td><td>0xEF4018</td></tr>
    <tr><td>Manufacturer:</td><td>Winbond (0xEF)</td></tr>
    <tr><td>Memory Type:</td><td>Q-series (0x40)</td></tr>
    <tr><td>Capacity:</td><td>128Mbit (0x18)</td></tr>
    <tr><td>UID Stored:</td><td>YES</td></tr>
    <tr><td>UID Valid:</td><td>MATCH</td></tr>
</table>
```

**Test:**
```bash
# First boot (no UID in flash)
# Bootloader writes MCU UID to external flash

# Access webpage
curl http://192.168.0.XXX/

# Should show:
# - MCU UID: 0x... (3 words)
# - JEDEC ID: 0xEF4018
# - UID Stored: YES
# - UID Valid: MATCH

# Erase external flash manually (optional test)
# Reboot - bootloader should re-write UID
```

**Deliverable:** Webpage shows MCU UID, external flash JEDEC ID, and UID validation status

**Success Criteria:**
- ✅ W25Q128 driver reads JEDEC ID correctly (0xEF4018)
- ✅ Bootloader writes MCU UID to external flash if missing
- ✅ Application reads and validates UID matches
- ✅ Webpage displays all flash information
- ✅ UID validation passes (shows "MATCH")

**Estimated Effort:** 2-3 days

---

## Milestone 4: HTTP Upload to External Flash

**Goal:** Upload binary via POST, store to external flash, return status

**Tasks:**

### 4.1 Implement Upload State Machine
```c
// application/src/app/firmware_update/fwu_state.c

typedef enum {
    FWU_STATE_NO_FILE,
    FWU_STATE_UPLOADING,
    FWU_STATE_VERIFYING,
    FWU_STATE_VERIFIED_VALID,
    FWU_STATE_VERIFIED_INVALID,
    FWU_STATE_DEPLOYING
} eFwuState;

typedef struct {
    eFwuState state;
    uint32_t size;
    char version[16];
    char scheduledInstall[32];  // ISO 8601 timestamp or empty
} sFwuStatus;

sFwuStatus g_fwuStatus = {
    .state = FWU_STATE_NO_FILE,
    .size = 0,
    .version = "",
    .scheduledInstall = ""
};
```

### 4.2 Implement POST /api/firmware/upload
```c
// application/src/app/firmware_update/http_handler.c

void handle_firmware_upload(struct httpd_request *req) {
    uint32_t total_size = 0;
    uint32_t flash_addr = EXT_FLASH_FWU_IMG_ADDR;

    // Set state to uploading
    g_fwuStatus.state = FWU_STATE_UPLOADING;
    g_fwuStatus.size = 0;

    // Erase sectors for new firmware (480KB)
    erase_firmware_area();

    // Stream data to external flash
    while (data_available(req)) {
        uint8_t buffer[256];
        int bytes = read_chunk(req, buffer, sizeof(buffer));

        w25q128_write(flash_addr, buffer, bytes);
        flash_addr += bytes;
        total_size += bytes;
    }

    g_fwuStatus.size = total_size;

    // Will verify in next milestone
    g_fwuStatus.state = FWU_STATE_VERIFYING;

    // Return status (same as GET /api/firmware/status)
    send_fwu_status_json(req, &g_fwuStatus);
}
```

### 4.3 Implement GET /api/firmware/status
```c
void handle_firmware_status(struct httpd_request *req) {
    send_fwu_status_json(req, &g_fwuStatus);
}

void send_fwu_status_json(struct httpd_request *req, sFwuStatus *status) {
    char json[256];
    const char *state_str = fwu_state_to_string(status->state);

    snprintf(json, sizeof(json),
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"size\": %lu,\n"
        "  \"version\": \"%s\",\n"
        "  \"scheduledInstall\": %s\n"
        "}",
        state_str,
        status->size,
        status->version,
        status->scheduledInstall[0] ? status->scheduledInstall : "null"
    );

    httpd_send_response(req, 200, "application/json", json);
}
```

### 4.4 Add FWU Status to Webpage
```html
<h2>Firmware Update Status</h2>
<table>
    <tr><td>Status:</td><td id="fwu-status">noFile</td></tr>
    <tr><td>Staged Size:</td><td id="fwu-size">0</td></tr>
    <tr><td>Staged Version:</td><td id="fwu-version">-</td></tr>
</table>

<script>
// Auto-refresh status every 2 seconds
setInterval(function() {
    fetch('/api/firmware/status')
        .then(r => r.json())
        .then(data => {
            document.getElementById('fwu-status').textContent = data.status;
            document.getElementById('fwu-size').textContent = data.size;
            document.getElementById('fwu-version').textContent = data.version || '-';
        });
}, 2000);
</script>
```

**Test:**
```bash
# Create test binary (just use current application.bin)
cp application.bin test_firmware.bin

# Upload
curl -X POST --data-binary @test_firmware.bin \
  http://192.168.0.135/api/firmware/upload

# Response (verification not yet implemented):
# {
#   "status": "verifying",
#   "size": 245760,
#   "version": "",
#   "scheduledInstall": null
# }

# Check status
curl http://192.168.0.135/api/firmware/status

# Read back from external flash and compare (manual verification)
```

**Deliverable:** Can upload binary via HTTP, stored in external flash

**Success Criteria:**
- ✅ POST /api/firmware/upload accepts binary data
- ✅ Data written to external flash at 0x00001000
- ✅ Returns JSON status response
- ✅ GET /api/firmware/status returns current state
- ✅ Webpage shows upload status

**Estimated Effort:** 2 days

---

## Milestone 5: Firmware Verification

**Goal:** Extract metadata from uploaded firmware and verify integrity

**Tasks:**

### 5.1 Define sAppInfo Structure
```c
// Core/Inc/bl_app_contract.h

#define APP_INFO_MAGIC      0x41505049  // "APPI"

typedef struct {
    uint32_t magic;
    uint32_t structVersion;

    char fwName[32];            // "PeriphNet"
    char fwVersion[16];         // "1.0.0"
    uint32_t buildTimestamp;

    uint32_t appSize;           // Application size in bytes
    uint32_t appCrc32;          // CRC32 of entire application

    uint32_t appMagicNumber;    // Shared secret
    uint32_t headerCrc32;       // CRC32 of this struct
} sAppInfo;
```

### 5.2 Add sAppInfo to Application
```c
// application/src/app_info.c

const sAppInfo __attribute__((section(".app_header"))) __attribute__((used)) app_info = {
    .magic = APP_INFO_MAGIC,
    .structVersion = 1,

    .fwName = "PeriphNet",
    .fwVersion = "1.0.0",
    .buildTimestamp = 0,  // Filled by build script

    .appSize = 0,         // Filled by build script
    .appCrc32 = 0,        // Filled by build script

    .appMagicNumber = 0xCAFEBABE,
    .headerCrc32 = 0      // Filled by build script
};
```

### 5.3 Update Application Linker Script
```ld
/* application/linker/application.ld */

SECTIONS
{
    .isr_vector 0x08008000 : {
        KEEP(*(.isr_vector))
    } > FLASH

    .app_header 0x08008200 : {
        KEEP(*(.app_header))
        . = ALIGN(4);
    } > FLASH

    .text : {
        *(.text)
        *(.text*)
        /* ... */
    } > FLASH
}
```

### 5.4 Post-Build Script
```python
#!/usr/bin/env python3
# tools/finalize_app_binary.py

import struct
import zlib
import sys

def calculate_crc32(data):
    return zlib.crc32(data) & 0xffffffff

def finalize_binary(bin_path):
    APP_INFO_OFFSET = 0x200  # 0x08008200 - 0x08008000

    with open(bin_path, 'r+b') as f:
        binary = bytearray(f.read())

        # Calculate app size
        app_size = len(binary)

        # Calculate app CRC (entire binary with CRC field = 0)
        struct.pack_into('<I', binary, APP_INFO_OFFSET + 56, 0)  # Clear appCrc32
        app_crc = calculate_crc32(binary)

        # Write app size and CRC
        struct.pack_into('<I', binary, APP_INFO_OFFSET + 52, app_size)
        struct.pack_into('<I', binary, APP_INFO_OFFSET + 56, app_crc)

        # Calculate header CRC (sAppInfo minus last 4 bytes)
        header_data = binary[APP_INFO_OFFSET:APP_INFO_OFFSET + 60]
        header_crc = calculate_crc32(header_data)
        struct.pack_into('<I', binary, APP_INFO_OFFSET + 64, header_crc)

        # Write back
        f.seek(0)
        f.write(binary)

    print(f"Finalized: size={app_size}, crc=0x{app_crc:08X}")

if __name__ == '__main__':
    finalize_binary(sys.argv[1])
```

**CMake integration:**
```cmake
# application/CMakeLists.txt
add_custom_command(TARGET application POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/finalize_app_binary.py
            $<TARGET_FILE_DIR:application>/application.bin
    COMMENT "Finalizing application binary with CRC"
)
```

### 5.5 Implement Verification in Upload Handler
```c
void handle_firmware_upload(struct httpd_request *req) {
    // ... upload code from M4 ...

    g_fwuStatus.state = FWU_STATE_VERIFYING;

    // Read sAppInfo from uploaded firmware (offset 0x200)
    sAppInfo app_info;
    w25q128_read(EXT_FLASH_FWU_IMG_ADDR + 0x200,
                 (uint8_t*)&app_info,
                 sizeof(sAppInfo));

    // Validate magic
    if (app_info.magic != APP_INFO_MAGIC) {
        g_fwuStatus.state = FWU_STATE_VERIFIED_INVALID;
        send_fwu_status_json(req, &g_fwuStatus);
        return;
    }

    // Verify header CRC
    uint32_t calc_header_crc = crc32_calculate((uint8_t*)&app_info,
                                                sizeof(sAppInfo) - 4);
    if (calc_header_crc != app_info.headerCrc32) {
        g_fwuStatus.state = FWU_STATE_VERIFIED_INVALID;
        send_fwu_status_json(req, &g_fwuStatus);
        return;
    }

    // Verify app CRC
    // Read firmware in chunks and calculate CRC
    uint32_t calc_app_crc = 0;
    // ... streaming CRC calculation ...

    if (calc_app_crc != app_info.appCrc32) {
        g_fwuStatus.state = FWU_STATE_VERIFIED_INVALID;
        send_fwu_status_json(req, &g_fwuStatus);
        return;
    }

    // All checks passed
    g_fwuStatus.state = FWU_STATE_VERIFIED_VALID;
    strncpy(g_fwuStatus.version, app_info.fwVersion, sizeof(g_fwuStatus.version));

    send_fwu_status_json(req, &g_fwuStatus);
}
```

**Test:**
```bash
# Build application with finalized CRC
make application

# Upload
curl -X POST --data-binary @application.bin \
  http://192.168.0.135/api/firmware/upload

# Response:
# {
#   "status": "verifiedValid",
#   "size": 245760,
#   "version": "1.0.0",
#   "scheduledInstall": null
# }

# Try uploading corrupted file
dd if=/dev/urandom of=bad.bin bs=1024 count=100
curl -X POST --data-binary @bad.bin \
  http://192.168.0.135/api/firmware/upload

# Response:
# {
#   "status": "verifiedInvalid",
#   "size": 102400,
#   "version": "",
#   "scheduledInstall": null
# }
```

**Deliverable:** Uploaded firmware verified, version extracted

**Success Criteria:**
- ✅ sAppInfo structure embedded in application binary
- ✅ Post-build script calculates and injects CRCs
- ✅ Upload handler extracts version from firmware
- ✅ Verification passes for valid firmware
- ✅ Verification fails for corrupted firmware
- ✅ Status shows "verifiedValid" or "verifiedInvalid"

**Estimated Effort:** 2 days

---

## Milestone 6: Bootloader API

**Goal:** Bootloader exposes verification API; application uses it

**Tasks:**

### 6.1 Define Bootloader API Structure
```c
// Core/Inc/bl_app_contract.h

#define BL_API_MAGIC    0x424C4150  // "BLAP"

typedef int (*fVerifyImage)(uint32_t extFlashAddr, uint32_t size, uint32_t expectedCrc);
typedef uint32_t (*fCalculateCrc32)(uint32_t addr, uint32_t size, bool isExternalFlash);
typedef void (*fGetBootloaderVersion)(uint32_t *major, uint32_t *minor, uint32_t *patch);

typedef struct {
    uint32_t magic;
    uint32_t version;

    fVerifyImage fVerifyImage;
    fCalculateCrc32 fCalculateCrc32;
    fGetBootloaderVersion fGetBootloaderVersion;
} sBootloaderApi;
```

### 6.2 Implement API in Bootloader
```c
// bootloader/src/boot_api.c

int verify_image_impl(uint32_t ext_flash_addr, uint32_t size, uint32_t expected_crc) {
    uint32_t calc_crc = crc32_calculate_external_flash(ext_flash_addr, size);
    return (calc_crc == expected_crc) ? 0 : -1;
}

uint32_t calculate_crc32_impl(uint32_t addr, uint32_t size, bool is_external_flash) {
    if (is_external_flash) {
        return crc32_calculate_external_flash(addr, size);
    } else {
        return crc32_calculate((uint8_t*)addr, size);
    }
}

void get_bootloader_version_impl(uint32_t *major, uint32_t *minor, uint32_t *patch) {
    *major = 1;
    *minor = 0;
    *patch = 0;
}

// API table at fixed address
const sBootloaderApi __attribute__((section(".bl_api"))) __attribute__((used)) bl_api = {
    .magic = BL_API_MAGIC,
    .version = 1,
    .fVerifyImage = verify_image_impl,
    .fCalculateCrc32 = calculate_crc32_impl,
    .fGetBootloaderVersion = get_bootloader_version_impl
};
```

### 6.3 Bootloader Linker Script
```ld
/* bootloader/linker/bootloader.ld */

MEMORY
{
    BOOTLOADER (rx) : ORIGIN = 0x08000000, LENGTH = 31K
    BL_API (rx)     : ORIGIN = 0x08007F00, LENGTH = 256
    RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 192K
}

SECTIONS
{
    .text : {
        /* Bootloader code */
    } > BOOTLOADER

    .bl_api 0x08007F00 : {
        KEEP(*(.bl_api))
    } > BL_API
}
```

### 6.4 Application Uses Bootloader API
```c
// application/src/app/firmware_update/fwu_verify.c

const sBootloaderApi* get_bootloader_api(void) {
    const sBootloaderApi *api = (const sBootloaderApi*)BL_API_ADDRESS;

    if (api->magic != BL_API_MAGIC) {
        return NULL;  // Bootloader API not available
    }

    return api;
}

void handle_firmware_upload(struct httpd_request *req) {
    // ... upload and extract sAppInfo ...

    // Use bootloader API for final verification
    const sBootloaderApi *bl_api = get_bootloader_api();

    if (bl_api != NULL) {
        int result = bl_api->fVerifyImage(EXT_FLASH_FWU_IMG_ADDR,
                                           app_info.appSize,
                                           app_info.appCrc32);

        if (result == 0) {
            g_fwuStatus.state = FWU_STATE_VERIFIED_VALID;
        } else {
            g_fwuStatus.state = FWU_STATE_VERIFIED_INVALID;
        }
    }

    // ...
}
```

### 6.5 Display Bootloader Info on Webpage
```html
<h2>Bootloader Information</h2>
<table>
    <tr><td>API Available:</td><td>YES</td></tr>
    <tr><td>API Address:</td><td>0x08007F00</td></tr>
    <tr><td>Version:</td><td>1.0.0</td></tr>
</table>
```

**Test:**
```bash
# Application should call bootloader API during upload verification
curl -X POST --data-binary @application.bin \
  http://192.168.0.135/api/firmware/upload

# Webpage should show bootloader version
```

**Deliverable:** Application calls bootloader API for verification

**Success Criteria:**
- ✅ Bootloader API table at 0x08007F00
- ✅ Application can read API and call functions
- ✅ Verification uses bootloader's CRC function
- ✅ Webpage shows bootloader version

**Estimated Effort:** 2 days

---

## Milestone 7: Bootloader Installation Logic

**Goal:** Bootloader installs firmware from external flash to internal flash

**Tasks:**

### 7.1 Implement Update Status Block
```c
// Core/Inc/bl_app_contract.h

#define UPDATE_STATUS_MAGIC 0x46575550  // "FWUP"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t updateRequested;   // 0 = staged, 1 = install requested
    uint32_t imageSize;
    uint32_t imageCrc32;
    uint32_t reserved[8];
    uint32_t headerCrc32;
} sUpdateStatus;
```

### 7.2 Application: Set Update Flag
```c
// application/src/app/firmware_update/http_handler.c

void handle_firmware_install(struct httpd_request *req) {
    // Check if verified firmware available
    if (g_fwuStatus.state != FWU_STATE_VERIFIED_VALID) {
        send_error_json(req, "No verified firmware available");
        return;
    }

    // Read sAppInfo from staged firmware
    sAppInfo app_info;
    w25q128_read(EXT_FLASH_FWU_IMG_ADDR + 0x200,
                 (uint8_t*)&app_info, sizeof(sAppInfo));

    // Write update status
    sUpdateStatus status = {
        .magic = UPDATE_STATUS_MAGIC,
        .version = 1,
        .updateRequested = 1,  // Request installation
        .imageSize = app_info.appSize,
        .imageCrc32 = app_info.appCrc32
    };
    status.headerCrc32 = crc32_calculate((uint8_t*)&status,
                                          sizeof(sUpdateStatus) - 4);

    w25q128_erase_sector(EXT_FLASH_STATUS_ADDR);
    w25q128_write(EXT_FLASH_STATUS_ADDR, (uint8_t*)&status, sizeof(sUpdateStatus));

    // Return response
    g_fwuStatus.state = FWU_STATE_DEPLOYING;
    send_fwu_status_json(req, &g_fwuStatus);

    // Delay and reboot
    HAL_Delay(3000);
    NVIC_SystemReset();
}
```

### 7.3 Bootloader: Read Status and Install
```c
// bootloader/src/boot_main.c

bool check_update_requested(sUpdateStatus *status) {
    // Read update status from external flash
    w25q128_read(EXT_FLASH_STATUS_ADDR, (uint8_t*)status, sizeof(sUpdateStatus));

    // Validate
    if (status->magic != UPDATE_STATUS_MAGIC) {
        return false;
    }

    uint32_t calc_crc = crc32_calculate((uint8_t*)status,
                                         sizeof(sUpdateStatus) - 4);
    if (calc_crc != status->headerCrc32) {
        return false;
    }

    return (status->updateRequested == 1);
}

void install_firmware_from_external_flash(sUpdateStatus *status) {
    // Verify image in external flash
    uint32_t calc_crc = crc32_calculate_external_flash(EXT_FLASH_FWU_IMG_ADDR,
                                                         status->imageSize);
    if (calc_crc != status->imageCrc32) {
        // Verification failed - abort
        return;
    }

    // Unlock flash
    HAL_FLASH_Unlock();

    // Erase application sectors (2-7)
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_SECTORS,
        .Sector = FLASH_SECTOR_2,
        .NbSectors = 6,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3
    };
    uint32_t error;
    HAL_FLASHEx_Erase(&erase, &error);

    // Program internal flash from external flash
    uint32_t src_addr = EXT_FLASH_FWU_IMG_ADDR;
    uint32_t dst_addr = APP_START_ADDRESS;
    uint32_t remaining = status->imageSize;

    while (remaining > 0) {
        uint8_t buffer[256];
        uint32_t chunk_size = (remaining > 256) ? 256 : remaining;

        w25q128_read(src_addr, buffer, chunk_size);

        // Program flash (word by word)
        for (uint32_t i = 0; i < chunk_size; i += 4) {
            uint32_t word = *(uint32_t*)(buffer + i);
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, dst_addr + i, word);
        }

        src_addr += chunk_size;
        dst_addr += chunk_size;
        remaining -= chunk_size;
    }

    // Lock flash
    HAL_FLASH_Lock();

    // Clear update request
    status->updateRequested = 0;
    status->headerCrc32 = crc32_calculate((uint8_t*)status,
                                           sizeof(sUpdateStatus) - 4);
    w25q128_erase_sector(EXT_FLASH_STATUS_ADDR);
    w25q128_write(EXT_FLASH_STATUS_ADDR, (uint8_t*)status, sizeof(sUpdateStatus));
}

void bootloader_main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_SPI_Init();
    w25q128_init();

    validate_or_write_uid();

    // Check for update request
    sUpdateStatus update_status;
    if (check_update_requested(&update_status)) {
        // Install firmware from external flash
        install_firmware_from_external_flash(&update_status);
    }

    // Validate application
    if (validate_application()) {
        jump_to_application(APP_START_ADDRESS);
    }

    // Error - hang
    while(1) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        HAL_Delay(100);
    }
}
```

### 7.4 Implement POST /api/firmware/schedule
```c
void handle_firmware_schedule(struct httpd_request *req) {
    // Parse JSON body
    // {"installAt": "2024-01-15T02:00:00Z"}
    // OR {"delaySeconds": 3600}

    // Store scheduled time
    strncpy(g_fwuStatus.scheduledInstall, scheduled_time_iso8601, 32);

    // Background task checks scheduled time and triggers install

    send_fwu_status_json(req, &g_fwuStatus);
}
```

### 7.5 Implement DELETE /api/firmware/staged
```c
void handle_firmware_cancel(struct httpd_request *req) {
    // Erase firmware area in external flash
    for (uint32_t addr = EXT_FLASH_FWU_IMG_ADDR;
         addr < EXT_FLASH_FWU_IMG_ADDR + (480 * 1024);
         addr += 4096) {
        w25q128_erase_sector(addr);
    }

    // Clear update status
    sUpdateStatus status = {0};
    w25q128_erase_sector(EXT_FLASH_STATUS_ADDR);

    // Reset FWU status
    g_fwuStatus.state = FWU_STATE_NO_FILE;
    g_fwuStatus.size = 0;
    g_fwuStatus.version[0] = '\0';
    g_fwuStatus.scheduledInstall[0] = '\0';

    send_fwu_status_json(req, &g_fwuStatus);
}
```

**Test:**
```bash
# Upload firmware
curl -X POST --data-binary @application.bin \
  http://192.168.0.135/api/firmware/upload

# Install
curl -X POST http://192.168.0.135/api/firmware/install

# Device reboots...
# Bootloader installs firmware from external flash
# Boots into new application

# Verify still works
curl http://192.168.0.135/
```

**Deliverable:** Bootloader installs firmware from external flash

**Success Criteria:**
- ✅ Application sets update flag in external flash
- ✅ Bootloader reads update flag on boot
- ✅ Bootloader erases and programs internal flash
- ✅ New application boots successfully
- ✅ Update status cleared after installation
- ✅ Can repeat update process

**Estimated Effort:** 3-4 days

---

## Milestone 8: End-to-End OTA Update ⭐

**Goal:** Complete OTA update cycle - upload new version, install, verify

**Tasks:**

### 8.1 Create "Hello World v2.0.0"
```c
// application/src/app_info.c

const sAppInfo app_info = {
    .magic = APP_INFO_MAGIC,
    .structVersion = 1,

    .fwName = "PeriphNet",
    .fwVersion = "2.0.0",  // Changed!
    // ...
};
```

```html
<!-- Update webpage -->
<h1>Hello World v2.0.0</h1>
<p style="color: green;">Firmware update successful!</p>
```

### 8.2 Build v2.0.0 Firmware
```bash
cd build
make application
# Produces application.bin with version 2.0.0
```

### 8.3 Perform Complete OTA Update
```bash
# 1. Check current version
curl http://192.168.0.135/
# Shows: Hello World v1.0.0

# 2. Upload v2.0.0
curl -X POST --data-binary @application_v2.bin \
  http://192.168.0.135/api/firmware/upload

# Response:
# {
#   "status": "verifiedValid",
#   "size": 246848,
#   "version": "2.0.0",
#   "scheduledInstall": null
# }

# 3. Verify status
curl http://192.168.0.135/api/firmware/status
# Confirms v2.0.0 ready to install

# 4. Install immediately
curl -X POST http://192.168.0.135/api/firmware/install

# Response:
# {
#   "status": "deploying",
#   "message": "Device will reboot in 3 seconds",
#   "rebootIn": 3
# }

# Wait for reboot (bootloader installs firmware)...

# 5. Verify new version running
curl http://192.168.0.135/
# Shows: Hello World v2.0.0

# 6. Verify update can be repeated (v3, v4, etc.)
```

### 8.4 Test Alternative Workflows

**Scheduled install:**
```bash
# Upload v3.0.0
curl -X POST --data-binary @application_v3.bin \
  http://192.168.0.135/api/firmware/upload

# Schedule for later
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"delaySeconds": 300}' \
  http://192.168.0.135/api/firmware/schedule

# Check status
curl http://192.168.0.135/api/firmware/status
# Shows scheduledInstall timestamp

# Wait 5 minutes - device auto-installs
```

**Cancel staged update:**
```bash
# Upload firmware
curl -X POST --data-binary @application.bin \
  http://192.168.0.135/api/firmware/upload

# Cancel before installing
curl -X DELETE http://192.168.0.135/api/firmware/staged

# Verify cleared
curl http://192.168.0.135/api/firmware/status
# Shows: "status": "noFile"
```

### 8.5 Final Webpage with All Information
```html
<!DOCTYPE html>
<html>
<head>
    <title>PeriphNet v2.0.0</title>
    <style>
        body { font-family: Arial; margin: 20px; }
        table { border-collapse: collapse; margin: 10px 0; }
        td { padding: 8px; border: 1px solid #ddd; }
        .success { color: green; font-weight: bold; }
    </style>
</head>
<body>
    <h1>Hello World v2.0.0</h1>
    <p class="success">Firmware update successful!</p>

    <h2>System Information</h2>
    <table>
        <tr><td>Device:</td><td>STM32F407VET6</td></tr>
        <tr><td>Firmware:</td><td>PeriphNet v2.0.0</td></tr>
        <tr><td>Boot Source:</td><td>Bootloader</td></tr>
        <tr><td>Application Base:</td><td>0x08008000</td></tr>
        <tr><td>IP Address:</td><td>192.168.0.135</td></tr>
        <tr><td>MAC Address:</td><td>10:06:1C:A7:9E:D7</td></tr>
        <tr><td>Uptime:</td><td>325 seconds</td></tr>
    </table>

    <h2>MCU Unique ID</h2>
    <table>
        <tr><td>UID[0]:</td><td>0x12345678</td></tr>
        <tr><td>UID[1]:</td><td>0x9ABCDEF0</td></tr>
        <tr><td>UID[2]:</td><td>0x11223344</td></tr>
    </table>

    <h2>External Flash (W25Q128)</h2>
    <table>
        <tr><td>JEDEC ID:</td><td>0xEF4018</td></tr>
        <tr><td>Manufacturer:</td><td>Winbond (0xEF)</td></tr>
        <tr><td>Capacity:</td><td>16 MB</td></tr>
        <tr><td>UID Stored:</td><td class="success">YES</td></tr>
        <tr><td>UID Valid:</td><td class="success">MATCH</td></tr>
    </table>

    <h2>Bootloader Information</h2>
    <table>
        <tr><td>Version:</td><td>1.0.0</td></tr>
        <tr><td>API Available:</td><td class="success">YES</td></tr>
        <tr><td>API Address:</td><td>0x08007F00</td></tr>
    </table>

    <h2>Firmware Update Status</h2>
    <table>
        <tr><td>Status:</td><td id="fwu-status">noFile</td></tr>
        <tr><td>Staged Size:</td><td id="fwu-size">0 bytes</td></tr>
        <tr><td>Staged Version:</td><td id="fwu-version">-</td></tr>
        <tr><td>Scheduled Install:</td><td id="fwu-schedule">-</td></tr>
    </table>

    <h2>Update API</h2>
    <pre>
POST   /api/firmware/upload    - Upload firmware binary
GET    /api/firmware/status    - Get update status
POST   /api/firmware/install   - Install staged firmware
POST   /api/firmware/schedule  - Schedule installation
DELETE /api/firmware/staged    - Cancel staged update
    </pre>

    <script>
    // Auto-refresh FWU status
    setInterval(function() {
        fetch('/api/firmware/status')
            .then(r => r.json())
            .then(data => {
                document.getElementById('fwu-status').textContent = data.status;
                document.getElementById('fwu-size').textContent = data.size + ' bytes';
                document.getElementById('fwu-version').textContent = data.version || '-';
                document.getElementById('fwu-schedule').textContent =
                    data.scheduledInstall || '-';
            });
    }, 2000);
    </script>
</body>
</html>
```

**Deliverable:** 🎉 **Complete OTA firmware update system working!**

**Success Criteria:**
- ✅ Upload firmware v2.0.0 via HTTP
- ✅ Verification passes automatically
- ✅ Install triggers bootloader update
- ✅ Bootloader installs from external flash
- ✅ Device boots into v2.0.0
- ✅ Webpage shows "Hello World v2.0.0"
- ✅ Can repeat process for v3, v4, etc.
- ✅ Scheduled install works
- ✅ Cancel staged update works
- ✅ All system information displayed correctly

**Estimated Effort:** 2-3 days (integration and testing)

---

## Total Timeline

| Milestone | Description | Effort | Cumulative |
|-----------|-------------|--------|------------|
| M0 | Development Environment | 1-2 days | 2 days |
| M1 | Ethernet + HTTP Server | 3-4 days | 6 days |
| M2 | Bootloader ↔ App Jump | 2-3 days | 9 days |
| M3 | External Flash + UID Contract | 2-3 days | 12 days |
| M4 | HTTP Upload | 2 days | 14 days |
| M5 | Firmware Verification | 2 days | 16 days |
| M6 | Bootloader API | 2 days | 18 days |
| M7 | Installation Logic | 3-4 days | 22 days |
| M8 | End-to-End OTA | 2-3 days | **25 days** |

**Total: ~5 weeks (25 working days)**

---

## Summary

This plan builds incrementally:

1. **M0**: Foundation - build system works
2. **M1**: Network works - webpage as debug interface
3. **M2**: BL-APP jump works - two-binary system proven
4. **M3**: External flash works - BL writes UID, APP validates
5. **M4**: Upload works - can receive firmware over network
6. **M5**: Verification works - can validate firmware integrity
7. **M6**: API contract works - APP calls BL functions
8. **M7**: Installation works - BL can program internal flash
9. **M8**: **Complete OTA system working end-to-end**

Each milestone validates via **webpage**, not just LEDs, providing rich debugging information throughout development.

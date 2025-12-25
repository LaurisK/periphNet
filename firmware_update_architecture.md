# Firmware Update Architecture

## Overview

**Separation of concerns:**
- **Application**: Downloads firmware to external flash via Ethernet
- **Bootloader**: Verifies and installs firmware from external flash

## Memory Architecture

### Internal Flash (STM32F407VET6 - 512KB)
```
0x0800 0000  ┌──────────────┐
             │ Bootloader   │ 32KB (Sectors 0-1)
0x0800 8000  ├──────────────┤
             │ Application  │ 480KB (Sectors 2-7)
0x0808 0000  └──────────────┘
```

### External Flash (W25Q128 - 16MB)
```
0x0000 0000  ┌──────────────┐
             │ FWU Status   │ 4KB
0x0000 1000  ├──────────────┤
             │ FWU Image    │ 480KB
0x0007 9000  ├──────────────┤
             │ Golden Image │ 480KB
0x000F 1000  ├──────────────┤
             │ Undefined    │ ~15MB (future use)
             └──────────────┘
```

## Update Status Block (External Flash)

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

## Bootloader API

**Location:** 0x08007F00 (near end of 32KB bootloader space)

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

**Application Usage:**
```c
const bootloader_api_t *bl_api = (const bootloader_api_t*)0x08007F00;

// Verify before setting update flag
int result = bl_api->verify_image(EXT_FLASH_FWU_IMG_ADDR, size, expected_crc);
if (result == 0) {
    write_update_status(...);
    NVIC_SystemReset();
}
```

## Update Flow

### 1. Application Downloads Firmware
- HTTP/TFTP server receives firmware
- Streams to external flash
- Calls `bl_api->verify_image()`
- Sets update flag
- Reboots

### 2. Bootloader Installs Firmware
- Reads update flag
- Verifies image in external flash
- Erases application area (Sectors 2-7)
- Programs internal flash from external flash
- Clears update flag
- Jumps to new application

## Configuration

```c
// Bootloader
#define BOOTLOADER_SIZE         (32 * 1024)
#define APP_START_ADDRESS       0x08008000
#define APP_MAX_SIZE            (480 * 1024)

// External flash (W25Q128)
#define EXT_FLASH_STATUS_ADDR   0x00000000      // 4KB
#define EXT_FLASH_FWU_IMG_ADDR  0x00001000      // 480KB
#define EXT_FLASH_GOLDEN_ADDR   0x00079000      // 480KB
// Remaining space undefined (future use)
```

#ifndef BL_APP_CONTRACT_H
#define BL_APP_CONTRACT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL_API_TABLE_ADDR            0x08007F00u
#define APP_INFO_HEADER_ADDR         0x08008200u
#define BOOTLOADER_START_ADDR        0x08000000u
#define BOOTLOADER_SIZE              (32 * 1024)
#define APPLICATION_START_ADDR       0x08008000u
#define APPLICATION_SIZE             (480 * 1024)
#define EXT_FLASH_BASE               0x00000000u
#define EXT_FLASH_FWU_STATUS_ADDR    0x00000000u
#define EXT_FLASH_FWU_STATUS_SIZE    0x00001000u
#define EXT_FLASH_FWU_IMG_ADDR       0x00001000u
#define EXT_FLASH_FWU_IMG_SIZE       0x00078000u
#define EXT_FLASH_GOLDEN_IMG_ADDR    0x00079000u
#define EXT_FLASH_GOLDEN_IMG_SIZE    0x00078000u

#define BL_API_MAGIC                 0x424C4150u
#define APP_INFO_MAGIC               0x41505049u
#define UPDATE_STATUS_MAGIC          0x46575550u

typedef int (*fVerifyImage)(uint32_t ext_flash_addr, uint32_t size, uint32_t expected_crc);
typedef uint32_t (*fCalculateCrc32)(uint32_t addr, uint32_t size, bool is_external_flash);
typedef void (*fGetBootloaderVersion)(uint32_t *major, uint32_t *minor, uint32_t *patch);
typedef int (*fVerifyInternalApp)(void);

typedef struct {
    uint32_t magic;
    uint32_t version;
    fVerifyImage verify_image;
    fCalculateCrc32 calculate_crc32;
    fVerifyInternalApp verify_internal_app;
    fGetBootloaderVersion get_bootloader_version;
    uint32_t reserved[11];
} sBootloaderApi;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t build_time;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t features;
    uint32_t min_bl_version;
    uint32_t reset_handler_addr;
    uint32_t vector_table_addr;
    uint32_t reserved[5];
} sAppInfo;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t update_requested;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t image_offset;
    uint8_t image_sha256[32];
    uint32_t app_version;
    uint32_t install_time;
    uint32_t reserved[15];
    uint32_t header_crc32;
} sUpdateStatus;

#define APP_FEATURE_ETHERNET         (1u << 0)
#define APP_FEATURE_MQTT             (1u << 1)
#define APP_FEATURE_MODBUS           (1u << 2)
#define APP_FEATURE_OTA              (1u << 3)
#define APP_FEATURE_TRACING          (1u << 4)
#define APP_FEATURE_RTC              (1u << 5)
#define APP_FEATURE_CAN              (1u << 6)

#define BL_OK                        0
#define BL_ERROR                     1
#define BL_INVALID_ADDR              2
#define BL_INVALID_SIZE              3
#define BL_INVALID_CRC               4
#define BL_FLASH_ERROR               5
#define BL_TIMEOUT                   6
#define BL_NOT_SUPPORTED             7
#define BL_WRONG_MAGIC               8
#define BL_VERSION_MISMATCH          9

#ifdef __cplusplus
}
#endif

#endif /* BL_APP_CONTRACT_H */

/**
 ******************************************************************************
 * @file    bl_app_contract.h
 * @brief   PeriphNet Bootloader-Application Interface Contract
 *          Defines structures and addresses for bootloader-application communication
 ******************************************************************************
 * @attention
 *
 * Bootloader-Application Communication Contract
 *
 * 1. Bootloader exposes API at fixed address 0x08007F00
 * 2. Application provides metadata at fixed address 0x08008200
 * 3. Both use this shared header for structure definitions
 *
 ******************************************************************************
 */

#ifndef BL_APP_CONTRACT_H
#define BL_APP_CONTRACT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * CRITICAL MEMORY ADDRESSES - DO NOT CHANGE
 * =============================================================================
 */

/* Bootloader API Table (fixed address) */
#define BL_API_TABLE_ADDR            0x08007F00u

/* Application Info Header (fixed address) */
#define APP_INFO_HEADER_ADDR          0x08008200u

/* Bootloader Memory Region */
#define BOOTLOADER_START_ADDR        0x08000000u
#define BOOTLOADER_SIZE             (32 * 1024)     /* 32KB */

/* Application Memory Region */
#define APPLICATION_START_ADDR        0x08008000u
#define APPLICATION_SIZE             (480 * 1024)    /* 480KB */

/* External Flash Memory Map */
#define EXT_FLASH_BASE               0x00000000u
#define EXT_FLASH_FWU_STATUS_ADDR    0x00000000u    /* 4KB - Firmware update status */
#define EXT_FLASH_FWU_STATUS_SIZE    0x00001000u
#define EXT_FLASH_FWU_IMG_ADDR       0x00001000u    /* 480KB - Downloaded firmware */
#define EXT_FLASH_FWU_IMG_SIZE      0x00078000u
#define EXT_FLASH_GOLDEN_IMG_ADDR    0x00079000u    /* 480KB - Factory/known-good */
#define EXT_FLASH_GOLDEN_IMG_SIZE    0x00078000u

/* =============================================================================
 * MAGIC NUMBERS FOR AUTHENTICATION
 * =============================================================================
 */

/* Bootloader API table magic */
#define BL_API_MAGIC                 0x424C4150u    /* "BLAP" */

/* Application info header magic */
#define APP_INFO_MAGIC               0x41505049u    /* "APPI" */

/* Update status block magic */
#define UPDATE_STATUS_MAGIC          0x46575550u    /* "FWUP" */

/* =============================================================================
 * BOOTLOADER API STRUCTURE
 * =============================================================================
 *
 * Bootloader exposes this structure at fixed address 0x08007F00
 * Application can call these functions to verify images, calculate CRCs, etc.
 *
 */

/* Forward declarations for function pointers */
typedef int (*fVerifyImage)(uint32_t ext_flash_addr, uint32_t size, uint32_t expected_crc);
typedef uint32_t (*fCalculateCrc32)(uint32_t addr, uint32_t size, bool is_external_flash);
typedef void (*fGetBootloaderVersion)(uint32_t *major, uint32_t *minor, uint32_t *patch);
typedef int (*fVerifyInternalApp)(void);

/**
 * @brief Bootloader API Table Structure
 * @note Located at fixed address 0x08007F00 (256 bytes reserved)
 */
typedef struct {
    uint32_t magic;                              /* Must be BL_API_MAGIC (0x424C4150) */
    uint32_t version;                             /* API version (currently 1) */
    
    /* Verification and validation functions */
    fVerifyImage verify_image;                     /* Verify firmware in external flash */
    fCalculateCrc32 calculate_crc32;               /* Calculate CRC32 of memory region */
    fVerifyInternalApp verify_internal_app;         /* Verify current application */
    
    /* Version and information functions */
    fGetBootloaderVersion get_bootloader_version;   /* Get bootloader version info */
    
    /* Reserved for future expansion */
    uint32_t reserved[11];                        /* Total 64 bytes (16 * 4) */
} sBootloaderApi;

/* =============================================================================
 * APPLICATION INFO STRUCTURE
 * =============================================================================
 *
 * Application provides this structure at fixed address 0x08008200
 * Bootloader reads this during boot to validate application
 *
 */

/**
 * @brief Application Info Header Structure
 * @note Located at fixed address 0x08008200 (right after vector table)
 */
typedef struct {
    uint32_t magic;                              /* Must be APP_INFO_MAGIC (0x41505049) */
    uint32_t version;                             /* Application version */
    uint32_t build_time;                          /* Unix timestamp of build */
    uint32_t image_size;                          /* Size of application image */
    uint32_t image_crc32;                         /* CRC32 of application image */
    
    /* Feature flags (bitfield) */
    uint32_t features;                            /* Bit flags for enabled features */
    
    /* Minimum required bootloader version */
    uint32_t min_bl_version;                      /* Minimum bootloader version required */
    
    /* Application entry points (for debugging/testing) */
    uint32_t reset_handler_addr;                   /* Address of Reset_Handler */
    uint32_t vector_table_addr;                    /* Address of vector table */
    
    /* Reserved for future use */
    uint32_t reserved[5];                         /* Total 64 bytes (16 * 4) */
} sAppInfo;

/* =============================================================================
 * UPDATE STATUS STRUCTURE
 * =============================================================================
 *
 * This structure is stored in external flash at 0x00000000
 * Bootloader reads this on boot to check for pending updates
 *
 */

/**
 * @brief Update Status Block Structure
 * @note Located in external flash at EXT_FLASH_FWU_STATUS_ADDR (0x00000000)
 */
typedef struct {
    uint32_t magic;                              /* Must be UPDATE_STATUS_MAGIC */
    uint32_t version;                             /* Status block version */
    uint32_t update_requested;                    /* 1 = pending, 0 = none */
    uint32_t image_size;                          /* Size of firmware to install */
    uint32_t image_crc32;                         /* CRC32 of firmware to install */
    uint32_t image_offset;                         /* Offset in external flash (usually 0x1000) */
    uint8_t image_sha256[32];                     /* SHA256 hash of firmware */
    uint32_t app_version;                         /* New application version */
    uint32_t install_time;                        /* Scheduled install time (unix timestamp) */
    uint32_t reserved[15];                        /* Reserved for future use */
    uint32_t header_crc32;                       /* CRC32 of this header */
} sUpdateStatus;

/* =============================================================================
 * FEATURE FLAGS
 * =============================================================================
 */

/* Application feature flags (sAppInfo.features) */
#define APP_FEATURE_ETHERNET         (1u << 0)   /* Ethernet/networking support */
#define APP_FEATURE_MQTT             (1u << 1)   /* MQTT client support */
#define APP_FEATURE_MODBUS            (1u << 2)   /* Modbus RTU support */
#define APP_FEATURE_OTA              (1u << 3)   /* OTA update support */
#define APP_FEATURE_TRACING          (1u << 4)   /* Trice tracing support */
#define APP_FEATURE_RTC              (1u << 5)   /* RTC/time support */
#define APP_FEATURE_CAN              (1u << 6)   /* CAN bus support */

/* =============================================================================
 * RESULT CODES
 * =============================================================================
 */

/* Common result codes for bootloader API functions */
#define BL_OK                      0           /* Success */
#define BL_ERROR                    1           /* General error */
#define BL_INVALID_ADDR             2           /* Invalid address */
#define BL_INVALID_SIZE             3           /* Invalid size */
#define BL_INVALID_CRC              4           /* CRC verification failed */
#define BL_FLASH_ERROR             5           /* Flash operation failed */
#define BL_TIMEOUT                 6           /* Operation timeout */
#define BL_NOT_SUPPORTED           7           /* Operation not supported */
#define BL_WRONG_MAGIC             8           /* Wrong magic number */
#define BL_VERSION_MISMATCH        9           /* Version mismatch */

/* =============================================================================
 * USAGE EXAMPLES
 * =============================================================================
 */

/*
 * Bootloader API usage (from application):
 *
 * const sBootloaderApi *bl_api = (const sBootloaderApi*)BL_API_TABLE_ADDR;
 *
 * // Verify magic before using API
 * if (bl_api->magic == BL_API_MAGIC) {
 *     // Calculate CRC of external flash image
 *     uint32_t crc = bl_api->calculate_crc32(EXT_FLASH_FWU_IMG_ADDR, image_size, true);
 *     
 *     // Verify image before installation
 *     int result = bl_api->verify_image(EXT_FLASH_FWU_IMG_ADDR, image_size, expected_crc);
 *     
 *     if (result == BL_OK) {
 *         // Trigger update
 *         sUpdateStatus status = {
 *             .magic = UPDATE_STATUS_MAGIC,
 *             .update_requested = 1,
 *             .image_size = image_size,
 *             .image_crc32 = expected_crc,
 *             // ... other fields
 *         };
 *         // Write status to external flash and reboot
 *     }
 * }
 *
 * // Get bootloader version
 * uint32_t major, minor, patch;
 * bl_api->get_bootloader_version(&major, &minor, &patch);
 */

/*
 * Application info usage (bootloader reads this):
 *
 * const sAppInfo *app_info = (const sAppInfo*)APP_INFO_HEADER_ADDR;
 *
 * // Validate application before jumping
 * if (app_info->magic == APP_INFO_MAGIC) {
 *     if (app_info->min_bl_version <= current_bl_version) {
 *         if (app_info->features & APP_FEATURE_ETHERNET) {
 *             // Application supports Ethernet
 *         }
 *         // Jump to application
 *         uint32_t stack_ptr = *((uint32_t*)APPLICATION_START_ADDR);
 *         uint32_t reset_handler = *((uint32_t*)(APPLICATION_START_ADDR + 4));
 *         // Set stack pointer and jump
 *     }
 * }
 */

#ifdef __cplusplus
}
#endif

#endif /* BL_APP_CONTRACT_H */
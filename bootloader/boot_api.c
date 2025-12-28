/**
 ******************************************************************************
 * @file    boot_api.c
 * @brief   Bootloader API Implementation
 * @date    Dec 27, 2024
 ******************************************************************************
 * @attention
 *
 * Implements the bootloader API exposed to the application at 0x08007F00
 * Functions are stubs for Phase 3 - actual CRC checking will be added later
 *
 ******************************************************************************
 */

#include "bl_app_contract.h"
#include "main.h"

/* Bootloader version */
#define BOOTLOADER_VERSION_MAJOR    1
#define BOOTLOADER_VERSION_MINOR    0
#define BOOTLOADER_VERSION_PATCH    0

/* =============================================================================
 * STUB FUNCTION IMPLEMENTATIONS (Phase 3 - No actual CRC checking yet)
 * =============================================================================
 */

/**
 * @brief Verify firmware image in external flash (STUB)
 * @param ext_flash_addr Address in external flash
 * @param size Size of image in bytes
 * @param expected_crc Expected CRC32 value
 * @return BL_OK if successful, error code otherwise
 */
static int bl_verify_image(uint32_t ext_flash_addr, uint32_t size, uint32_t expected_crc)
{
    /* STUB: Phase 3 - No actual verification yet */
    /* TODO Phase 4: Implement actual CRC verification */

    /* Basic sanity checks */
    if (ext_flash_addr >= 0x00800000) {  /* Beyond 8MB */
        return BL_INVALID_ADDR;
    }

    if (size == 0 || size > APPLICATION_SIZE) {
        return BL_INVALID_SIZE;
    }

    /* Stub: Always return OK for now */
    return BL_OK;
}

/**
 * @brief Calculate CRC32 of memory region (STUB)
 * @param addr Address to start calculating from
 * @param size Number of bytes to process
 * @param is_external_flash True if external flash, false if internal flash
 * @return Calculated CRC32 value (stub returns dummy value)
 */
static uint32_t bl_calculate_crc32(uint32_t addr, uint32_t size, bool is_external_flash)
{
    /* STUB: Phase 3 - No actual CRC calculation yet */
    /* TODO Phase 4: Implement STM32 hardware CRC or software CRC32 */

    /* Return dummy CRC for now */
    return 0xDEADBEEF;
}

/**
 * @brief Verify currently running application (STUB)
 * @return BL_OK if valid, error code otherwise
 */
static int bl_verify_internal_app(void)
{
    /* STUB: Phase 3 - No actual verification yet */
    /* TODO Phase 4: Read app_info header and verify CRC */

    const sAppInfo *app_info = (const sAppInfo*)APP_INFO_HEADER_ADDR;

    /* Check magic number */
    if (app_info->magic != APP_INFO_MAGIC) {
        return BL_WRONG_MAGIC;
    }

    /* Stub: Don't verify CRC yet */
    return BL_OK;
}

/**
 * @brief Get bootloader version information
 * @param major Pointer to store major version
 * @param minor Pointer to store minor version
 * @param patch Pointer to store patch version
 */
static void bl_get_bootloader_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (major) *major = BOOTLOADER_VERSION_MAJOR;
    if (minor) *minor = BOOTLOADER_VERSION_MINOR;
    if (patch) *patch = BOOTLOADER_VERSION_PATCH;
}

/* =============================================================================
 * BOOTLOADER API TABLE
 * =============================================================================
 */

/**
 * @brief Bootloader API Table
 * @note Placed at fixed address 0x08007F00 via linker script
 * @note This is the contract between bootloader and application
 */
__attribute__((section(".bl_api")))
__attribute__((used))
const sBootloaderApi bootloader_api = {
    .magic = BL_API_MAGIC,
    .version = 1,
    .verify_image = bl_verify_image,
    .calculate_crc32 = bl_calculate_crc32,
    .verify_internal_app = bl_verify_internal_app,
    .get_bootloader_version = bl_get_bootloader_version,
    .reserved = {0}
};

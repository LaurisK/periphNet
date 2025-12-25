#include "bl_app_contract.h"
#include "main.h"

#define BOOTLOADER_VERSION_MAJOR    1
#define BOOTLOADER_VERSION_MINOR    0
#define BOOTLOADER_VERSION_PATCH    0

/**
 * @brief Verify a firmware image stored in external flash.
 * @param ext_flash_addr Start address of the image in external flash.
 * @param size Size of the image in bytes.
 * @param expected_crc Expected CRC32 of the image (not yet checked).
 * @return BL_OK if basic sanity checks pass, BL_INVALID_ADDR or BL_INVALID_SIZE otherwise.
 */
static int bl_verify_image(uint32_t ext_flash_addr, uint32_t size, uint32_t expected_crc)
{
    if (ext_flash_addr >= 0x00800000) {
        return BL_INVALID_ADDR;
    }

    if (size == 0 || size > APPLICATION_SIZE) {
        return BL_INVALID_SIZE;
    }

    return BL_OK;
}

/**
 * @brief Calculate CRC32 of a memory region (stub).
 * @param addr Start address of the region.
 * @param size Number of bytes to process.
 * @param is_external_flash True if the address is in external flash, false for internal.
 * @return Dummy value 0xDEADBEEF until real CRC is implemented.
 */
static uint32_t bl_calculate_crc32(uint32_t addr, uint32_t size, bool is_external_flash)
{
    return 0xDEADBEEF;
}

/**
 * @brief Verify the currently running application by checking its magic number.
 * @return BL_OK if the application info magic is valid, BL_WRONG_MAGIC otherwise.
 */
static int bl_verify_internal_app(void)
{
    const sAppInfo *app_info = (const sAppInfo*)APP_INFO_HEADER_ADDR;

    if (app_info->magic != APP_INFO_MAGIC) {
        return BL_WRONG_MAGIC;
    }

    return BL_OK;
}

/**
 * @brief Return the bootloader version numbers.
 * @param major Pointer to store the major version; may be NULL.
 * @param minor Pointer to store the minor version; may be NULL.
 * @param patch Pointer to store the patch version; may be NULL.
 */
static void bl_get_bootloader_version(uint32_t *major, uint32_t *minor, uint32_t *patch)
{
    if (major) *major = BOOTLOADER_VERSION_MAJOR;
    if (minor) *minor = BOOTLOADER_VERSION_MINOR;
    if (patch) *patch = BOOTLOADER_VERSION_PATCH;
}

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

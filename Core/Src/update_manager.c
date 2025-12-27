/**
 ******************************************************************************
 * @file    update_manager.c
 * @brief   Firmware Update Manager - Shared between Bootloader and Application
 * @date    Dec 27, 2024
 ******************************************************************************
 * @attention
 *
 * Manages firmware update status in external flash
 * Shared code compiled into both bootloader and application
 *
 ******************************************************************************
 */

#include "update_manager.h"
#include "w25q128.h"
#include <string.h>

/**
 * @brief Read update status from external flash
 * @param status Pointer to update status structure
 * @return 0 if successful, -1 if error
 */
int update_status_read(sUpdateStatus *status)
{
    if (!status) {
        return -1;
    }

    /* Read status block from external flash */
    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR, (uint8_t*)status, sizeof(sUpdateStatus)) != W25Q128_OK) {
        return -1;
    }

    /* Validate magic number */
    if (status->magic != UPDATE_STATUS_MAGIC) {
        /* Not initialized or corrupted - return empty status */
        memset(status, 0, sizeof(sUpdateStatus));
        return -1;
    }

    /* TODO Phase 5: Verify header CRC32 */

    return 0;
}

/**
 * @brief Write update status to external flash
 * @param status Pointer to update status structure
 * @return 0 if successful, -1 if error
 */
int update_status_write(const sUpdateStatus *status)
{
    if (!status) {
        return -1;
    }

    /* Erase the status sector */
    if (W25Q128_EraseSector(EXT_FLASH_FWU_STATUS_ADDR) != W25Q128_OK) {
        return -1;
    }

    /* Write status block to external flash */
    /* Split into pages if needed (W25Q page size is 256 bytes) */
    uint32_t bytes_written = 0;
    const uint8_t *data = (const uint8_t*)status;

    while (bytes_written < sizeof(sUpdateStatus)) {
        uint32_t chunk_size = 256;
        if (bytes_written + chunk_size > sizeof(sUpdateStatus)) {
            chunk_size = sizeof(sUpdateStatus) - bytes_written;
        }

        if (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR + bytes_written,
                             data + bytes_written,
                             chunk_size) != W25Q128_OK) {
            return -1;
        }

        bytes_written += chunk_size;
    }

    return 0;
}

/**
 * @brief Clear update status (no update pending)
 * @return 0 if successful, -1 if error
 */
int update_status_clear(void)
{
    sUpdateStatus status = {0};
    status.magic = UPDATE_STATUS_MAGIC;
    status.version = 1;
    status.update_requested = 0;

    return update_status_write(&status);
}

/**
 * @brief Request firmware update
 * @param image_size Size of firmware image in bytes
 * @param image_crc32 CRC32 of firmware image
 * @param app_version New application version
 * @return 0 if successful, -1 if error
 */
int update_status_request(uint32_t image_size, uint32_t image_crc32, uint32_t app_version)
{
    sUpdateStatus status = {0};

    status.magic = UPDATE_STATUS_MAGIC;
    status.version = 1;
    status.update_requested = 1;
    status.image_size = image_size;
    status.image_crc32 = image_crc32;
    status.image_offset = EXT_FLASH_FWU_IMG_ADDR;
    status.app_version = app_version;
    status.install_time = 0;  /* Install immediately on next boot */

    /* TODO Phase 5: Calculate header CRC32 */
    status.header_crc32 = 0;

    return update_status_write(&status);
}

/**
 * @brief Check if update is pending
 * @return 1 if update pending, 0 if not, -1 if error
 */
int update_status_is_pending(void)
{
    sUpdateStatus status;

    if (update_status_read(&status) != 0) {
        return 0;  /* No valid status = no update pending */
    }

    return (status.update_requested == 1) ? 1 : 0;
}

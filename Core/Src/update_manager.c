#include "update_manager.h"
#include "w25q128.h"
#include <string.h>

/**
 * @brief Read update status from external flash.
 * @param status Pointer to update status structure to populate.
 * @return 0 on success, -1 if flash read fails or magic is invalid.
 */
int update_status_read(sUpdateStatus *status)
{
    if (!status) {
        return -1;
    }

    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR, (uint8_t*)status, sizeof(sUpdateStatus)) != W25Q128_OK) {
        return -1;
    }

    if (status->magic != UPDATE_STATUS_MAGIC) {
        memset(status, 0, sizeof(sUpdateStatus));
        return -1;
    }

    return 0;
}

/**
 * @brief Write update status to external flash.
 * @param status Pointer to update status structure to write.
 * @return 0 on success, -1 if erase or write fails.
 */
int update_status_write(const sUpdateStatus *status)
{
    if (!status) {
        return -1;
    }

    if (W25Q128_EraseSector(EXT_FLASH_FWU_STATUS_ADDR) != W25Q128_OK) {
        return -1;
    }

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
 * @brief Clear update status block, marking no update pending.
 * @return 0 on success, -1 on flash write failure.
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
 * @brief Write a firmware update request to external flash.
 * @param image_size Size of the firmware image in bytes.
 * @param image_crc32 CRC32 checksum of the firmware image.
 * @param app_version New application version number.
 * @return 0 on success, -1 on flash write failure.
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
    status.install_time = 0;
    status.header_crc32 = 0;

    return update_status_write(&status);
}

/**
 * @brief Check whether a firmware update is pending.
 * @return 1 if update is pending, 0 if not or if status is invalid.
 */
int update_status_is_pending(void)
{
    sUpdateStatus status;

    if (update_status_read(&status) != 0) {
        return 0;
    }

    return (status.update_requested == 1) ? 1 : 0;
}

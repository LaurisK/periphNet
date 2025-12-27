/**
 ******************************************************************************
 * @file    update_manager.h
 * @brief   Firmware Update Manager Header
 * @date    Dec 27, 2024
 ******************************************************************************
 */

#ifndef UPDATE_MANAGER_H
#define UPDATE_MANAGER_H

#include "bl_app_contract.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read update status from external flash
 * @param status Pointer to update status structure
 * @return 0 if successful, -1 if error
 */
int update_status_read(sUpdateStatus *status);

/**
 * @brief Write update status to external flash
 * @param status Pointer to update status structure
 * @return 0 if successful, -1 if error
 */
int update_status_write(const sUpdateStatus *status);

/**
 * @brief Clear update status (no update pending)
 * @return 0 if successful, -1 if error
 */
int update_status_clear(void);

/**
 * @brief Request firmware update
 * @param image_size Size of firmware image in bytes
 * @param image_crc32 CRC32 of firmware image
 * @param app_version New application version
 * @return 0 if successful, -1 if error
 */
int update_status_request(uint32_t image_size, uint32_t image_crc32, uint32_t app_version);

/**
 * @brief Check if update is pending
 * @return 1 if update pending, 0 if not, -1 if error
 */
int update_status_is_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* UPDATE_MANAGER_H */

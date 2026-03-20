#ifndef BOOT_STATUS_H
#define BOOT_STATUS_H

#include "bl_app_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read boot status from external flash.
 * @return 0 on success, -1 on read error or invalid magic.
 */
int BootStatus_Read(sBootStatus *status);

/**
 * Write full boot status to external flash (erases sector first).
 * @return 0 on success, -1 on flash error.
 */
int BootStatus_Write(const sBootStatus *status);

/**
 * Ensure boot status sector contains a valid header.
 * If magic is invalid, writes a fresh default header.
 * @return 0 on success (existing or freshly written), -1 on flash error.
 */
int BootStatus_EnsureValid(void);

/**
 * Request firmware update.  Writes staged image metadata and clears
 * the fwu_requested flag (NOR-flash bit clear).
 *
 * @param image_size   Size of the staged image in ext flash.
 * @param image_crc32  CRC32 of the staged image.
 * @param staged_ver   Version of the staged image.
 * @return 0 on success.
 */
int BootStatus_RequestFwu(uint32_t image_size, uint32_t image_crc32,
                          const sFwVerArea *staged_ver);

/**
 * Confirm the running application image is healthy.
 * Clears the confirmed flag bit (NOR bit-clear, no erase).
 * @return 0 on success.
 */
int BootStatus_ConfirmApp(void);

/**
 * Consume one boot attempt (BL calls this on unconfirmed boot).
 * Clears the next available boot_attempt_N flag bit.
 * @return 0 on success.
 */
int BootStatus_ConsumeBootAttempt(void);

/**
 * Check if the running image is unconfirmed (needs APP confirmation).
 * @return true if confirmed bit is still 1 (not yet cleared by APP).
 */
bool BootStatus_IsUnconfirmed(void);

/**
 * Determine the FWU action for the bootloader based on flag state.
 * @return fwu_install, fwu_rollback, or fwu_none.
 */
eFwuAction BootStatus_GetFwuAction(void);

/**
 * Clear all flags by rewriting the boot status with fresh flags.
 * Used after a successful FWU install or rollback.
 * @return 0 on success.
 */
int BootStatus_ClearFlags(void);

/**
 * Read the AES-128 key from boot status.
 * @param key  Output buffer (16 bytes). Zeroed on error.
 * @return 0 on success, -1 on error.
 */
int BootStatus_GetAesKey(uint8_t key[AES128_KEY_SIZE]);

/**
 * Update the AES-128 key in boot status (sector erase + rewrite).
 * @param key  New 16-byte key.
 * @return 0 on success, -1 on error.
 */
int BootStatus_SetAesKey(const uint8_t key[AES128_KEY_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_STATUS_H */

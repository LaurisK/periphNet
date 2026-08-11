#ifndef BOOT_STATUS_H
#define BOOT_STATUS_H

#include "bl_app_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read boot status from external flash.
 * @return 0 on success, -1 on read error, invalid magic/version or bad CRC.
 */
int BootStatus_Read(sBootStatus *status);

/**
 * Write full boot status to external flash (erases sector first).
 * @return 0 on success, -1 on flash error.
 */
int BootStatus_Write(const sBootStatus *status);

/**
 * Ensure boot status sector contains a valid header.
 * If magic/version/CRC is invalid, writes a fresh default header.
 * @return 0 on success (existing or freshly written), -1 on flash error.
 */
int BootStatus_EnsureValid(void);

/**
 * Arm the FWU request flag (NOR bit-clear, no sector erase).
 * The staged blob in ext flash is self-describing — no metadata is
 * carried here, keeping transfer and FWU fully independent.
 * @return 0 on success.
 */
int BootStatus_RequestFwu(void);

/**
 * Confirm the running application image is healthy.
 * Clears the confirmed flag bit (NOR bit-clear, no erase).
 * Called on behalf of an outside actor (HTTP confirm endpoint) —
 * the application must never call this on its own initiative.
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
 * Check if the running image is unconfirmed (needs actor confirmation).
 * @return true if confirmed bit is still 1 (not yet cleared).
 */
bool BootStatus_IsUnconfirmed(void);

/**
 * Determine the FWU action for the bootloader based on flag state.
 * @return fwuAction_install, fwuAction_rollback, or fwuAction_none.
 */
eFwuAction BootStatus_GetFwuAction(void);

/**
 * Finish an FWU install / rollback attempt: record the result code and
 * rewrite the flags fresh (disarms fwu_requested, restores all boot
 * attempts).
 *
 * @param result         eFwuRes of the attempt (stored as last_fwu_result).
 * @param pre_confirmed  true  = mark the image confirmed immediately
 *                               (failed install keeps the old, already
 *                               confirmed app; rollback restores the
 *                               golden image which is known-good),
 *                       false = leave unconfirmed so the outside actor
 *                               must confirm within BOOT_ATTEMPTS_MAX boots.
 * @return 0 on success.
 */
int BootStatus_FinishFwu(eFwuRes result, bool pre_confirmed);

/**
 * Read current boot flags (raw word from ext flash).
 * @return 0 on success.
 */
int BootStatus_GetFlags(sBootFlags *flags);

/**
 * @return Number of unconsumed boot attempts (0..BOOT_ATTEMPTS_MAX),
 *         or BOOT_ATTEMPTS_MAX on read error.
 */
uint8_t BootStatus_AttemptsRemaining(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_STATUS_H */

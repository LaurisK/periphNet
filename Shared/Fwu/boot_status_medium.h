/**
 * @file    boot_status_medium.h
 * @brief   Where the boot status lives — the one thing boot_status.c is not
 *          allowed to know.
 *
 * `boot_status.c` compiles into BOTH targets, so it can never call nvDb: the
 * bootloader is 32 KB with no RTOS and runs before the application exists.
 * It can, however, stop caring where the bytes are.  These three calls are
 * the whole of what it needs, and each target answers them its own way:
 *
 *   - the bootloader (`bootloader/boot_status_medium.c`) writes the flash
 *     directly, at the address it was built against;
 *   - the application (`App/Fwu/boot_status_medium.c`) goes through nvDb,
 *     like everything else in the application does.
 *
 * `Program` and `Rewrite` are separate because the difference matters: the
 * flags word is updated by CLEARING BITS, which needs no erase and is
 * therefore atomic, and that is the whole reason the flags sit outside the
 * header CRC.  Collapsing the two would quietly turn every flag update into
 * an erase-and-write-back and put the most safety-critical structure on the
 * chip at risk of a power cut.
 */
#ifndef BOOT_STATUS_MEDIUM_H
#define BOOT_STATUS_MEDIUM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Read `len_bytes` from the boot status area.  0 = ok, -1 = failed. */
int BootStatusMedium_Read(uint32_t off_bytes, void *buff, uint32_t len_bytes);

/** Program bytes that only clear bits of what is already there — no erase,
 *  so it is atomic.  0 = ok, -1 = failed. */
int BootStatusMedium_Program(uint32_t off_bytes, const void *buff,
                             uint32_t len_bytes);

/** Replace the whole record, setting bits back if it has to.  0 = ok,
 *  -1 = failed. */
int BootStatusMedium_Rewrite(const void *buff, uint32_t len_bytes);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_STATUS_MEDIUM_H */

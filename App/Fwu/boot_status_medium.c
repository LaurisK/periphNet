/*
 * boot_status_medium.c — the application's half of boot_status_medium.h.
 *
 * Through nvDb, like everything else in the application.  The boot status is
 * an ordinary user with an ordinary area; that it happens to be the one thing
 * the bootloader also reads is FwuCtl_BlContractHolds()'s problem, not this
 * file's.
 *
 * Depends only on nvdb.h, so the host tests drive the same path the firmware
 * does rather than a second implementation of it.
 */

#include "boot_status_medium.h"
#include "nvdb.h"

int BootStatusMedium_Read(uint32_t off_bytes, void *buff, uint32_t len_bytes)
{
    return (NvDb_Read(nvdbUser_bootStatus, buff, off_bytes,
                      len_bytes) == nvdbRes_ok) ? 0 : -1;
}

int BootStatusMedium_Program(uint32_t off_bytes, const void *buff,
                             uint32_t len_bytes)
{
    /* Clearing bits of what is already there is precisely the write nvDb
     * programs in place, so this stays erase-free and atomic without either
     * side having to say so. */
    return (NvDb_Write(nvdbUser_bootStatus, buff, off_bytes,
                       len_bytes) == nvdbRes_ok) ? 0 : -1;
}

int BootStatusMedium_Rewrite(const void *buff, uint32_t len_bytes)
{
    return (NvDb_Write(nvdbUser_bootStatus, buff, 0u,
                       len_bytes) == nvdbRes_ok) ? 0 : -1;
}

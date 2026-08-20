/*
 * boot_status_medium.c — the bootloader's half of boot_status_medium.h.
 *
 * Direct flash, at the address this image was built against.  The BL has no
 * knowledge of nvDb by design: it cannot link it, and keeping it ignorant is
 * what leaves nvDb's directory format free to evolve without a bootloader in
 * lockstep (docs/task_nv_db.md §1.6).
 */

#include "boot_status_medium.h"
#include "bl_app_contract.h"
#include "w25q128.h"

int BootStatusMedium_Read(uint32_t off_bytes, void *buff, uint32_t len_bytes)
{
    return (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR + off_bytes,
                         (uint8_t *)buff, len_bytes) == w25q_ok) ? 0 : -1;
}

int BootStatusMedium_Program(uint32_t off_bytes, const void *buff,
                             uint32_t len_bytes)
{
    return (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR + off_bytes,
                              (const uint8_t *)buff, len_bytes) == w25q_ok)
               ? 0 : -1;
}

int BootStatusMedium_Rewrite(const void *buff, uint32_t len_bytes)
{
    if (W25Q128_EraseSector(EXT_FLASH_FWU_STATUS_ADDR) != w25q_ok) {
        return -1;
    }
    return (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR,
                              (const uint8_t *)buff, len_bytes) == w25q_ok)
               ? 0 : -1;
}

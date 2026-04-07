#include "boot_status.h"
#include "image_mgmt.h"
#include "w25q128.h"
#include <string.h>
#include <stddef.h>

/* Offset of the flags field inside sBootStatus.
 * Flags live outside the CRC so individual bits can be cleared
 * without erasing/rewriting the whole header. */
#define FLAGS_OFFSET  offsetof(sBootStatus, flags)

/* --------------------------------------------------------------------------
 * Header CRC covers magic..staged_version (everything before header_crc32).
 * -------------------------------------------------------------------------- */

static uint32_t compute_header_crc(const sBootStatus *st)
{
    return ImgMgmt_Crc32((const uint8_t *)st, FLAGS_OFFSET - sizeof(uint32_t));
}

/* --------------------------------------------------------------------------
 * Read / Write
 * -------------------------------------------------------------------------- */

int BootStatus_Read(sBootStatus *status)
{
    if (!status) {
        return -1;
    }

    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR,
                     (uint8_t *)status, sizeof(sBootStatus)) != W25Q128_OK) {
        return -1;
    }

    if (status->magic != BOOT_STATUS_MAGIC) {
        memset(status, 0, sizeof(sBootStatus));
        return -1;
    }

    return 0;
}

int BootStatus_Write(const sBootStatus *status)
{
    if (!status) {
        return -1;
    }

    if (W25Q128_EraseSector(EXT_FLASH_FWU_STATUS_ADDR) != W25Q128_OK) {
        return -1;
    }

    /* Write in 256-byte pages */
    const uint8_t *data = (const uint8_t *)status;
    uint32_t remaining = sizeof(sBootStatus);
    uint32_t offset = 0;

    while (remaining > 0) {
        uint32_t chunk = (remaining > 256u) ? 256u : remaining;
        if (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR + offset,
                              data + offset, chunk) != W25Q128_OK) {
            return -1;
        }
        offset    += chunk;
        remaining -= chunk;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * EnsureValid — create default boot status if sector is blank/corrupt
 * -------------------------------------------------------------------------- */

int BootStatus_EnsureValid(void)
{
    sBootStatus st;

    if (BootStatus_Read(&st) == 0) {
        return 0;   /* already valid */
    }

    /* Write fresh default */
    memset(&st, 0, sizeof(st));
    st.magic        = BOOT_STATUS_MAGIC;
    st.version      = 1;
    st.header_crc32 = compute_header_crc(&st);
    st.flags.word   = 0xFFFFFFFFu;          /* all flags at erased state */

    return BootStatus_Write(&st);
}

/* --------------------------------------------------------------------------
 * RequestFwu — stage an image and arm the FWU flag
 * -------------------------------------------------------------------------- */

int BootStatus_RequestFwu(uint32_t image_size, uint32_t image_crc32,
                          const sFwVerArea *staged_ver)
{
    sBootStatus st;
    memset(&st, 0, sizeof(st));

    st.magic      = BOOT_STATUS_MAGIC;
    st.version    = 1;
    st.image_size = image_size;
    st.image_crc32 = image_crc32;

    if (staged_ver) {
        memcpy(&st.staged_version, staged_ver, sizeof(sFwVerArea));
    }

    st.header_crc32 = compute_header_crc(&st);

    /* fwu_requested = 0 (cleared), rest at erased state */
    st.flags.word = 0xFFFFFFFFu;
    st.flags.bits.fwu_requested = 0;

    return BootStatus_Write(&st);
}

/* --------------------------------------------------------------------------
 * ConfirmApp — clear the confirmed bit (NOR bit-clear, no erase)
 * -------------------------------------------------------------------------- */

int BootStatus_ConfirmApp(void)
{
    sBootFlags flags;

    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR + FLAGS_OFFSET,
                     (uint8_t *)&flags, sizeof(flags)) != W25Q128_OK) {
        return -1;
    }

    flags.bits.confirmed = 0;

    if (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR + FLAGS_OFFSET,
                          (const uint8_t *)&flags, sizeof(flags)) != W25Q128_OK) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * ConsumeBootAttempt — clear next available boot_attempt_N bit
 * -------------------------------------------------------------------------- */

int BootStatus_ConsumeBootAttempt(void)
{
    sBootFlags flags;

    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR + FLAGS_OFFSET,
                     (uint8_t *)&flags, sizeof(flags)) != W25Q128_OK) {
        return -1;
    }

    /* Find first attempt bit still at 1 (available) and clear it */
    if (flags.bits.boot_attempt_0) {
        flags.bits.boot_attempt_0 = 0;
    } else if (flags.bits.boot_attempt_1) {
        flags.bits.boot_attempt_1 = 0;
    } else if (flags.bits.boot_attempt_2) {
        flags.bits.boot_attempt_2 = 0;
    } else if (flags.bits.boot_attempt_3) {
        flags.bits.boot_attempt_3 = 0;
    } else {
        return 0;   /* all attempts already consumed */
    }

    if (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR + FLAGS_OFFSET,
                          (const uint8_t *)&flags, sizeof(flags)) != W25Q128_OK) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * IsUnconfirmed — confirmed bit still at 1 means APP hasn't confirmed yet
 * -------------------------------------------------------------------------- */

bool BootStatus_IsUnconfirmed(void)
{
    sBootFlags flags;

    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR + FLAGS_OFFSET,
                     (uint8_t *)&flags, sizeof(flags)) != W25Q128_OK) {
        return false;
    }

    return flags.bits.confirmed != 0;
}

/* --------------------------------------------------------------------------
 * GetFwuAction — determine bootloader action from flag state
 * -------------------------------------------------------------------------- */

eFwuAction BootStatus_GetFwuAction(void)
{
    sBootStatus st;

    if (BootStatus_Read(&st) != 0) {
        return fwu_none;
    }

    /* FWU requested by application? */
    if (st.flags.bits.fwu_requested == 0) {
        return fwu_install;
    }

    /* All boot attempts exhausted? (all 4 bits cleared = 0) */
    if (st.flags.bits.boot_attempt_0 == 0 &&
        st.flags.bits.boot_attempt_1 == 0 &&
        st.flags.bits.boot_attempt_2 == 0 &&
        st.flags.bits.boot_attempt_3 == 0) {
        return fwu_rollback;
    }

    return fwu_none;
}

/* --------------------------------------------------------------------------
 * ClearFlags — rewrite boot status with all flags reset (erased state)
 * -------------------------------------------------------------------------- */

int BootStatus_ClearFlags(void)
{
    sBootStatus st;

    if (BootStatus_Read(&st) != 0) {
        return -1;
    }

    st.flags.word = 0xFFFFFFFFu;

    return BootStatus_Write(&st);
}

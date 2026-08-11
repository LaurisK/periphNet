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
 * Header CRC covers magic..last_fwu_result (everything before header_crc32).
 * -------------------------------------------------------------------------- */

static uint32_t compute_header_crc(const sBootStatus *st)
{
    return ImgMgmt_Crc32((const uint8_t *)st,
                         offsetof(sBootStatus, header_crc32));
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
                     (uint8_t *)status, sizeof(sBootStatus)) != w25q_ok) {
        return -1;
    }

    if (status->magic != BOOT_STATUS_MAGIC ||
        status->version != BOOT_STATUS_VERSION ||
        status->header_crc32 != compute_header_crc(status)) {
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

    if (W25Q128_EraseSector(EXT_FLASH_FWU_STATUS_ADDR) != w25q_ok) {
        return -1;
    }

    if (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR,
                          (const uint8_t *)status,
                          sizeof(sBootStatus)) != w25q_ok) {
        return -1;
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
    st.magic           = BOOT_STATUS_MAGIC;
    st.version         = BOOT_STATUS_VERSION;
    st.last_fwu_result = fwuRes_noResult;
    st.header_crc32    = compute_header_crc(&st);
    st.flags.word      = 0xFFFFFFFFu;       /* all flags at erased state */

    return BootStatus_Write(&st);
}

/* --------------------------------------------------------------------------
 * Flag helpers — NOR bit-clear on the flags word (no sector erase)
 * -------------------------------------------------------------------------- */

static int write_flags(const sBootFlags *flags)
{
    if (W25Q128_WritePage(EXT_FLASH_FWU_STATUS_ADDR + FLAGS_OFFSET,
                          (const uint8_t *)flags, sizeof(*flags)) != w25q_ok) {
        return -1;
    }
    return 0;
}

int BootStatus_GetFlags(sBootFlags *flags)
{
    if (!flags) {
        return -1;
    }

    if (W25Q128_Read(EXT_FLASH_FWU_STATUS_ADDR + FLAGS_OFFSET,
                     (uint8_t *)flags, sizeof(*flags)) != w25q_ok) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * RequestFwu — arm the FWU flag (bit-clear only, header untouched)
 * -------------------------------------------------------------------------- */

int BootStatus_RequestFwu(void)
{
    if (BootStatus_EnsureValid() != 0) {
        return -1;
    }

    sBootFlags flags;
    if (BootStatus_GetFlags(&flags) != 0) {
        return -1;
    }

    flags.bits.fwu_requested = 0;
    return write_flags(&flags);
}

/* --------------------------------------------------------------------------
 * ConfirmApp — clear the confirmed bit (NOR bit-clear, no erase)
 * -------------------------------------------------------------------------- */

int BootStatus_ConfirmApp(void)
{
    sBootFlags flags;

    if (BootStatus_GetFlags(&flags) != 0) {
        return -1;
    }

    flags.bits.confirmed = 0;
    return write_flags(&flags);
}

/* --------------------------------------------------------------------------
 * ConsumeBootAttempt — clear next available boot_attempt_N bit
 * -------------------------------------------------------------------------- */

int BootStatus_ConsumeBootAttempt(void)
{
    sBootFlags flags;

    if (BootStatus_GetFlags(&flags) != 0) {
        return -1;
    }

    /* Find first attempt bit still at 1 (available) and clear it */
    if (flags.bits.boot_attempt_0) {
        flags.bits.boot_attempt_0 = 0;
    } else if (flags.bits.boot_attempt_1) {
        flags.bits.boot_attempt_1 = 0;
    } else if (flags.bits.boot_attempt_2) {
        flags.bits.boot_attempt_2 = 0;
    } else {
        return 0;   /* all attempts already consumed */
    }

    return write_flags(&flags);
}

/* --------------------------------------------------------------------------
 * IsUnconfirmed — confirmed bit still at 1 means nobody confirmed yet
 * -------------------------------------------------------------------------- */

bool BootStatus_IsUnconfirmed(void)
{
    sBootFlags flags;

    if (BootStatus_GetFlags(&flags) != 0) {
        return false;
    }

    return flags.bits.confirmed != 0;
}

uint8_t BootStatus_AttemptsRemaining(void)
{
    sBootFlags flags;

    if (BootStatus_GetFlags(&flags) != 0) {
        return BOOT_ATTEMPTS_MAX;
    }

    uint8_t left = 0;
    if (flags.bits.boot_attempt_0) left++;
    if (flags.bits.boot_attempt_1) left++;
    if (flags.bits.boot_attempt_2) left++;
    return left;
}

/* --------------------------------------------------------------------------
 * GetFwuAction — determine bootloader action from flag state
 * -------------------------------------------------------------------------- */

eFwuAction BootStatus_GetFwuAction(void)
{
    sBootStatus st;

    if (BootStatus_Read(&st) != 0) {
        return fwuAction_none;
    }

    /* FWU requested by application? */
    if (st.flags.bits.fwu_requested == 0) {
        return fwuAction_install;
    }

    /* Unconfirmed with all boot attempts exhausted? */
    if (st.flags.bits.confirmed != 0 &&
        st.flags.bits.boot_attempt_0 == 0 &&
        st.flags.bits.boot_attempt_1 == 0 &&
        st.flags.bits.boot_attempt_2 == 0) {
        return fwuAction_rollback;
    }

    return fwuAction_none;
}

/* --------------------------------------------------------------------------
 * FinishFwu — record result and rewrite flags fresh
 * -------------------------------------------------------------------------- */

int BootStatus_FinishFwu(eFwuRes result, bool pre_confirmed)
{
    sBootStatus st;

    memset(&st, 0, sizeof(st));
    st.magic           = BOOT_STATUS_MAGIC;
    st.version         = BOOT_STATUS_VERSION;
    st.last_fwu_result = (uint32_t)result;
    st.header_crc32    = compute_header_crc(&st);

    st.flags.word = 0xFFFFFFFFu;
    if (pre_confirmed) {
        st.flags.bits.confirmed = 0;
    }

    return BootStatus_Write(&st);
}

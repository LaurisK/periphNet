/*
 * FWU process control — install / confirm / golden promotion / verify.
 *
 * The stored .pnfw blob is owned by the image store; this module only
 * arms boot-status flags for the bootloader (which does all cryptography
 * during install) and maintains the golden rollback image.  A confirm
 * takes a read hold on the store for the whole pending+running promotion
 * window so a new upload cannot corrupt the copy.
 *
 * Runs in the HTTP server task (install/confirm/verify) and defaultTask
 * (promotion, reboot); the W25Q128 driver serializes SPI access.
 */

#include "App/Fwu/fwu_control.h"
#include "App/system.h"
#include "main.h"
#include "nvdb.h"
#include "nvdb_exceptions.h"
#include "boot_status.h"
#include "bl_app_contract.h"
#include <string.h>

#define PROMOTE_CHUNK_SIZE   4096U   /* one ext-flash sector */

static sBlobInfo golden;
static bool      bl_contract_ok;

static volatile bool reboot_pending  = false;
static volatile bool promote_pending = false;
static volatile bool promoting       = false;

/* The confirmation deadline (fwu_control.h).  All of it is RAM: each boot
 * gets a fresh window, and each expiry spends one of the bootloader's three
 * attempts, so the state that has to survive a reset already lives in the
 * boot status. */
static volatile bool     guard_armed       = false;
static volatile bool     guard_exempt      = false;
static volatile uint32_t guard_window_sec  = FWU_CONFIRM_WINDOW_DEFAULT_SEC;
static volatile uint32_t guard_deadline_ms = 0U;
static volatile uint32_t guard_kickCnt     = 0U;

/**
 * @brief Does nvDb still put the BL-visible areas where the BL looks?
 * @param  user - the nvDb user holding the area
 * @param  addr - the address the bootloader was built against
 * @param  size - the size it was built against
 * @retval true if they agree
 */
static bool bl_area_agrees(eNvDbUser user, uint32_t addr, uint32_t size)
{
    uint32_t a = 0U;
    uint32_t n = 0U;

    if (NvDb_GetAbsoluteAddress(user, &a, &n) != nvdbRes_ok) {
        return false;
    }
    return (a == addr) && (n >= size);
}

/** @return true if the RUNNING image is a local ('l') build. */
static bool running_is_local(void)
{
    const sAppInfo *app = (const sAppInfo *)APP_INFO_HEADER_ADDR;

    return (app->magic == APP_INFO_MAGIC) &&
           (app->fw_version.ver.target == (uint8_t)fwTarget_local);
}

/** Restart the countdown.  window_sec is already clamped. */
static void guard_reload(uint32_t window_sec)
{
    guard_window_sec  = window_sec;
    guard_deadline_ms = HAL_GetTick() + (window_sec * 1000U);
}

void FwuCtl_Init(void)
{
    ImgStore_ScanArea(nvdbUser_fwuGolden, &golden);

    /* Arm the confirmation deadline HERE rather than at the first HTTP
     * request: the failure it exists for is an image whose network never
     * comes up, so nothing on the network may be a precondition for it.
     *
     * THREE conditions, and the third is not obvious.  Without a golden
     * image there is nothing to roll back TO, so a self-reboot would spend
     * the bootloader's attempts to reach a rollback that must fail -- and on
     * a virgin board the erased boot-status flags read as "unconfirmed", so
     * that is not a hypothetical: it is what a factory J-Link write looks
     * like.  No target, no countdown. */
    guard_exempt = running_is_local();
    if (!guard_exempt && golden.valid && BootStatus_IsUnconfirmed()) {
        guard_armed   = true;
        guard_kickCnt = 0U;
        guard_reload(FWU_CONFIRM_WINDOW_DEFAULT_SEC);
    }

    bl_contract_ok =
        bl_area_agrees(nvdbUser_bootStatus, EXT_FLASH_FWU_STATUS_ADDR,
                       EXT_FLASH_FWU_STATUS_SIZE) &&
        bl_area_agrees(nvdbUser_fwuStored, EXT_FLASH_FWU_IMG_ADDR,
                       EXT_FLASH_FWU_IMG_SIZE) &&
        bl_area_agrees(nvdbUser_fwuGolden, EXT_FLASH_GOLDEN_IMG_ADDR,
                       EXT_FLASH_GOLDEN_IMG_SIZE);
}

bool FwuCtl_BlContractHolds(void)
{
    return bl_contract_ok;
}

const sBlobInfo *FwuCtl_GetGolden(void)
{
    return &golden;
}

/* --------------------------------------------------------------------------
 * Install / confirm / verify
 * -------------------------------------------------------------------------- */

eFwuCtlRes FwuCtl_RequestInstall(void)
{
    const sImageStoreState *st = ImgStore_GetState();

    if (!st->blob.valid || st->status != imgStore_ready) {
        return fwuCtlRes_noImage;
    }
    if (promote_pending || promoting) {
        return fwuCtlRes_busy;
    }
    if (!bl_contract_ok) {
        return fwuCtlRes_blContract;
    }
    if (BootStatus_RequestFwu() != 0) {
        return fwuCtlRes_flashErr;
    }

    reboot_pending = true;
    return fwuCtlRes_ok;
}

eFwuCtlRes FwuCtl_Confirm(bool *promote)
{
    *promote = false;

    if (!BootStatus_IsUnconfirmed()) {
        return fwuCtlRes_already;
    }
    if (BootStatus_ConfirmApp() != 0) {
        return fwuCtlRes_flashErr;
    }
    guard_armed = false;              /* confirmed: nothing left to count */

    /* Promote stored → golden only if the stored blob is what is actually
     * running (a newer, not-yet-installed upload must not become golden).
     * The read hold spans the pending+running window so the blob cannot
     * be replaced or deleted before the copy completes. */
    const sImageStoreState *st = ImgStore_GetState();
    const sAppInfo *app = (const sAppInfo *)APP_INFO_HEADER_ADDR;
    if (st->blob.valid &&
        app->magic == APP_INFO_MAGIC &&
        memcmp(&st->blob.version.ver, &app->fw_version.ver,
               sizeof(sFwVer)) == 0 &&
        (!golden.valid || golden.blob_crc32 != st->blob.blob_crc32) &&
        ImgStore_AcquireRead()) {
        *promote = true;
        promote_pending = true;
    }

    return fwuCtlRes_ok;
}

eFwuRes FwuCtl_VerifyRunning(void)
{
    const sAppInfo *app = (const sAppInfo *)APP_INFO_HEADER_ADDR;
    const sBootloaderApi *bl = (const sBootloaderApi *)BL_API_TABLE_ADDR;

    if (app->magic != APP_INFO_MAGIC) {
        return fwuRes_errWrongMagic;
    }
    if (app->image_size == 0xFFFFFFFFu) {
        return fwuRes_errImageSize;      /* unsigned dev image */
    }
    if (bl->magic != BL_API_MAGIC || bl->version < 3 ||
        bl->verify_image_hmac == NULL) {
        return fwuRes_errNoImage;        /* BL API unavailable */
    }

    return bl->verify_image_hmac(APPLICATION_START_ADDR, false,
                                 app->image_size, app->image_hmac);
}

/* --------------------------------------------------------------------------
 * The confirmation deadline
 * -------------------------------------------------------------------------- */

eFwuCtlRes FwuCtl_KickConfirm(uint32_t window_sec)
{
    if (!guard_armed) {
        return fwuCtlRes_notArmed;
    }
    if (window_sec == 0U) {
        window_sec = guard_window_sec;      /* keep the window in force */
    }
    if (window_sec < FWU_CONFIRM_WINDOW_MIN_SEC) {
        window_sec = FWU_CONFIRM_WINDOW_MIN_SEC;
    }
    if (window_sec > FWU_CONFIRM_WINDOW_MAX_SEC) {
        window_sec = FWU_CONFIRM_WINDOW_MAX_SEC;
    }
    guard_reload(window_sec);
    guard_kickCnt++;

    return fwuCtlRes_ok;
}

bool FwuCtl_ConfirmDeadlineDue(void)
{
    if (!guard_armed) {
        return false;
    }
    /* Signed difference: the deadline may sit the far side of a tick wrap. */
    if ((int32_t)(HAL_GetTick() - guard_deadline_ms) < 0) {
        return false;
    }

    /* Only now, once, is the flash flag worth reading: something may have
     * confirmed by a route this module did not see. */
    if (!BootStatus_IsUnconfirmed()) {
        guard_armed = false;
        return false;
    }

    /* THE ONE IMPLICIT REPRIEVE, and it is bounded: an upload in flight is
     * the operator pushing a fix, and the HTTP server's own recv timeout
     * ends a stalled one -- so this cannot be held open by the subsystem
     * under test the way a general "traffic counts" rule could. */
    if (ImgStore_GetState()->status == imgStore_uploading) {
        guard_reload(guard_window_sec);
        return false;
    }

    return true;
}

void FwuCtl_GetConfirmGuard(sFwuConfirmGuard *out)
{
    uint32_t left = 0U;

    if (guard_armed) {
        const int32_t d = (int32_t)(guard_deadline_ms - HAL_GetTick());

        left = (d > 0) ? ((uint32_t)d / 1000U) : 0U;
    }
    out->window_sec    = guard_window_sec;
    out->remaining_sec = left;
    out->kickCnt       = guard_kickCnt;
    out->attemptsLeft  = BootStatus_AttemptsRemaining();
    out->armed         = guard_armed;
    out->exempt        = guard_exempt;
}

/* --------------------------------------------------------------------------
 * defaultTask-side jobs: reboot + golden promotion
 * -------------------------------------------------------------------------- */

bool FwuCtl_RebootPending(void)
{
    return reboot_pending;
}

bool FwuCtl_PromotePending(void)
{
    return promote_pending;
}

void FwuCtl_RunPromotion(void)
{
    if (!promote_pending) {
        return;
    }

    promoting = true;
    promote_pending = false;

    const sImageStoreState *st = ImgStore_GetState();
    uint32_t total = st->blob.blob_size;
    /* STATIC, so it lands in .bss and the DMA controller can see it.
     * A local would sit on defaultTask's stack, and task stacks are
     * pvPortMalloc'd out of the CCM heap — invisible to DMA, which would
     * quietly put this 488 KB copy back on the polled path.  Safe as a
     * single instance: FwuCtl_RunPromotion has one caller (defaultTask)
     * and the promote_pending/promoting flags serialize it.
     * Source ≠ destination, so a page bounce suffices. */
    static uint8_t buf[256];

    if (!st->blob.valid || total == 0) {
        ImgStore_ReleaseRead();
        promoting = false;
        return;
    }

    /* The golden area still holds the previous image, so declare it unwanted
     * before overwriting it: a write into occupied space drags an erase onto
     * the write path, and there are 488 KB of them here. */
    (void)NvDb_Wipe(nvdbUser_fwuGolden, NULL);

    bool ok = true;
    for (uint32_t off = 0; off < total && ok; off += PROMOTE_CHUNK_SIZE) {
        KickIwdg();

        uint32_t n = total - off;
        if (n > PROMOTE_CHUNK_SIZE) n = PROMOTE_CHUNK_SIZE;

        for (uint32_t page = 0; page < n; page += sizeof(buf)) {
            uint32_t plen = n - page;
            if (plen > sizeof(buf)) plen = sizeof(buf);
            if (!ImgStore_Read(off + page, buf, plen) ||
                NvDb_Write(nvdbUser_fwuGolden, buf, off + page,
                           plen) != nvdbRes_ok) {
                ok = false;
                break;
            }
        }
    }

    /* Re-scan golden: validates the copy (manifest + CRC32) */
    ImgStore_ScanArea(nvdbUser_fwuGolden, &golden);

    (void)ok;
    ImgStore_ReleaseRead();
    promoting = false;
}

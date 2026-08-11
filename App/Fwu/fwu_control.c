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
#include "w25q128.h"
#include "boot_status.h"
#include "bl_app_contract.h"
#include <string.h>

#define PROMOTE_CHUNK_SIZE   4096U   /* one ext-flash sector */

static sBlobInfo golden;

static volatile bool reboot_pending  = false;
static volatile bool promote_pending = false;
static volatile bool promoting       = false;

void FwuCtl_Init(void)
{
    ImgStore_ScanArea(EXT_FLASH_GOLDEN_IMG_ADDR, EXT_FLASH_GOLDEN_IMG_SIZE,
                      &golden);
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
    uint8_t  buf[256];   /* source ≠ destination, so a page bounce suffices */

    if (!st->blob.valid || total == 0) {
        ImgStore_ReleaseRead();
        promoting = false;
        return;
    }

    bool ok = true;
    for (uint32_t off = 0; off < total && ok; off += PROMOTE_CHUNK_SIZE) {
        KickIwdg();

        uint32_t n = total - off;
        if (n > PROMOTE_CHUNK_SIZE) n = PROMOTE_CHUNK_SIZE;

        if (W25Q128_EraseSector(EXT_FLASH_GOLDEN_IMG_ADDR + off) != w25q_ok) {
            ok = false;
            break;
        }

        for (uint32_t page = 0; page < n; page += sizeof(buf)) {
            uint32_t plen = n - page;
            if (plen > sizeof(buf)) plen = sizeof(buf);
            if (!ImgStore_Read(off + page, buf, plen) ||
                W25Q128_WritePage(EXT_FLASH_GOLDEN_IMG_ADDR + off + page,
                                  buf, plen) != w25q_ok) {
                ok = false;
                break;
            }
        }
    }

    /* Re-scan golden: validates the copy (manifest + CRC32) */
    ImgStore_ScanArea(EXT_FLASH_GOLDEN_IMG_ADDR, EXT_FLASH_GOLDEN_IMG_SIZE,
                      &golden);

    (void)ok;
    ImgStore_ReleaseRead();
    promoting = false;
}

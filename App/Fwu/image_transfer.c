/*
 * FWU staging + control logic (protocol-agnostic).
 *
 * The application only ever handles opaque encrypted .pnfw blobs:
 * it stores them, checks the keyless manifest + CRC32, and arms the FWU
 * flag.  All cryptography (GCM tag, HMAC, version gating) happens in the
 * bootloader during install.  Transfer and FWU are independent: a staged
 * blob survives reboots and can be installed at any time.
 *
 * Runs in the HTTP server task (upload/download/control) and defaultTask
 * (promotion, reboot); the W25Q128 driver serializes SPI access.
 */

#include "App/Fwu/image_transfer.h"
#include "App/system.h"
#include "w25q128.h"
#include "boot_status.h"
#include "image_mgmt.h"
#include "version.h"
#include "bl_app_contract.h"
#include <string.h>
#include <stdio.h>

#define SCAN_CHUNK_SIZE      256U
#define PROMOTE_CHUNK_SIZE   4096U   /* one ext-flash sector */

static image_state_t fw_state;

static volatile bool reboot_pending  = false;
static volatile bool promote_pending = false;
static volatile bool promoting       = false;

/* --------------------------------------------------------------------------
 * Blob area scanning — manifest sanity + whole-blob CRC32 (keyless)
 * -------------------------------------------------------------------------- */

static void scan_blob_area(uint32_t base, uint32_t area_size, sBlobInfo *out)
{
    memset(out, 0, sizeof(*out));

    sFwuManifest man;
    if (W25Q128_Read(base, (uint8_t *)&man, sizeof(man)) != W25Q128_OK) {
        return;
    }

    if (man.magic != FWU_BLOB_MAGIC || man.format != FWU_BLOB_FORMAT) {
        return;
    }
    if (man.image_size < FW_OFFSET_APP_HEADER + sizeof(sAppInfo) ||
        man.image_size > APPLICATION_SIZE ||
        man.blob_size != man.image_size + FWU_BLOB_OVERHEAD ||
        man.blob_size > area_size) {
        return;
    }

    uint8_t  buf[SCAN_CHUNK_SIZE];
    uint32_t body_len = man.blob_size - FWU_BLOB_CRC_SIZE;
    uint32_t crc = ImgMgmt_Crc32Init();

    for (uint32_t off = 0; off < body_len; off += SCAN_CHUNK_SIZE) {
        uint32_t n = body_len - off;
        if (n > SCAN_CHUNK_SIZE) n = SCAN_CHUNK_SIZE;

        if ((off & 0xFFFFU) == 0U) {
            KickIwdg();
        }
        if (W25Q128_Read(base + off, buf, n) != W25Q128_OK) {
            return;
        }
        crc = ImgMgmt_Crc32Update(crc, buf, n);
    }

    uint32_t stored;
    if (W25Q128_Read(base + body_len, (uint8_t *)&stored,
                     sizeof(stored)) != W25Q128_OK) {
        return;
    }
    if (ImgMgmt_Crc32Final(crc) != stored) {
        return;
    }

    out->valid      = true;
    out->version    = man.fw_version;
    out->image_size = man.image_size;
    out->blob_size  = man.blob_size;
    out->blob_crc32 = stored;
    ver_toString(&man.fw_version.ver, out->version_str,
                 sizeof(out->version_str));
}

/* --------------------------------------------------------------------------
 * Init / status
 * -------------------------------------------------------------------------- */

void image_transfer_init(void)
{
    memset(&fw_state, 0, sizeof(fw_state));

    /* Blob areas are self-describing: rescan so a staged image uploaded
     * before a reboot is still installable. */
    scan_blob_area(EXT_FLASH_FWU_IMG_ADDR, EXT_FLASH_FWU_IMG_SIZE,
                   &fw_state.staged);
    scan_blob_area(EXT_FLASH_GOLDEN_IMG_ADDR, EXT_FLASH_GOLDEN_IMG_SIZE,
                   &fw_state.golden);

    fw_state.status = fw_state.staged.valid ? IMG_STATUS_STAGED
                                            : IMG_STATUS_IDLE;
}

const image_state_t *image_transfer_get_status(void)
{
    return &fw_state;
}

/* --------------------------------------------------------------------------
 * Upload session — page-buffered synchronous flash writes with lazy
 * sector erase.  Single session; the HTTP task is the only caller.
 * -------------------------------------------------------------------------- */

typedef struct {
    bool     active;
    uint32_t content_length;
    uint32_t bytes_received;
    uint32_t flash_write_addr;
    uint32_t current_sector;
    uint16_t buffer_pos;
    uint8_t  buffer[256];
} sUploadSession;

static sUploadSession upload;

static bool flush_upload_page(void)
{
    if (upload.buffer_pos == 0) {
        return true;
    }

    uint32_t addr = upload.flash_write_addr;
    uint32_t sector = addr & ~0xFFFU;

    /* Lazy sector erase before first write to each 4KB sector */
    if (sector != upload.current_sector) {
        KickIwdg();
        if (W25Q128_EraseSector(sector) != W25Q128_OK) {
            return false;
        }
        upload.current_sector = sector;
    }

    if (W25Q128_WritePage(addr, upload.buffer, upload.buffer_pos) != W25Q128_OK) {
        return false;
    }

    upload.flash_write_addr += upload.buffer_pos;
    upload.buffer_pos = 0;
    return true;
}

bool img_upload_begin(uint32_t content_length, const char **err)
{
    if (promote_pending || promoting) {
        *err = "golden promotion in progress";
        return false;
    }
    if (upload.active) {
        *err = "upload already in progress";
        return false;
    }
    if (content_length < FWU_BLOB_OVERHEAD ||
        content_length > EXT_FLASH_FWU_IMG_SIZE) {
        *err = "invalid content-length";
        return false;
    }

    memset(&upload, 0, sizeof(upload));
    upload.active           = true;
    upload.content_length   = content_length;
    upload.flash_write_addr = EXT_FLASH_FWU_IMG_ADDR;
    upload.current_sector   = 0xFFFFFFFFU;

    /* Incoming upload invalidates whatever was staged */
    fw_state.staged.valid      = false;
    fw_state.status            = IMG_STATUS_UPLOADING;
    fw_state.bytes_transferred = 0;
    fw_state.total_bytes       = content_length;
    memset(fw_state.error_message, 0, sizeof(fw_state.error_message));

    return true;
}

bool img_upload_write(const uint8_t *data, uint32_t len)
{
    if (!upload.active) {
        return false;
    }

    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        uint32_t space = sizeof(upload.buffer) - upload.buffer_pos;
        if (chunk > space) chunk = space;

        memcpy(&upload.buffer[upload.buffer_pos], &data[offset], chunk);
        upload.buffer_pos += (uint16_t)chunk;
        offset += chunk;
        upload.bytes_received += chunk;

        if (upload.buffer_pos >= sizeof(upload.buffer)) {
            if (!flush_upload_page()) {
                img_upload_abort("Flash write error");
                return false;
            }
        }
    }

    fw_state.bytes_transferred = upload.bytes_received;
    return true;
}

bool img_upload_finish(void)
{
    if (!upload.active) {
        return false;
    }

    bool flushed = flush_upload_page();
    upload.active = false;

    if (!flushed) {
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Final flash write failed");
        return false;
    }

    /* Validate the received blob: manifest + CRC32 (keyless) */
    scan_blob_area(EXT_FLASH_FWU_IMG_ADDR, EXT_FLASH_FWU_IMG_SIZE,
                   &fw_state.staged);

    if (fw_state.staged.valid) {
        fw_state.status = IMG_STATUS_STAGED;
        return true;
    }

    fw_state.status = IMG_STATUS_ERROR;
    strcpy(fw_state.error_message, "Invalid blob (manifest/CRC)");
    return false;
}

void img_upload_abort(const char *reason)
{
    if (upload.active) {
        upload.active = false;
        fw_state.status = IMG_STATUS_ERROR;
        snprintf(fw_state.error_message, sizeof(fw_state.error_message),
                 "%s", reason ? reason : "Upload aborted");
    }
}

/* --------------------------------------------------------------------------
 * Staged blob access (download)
 * -------------------------------------------------------------------------- */

void img_download_begin(void)
{
    fw_state.status            = IMG_STATUS_DOWNLOADING;
    fw_state.bytes_transferred = 0;
    fw_state.total_bytes       = fw_state.staged.blob_size;
}

void img_download_end(bool ok, const char *err)
{
    if (ok) {
        fw_state.status = IMG_STATUS_STAGED;
    } else {
        fw_state.status = IMG_STATUS_ERROR;
        snprintf(fw_state.error_message, sizeof(fw_state.error_message),
                 "%s", err ? err : "Download failed");
    }
}

bool img_read_staged(uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (W25Q128_Read(EXT_FLASH_FWU_IMG_ADDR + offset, buf, len) != W25Q128_OK) {
        return false;
    }
    fw_state.bytes_transferred = offset + len;
    return true;
}

/* --------------------------------------------------------------------------
 * FWU control — install / confirm / delete / verify
 * -------------------------------------------------------------------------- */

eImgCtlRes img_install_request(void)
{
    if (!fw_state.staged.valid || fw_state.status != IMG_STATUS_STAGED) {
        return IMG_CTL_NO_IMAGE;
    }
    if (promote_pending || promoting) {
        return IMG_CTL_BUSY;
    }
    if (BootStatus_RequestFwu() != 0) {
        return IMG_CTL_FLASH_ERR;
    }

    reboot_pending = true;
    return IMG_CTL_OK;
}

eImgCtlRes img_confirm(bool *promote)
{
    *promote = false;

    if (!BootStatus_IsUnconfirmed()) {
        return IMG_CTL_ALREADY;
    }
    if (BootStatus_ConfirmApp() != 0) {
        return IMG_CTL_FLASH_ERR;
    }

    /* Promote staged → golden only if the staged blob is what is actually
     * running (a newer, not-yet-installed upload must not become golden). */
    const sAppInfo *app = (const sAppInfo *)APP_INFO_HEADER_ADDR;
    if (fw_state.staged.valid &&
        app->magic == APP_INFO_MAGIC &&
        memcmp(&fw_state.staged.version.ver, &app->fw_version.ver,
               sizeof(sFwVer)) == 0 &&
        (!fw_state.golden.valid ||
         fw_state.golden.blob_crc32 != fw_state.staged.blob_crc32)) {
        *promote = true;
        promote_pending = true;
    }

    return IMG_CTL_OK;
}

eImgCtlRes img_delete(void)
{
    if (fw_state.status == IMG_STATUS_UPLOADING ||
        fw_state.status == IMG_STATUS_DOWNLOADING ||
        promote_pending || promoting) {
        return IMG_CTL_BUSY;
    }

    /* Erase first sector of staged area — kills the manifest */
    if (W25Q128_EraseSector(EXT_FLASH_FWU_IMG_ADDR) != W25Q128_OK) {
        return IMG_CTL_FLASH_ERR;
    }

    memset(&fw_state.staged, 0, sizeof(fw_state.staged));
    fw_state.status            = IMG_STATUS_IDLE;
    fw_state.bytes_transferred = 0;
    fw_state.total_bytes       = 0;
    memset(fw_state.error_message, 0, sizeof(fw_state.error_message));

    return IMG_CTL_OK;
}

eFwuRes img_verify_running(void)
{
    const sAppInfo *app = (const sAppInfo *)APP_INFO_HEADER_ADDR;
    const sBootloaderApi *bl = (const sBootloaderApi *)BL_API_TABLE_ADDR;

    if (app->magic != APP_INFO_MAGIC) {
        return FWU_ERR_WRONG_MAGIC;
    }
    if (app->image_size == 0xFFFFFFFFu) {
        return FWU_ERR_IMAGE_SIZE;      /* unsigned dev image */
    }
    if (bl->magic != BL_API_MAGIC || bl->version < 3 ||
        bl->verify_image_hmac == NULL) {
        return FWU_ERR_NO_IMAGE;        /* BL API unavailable */
    }

    return bl->verify_image_hmac(APPLICATION_START_ADDR, false,
                                 app->image_size, app->image_hmac);
}

/* --------------------------------------------------------------------------
 * defaultTask-side jobs: reboot + golden promotion
 * -------------------------------------------------------------------------- */

bool image_transfer_reboot_pending(void)
{
    return reboot_pending;
}

bool image_transfer_promote_pending(void)
{
    return promote_pending;
}

void image_transfer_run_promotion(void)
{
    if (!promote_pending) {
        return;
    }

    promoting = true;
    promote_pending = false;

    uint32_t total = fw_state.staged.blob_size;
    uint8_t  buf[256];   /* source ≠ destination, so a page bounce suffices */

    if (!fw_state.staged.valid || total == 0) {
        promoting = false;
        return;
    }

    bool ok = true;
    for (uint32_t off = 0; off < total && ok; off += PROMOTE_CHUNK_SIZE) {
        KickIwdg();

        uint32_t n = total - off;
        if (n > PROMOTE_CHUNK_SIZE) n = PROMOTE_CHUNK_SIZE;

        if (W25Q128_EraseSector(EXT_FLASH_GOLDEN_IMG_ADDR + off) != W25Q128_OK) {
            ok = false;
            break;
        }

        for (uint32_t page = 0; page < n; page += sizeof(buf)) {
            uint32_t plen = n - page;
            if (plen > sizeof(buf)) plen = sizeof(buf);
            if (W25Q128_Read(EXT_FLASH_FWU_IMG_ADDR + off + page,
                             buf, plen) != W25Q128_OK ||
                W25Q128_WritePage(EXT_FLASH_GOLDEN_IMG_ADDR + off + page,
                                  buf, plen) != W25Q128_OK) {
                ok = false;
                break;
            }
        }
    }

    /* Re-scan golden: validates the copy (manifest + CRC32) */
    scan_blob_area(EXT_FLASH_GOLDEN_IMG_ADDR, EXT_FLASH_GOLDEN_IMG_SIZE,
                   &fw_state.golden);

    (void)ok;
    promoting = false;
}

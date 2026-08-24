/*
 * Image store — persistent management of one uploaded .pnfw blob.
 *
 * The application only ever handles opaque encrypted blobs: it stores
 * them, checks the keyless manifest + CRC32, and remembers the original
 * file name in a small metadata record (own ext-flash sector, bound to
 * the blob by its CRC32 so stale names never survive a new upload).
 *
 * No FWU knowledge lives here — install/confirm/golden logic is in
 * App/Fwu/fwu_control.c and consumes this module through its public API.
 *
 * Runs in the HTTP server task (upload/download/delete); FWU promotion
 * reads from defaultTask under a read hold.  Every byte goes through nvDb,
 * which serializes access and decides where the areas live — this module
 * names users (nvdbUser_fwuStored, fwuGolden, imageMeta), never addresses.
 */

#include "App/Img/image_store.h"
#include "App/system.h"
#include "image_mgmt.h"
#include "version.h"
#include "bl_app_contract.h"
#include "nvdb.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define SCAN_CHUNK_SIZE  256U

#define IMG_META_MAGIC   0x4D474D49u   /* "IMGM" */

/* Metadata record in its own nvDb area — app-side extras that do not fit the
 * fixed .pnfw manifest.  blob_crc32 binds it to the blob. */
typedef struct {
    uint32_t magic;
    uint32_t blob_crc32;
    char     name[IMG_STORE_NAME_MAX];
    uint32_t crc;                       /* CRC32 over preceding bytes */
} sImageMeta;

static sImageStoreState store_state;
static volatile bool    read_held;

/* --------------------------------------------------------------------------
 * Blob area scanning — manifest sanity + whole-blob CRC32 (keyless)
 * -------------------------------------------------------------------------- */

void ImgStore_ScanArea(eNvDbUser area, sBlobInfo *out)
{
    uint32_t area_size = 0U;

    memset(out, 0, sizeof(*out));

    if (NvDb_GetSize(area, &area_size) != nvdbRes_ok) {
        return;
    }

    sFwuManifest man;
    if (NvDb_Read(area, &man, 0U, sizeof(man)) != nvdbRes_ok) {
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
        if (NvDb_Read(area, buf, off, n) != nvdbRes_ok) {
            return;
        }
        crc = ImgMgmt_Crc32Update(crc, buf, n);
    }

    uint32_t stored;
    if (NvDb_Read(area, &stored, body_len, sizeof(stored)) != nvdbRes_ok) {
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
 * Metadata record (file name), bound to the blob by CRC32
 * -------------------------------------------------------------------------- */

static void meta_load(void)
{
    memset(store_state.name, 0, sizeof(store_state.name));

    if (!store_state.blob.valid) {
        return;
    }

    sImageMeta meta;
    if (NvDb_Read(nvdbUser_imageMeta, &meta, 0U, sizeof(meta)) != nvdbRes_ok) {
        return;
    }
    if (meta.magic != IMG_META_MAGIC ||
        meta.crc != ImgMgmt_Crc32((const uint8_t *)&meta,
                                  offsetof(sImageMeta, crc)) ||
        meta.blob_crc32 != store_state.blob.blob_crc32) {
        return;
    }

    meta.name[sizeof(meta.name) - 1] = '\0';
    strcpy(store_state.name, meta.name);
}

static void meta_store(const char *name)
{
    sImageMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic      = IMG_META_MAGIC;
    meta.blob_crc32 = store_state.blob.blob_crc32;
    snprintf(meta.name, sizeof(meta.name), "%s", name ? name : "");
    meta.crc = ImgMgmt_Crc32((const uint8_t *)&meta,
                             offsetof(sImageMeta, crc));

    /* Name is best-effort; the blob is intact either way. */
    (void)NvDb_Write(nvdbUser_imageMeta, &meta, 0U, sizeof(meta));
}

/* --------------------------------------------------------------------------
 * Init / state
 * -------------------------------------------------------------------------- */

void ImgStore_Init(void)
{
    memset(&store_state, 0, sizeof(store_state));

    /* The blob area is self-describing: rescan so an image uploaded
     * before a reboot is still available. */
    ImgStore_ScanArea(nvdbUser_fwuStored, &store_state.blob);
    meta_load();

    store_state.status = store_state.blob.valid ? imgStore_ready
                                                : imgStore_empty;
}

const sImageStoreState *ImgStore_GetState(void)
{
    return &store_state;
}

/* --------------------------------------------------------------------------
 * Upload session — page-buffered synchronous flash writes with lazy
 * sector erase.  Single session; the HTTP task is the only caller.
 * -------------------------------------------------------------------------- */

typedef struct {
    bool     active;
    uint32_t content_length;
    uint32_t bytes_received;
    uint32_t write_off;
    uint16_t buffer_pos;
    uint8_t  buffer[256];
    char     name[IMG_STORE_NAME_MAX];
} sUploadSession;

/* PLAIN .bss, NOT .ccmram, and that placement is now load-bearing.
 *
 * This used to live in CCM to spare main SRAM, on the stated grounds that the
 * W25Q128 driver was polling anyway.  That driver takes DMA now, and DMA
 * cannot address CCM on this part — a buffer left there would silently drop
 * back to the polled path on the single hottest flash writer on the board.
 * ~300 bytes of main SRAM (of which ~39 KB is free) buys DMA on every OTA
 * page program, and hands CCM (~92 % full) those bytes back. */
static sUploadSession upload;

static bool flush_upload_page(void)
{
    if (upload.buffer_pos == 0) {
        return true;
    }

    KickIwdg();

    /* The area was declared unwanted at UploadBegin, so these writes land in
     * space the collector is already erasing — or pull that unit's erase
     * forward themselves.  Either way the lazy sector erase this used to do
     * by hand is now nvDb's business, and so is the page boundary. */
    if (NvDb_Write(nvdbUser_fwuStored, upload.buffer, upload.write_off,
                   upload.buffer_pos) != nvdbRes_ok) {
        return false;
    }

    upload.write_off += upload.buffer_pos;
    upload.buffer_pos = 0;
    return true;
}

bool ImgStore_UploadBegin(uint32_t content_length, const char *name,
                          const char **err)
{
    if (read_held) {
        *err = "image in use (FWU)";
        return false;
    }
    if (upload.active) {
        *err = "upload already in progress";
        return false;
    }
    uint32_t area_size = 0U;

    if (NvDb_GetSize(nvdbUser_fwuStored, &area_size) != nvdbRes_ok ||
        content_length < FWU_BLOB_OVERHEAD ||
        content_length > area_size) {
        *err = "invalid content-length";
        return false;
    }

    memset(&upload, 0, sizeof(upload));
    upload.active         = true;
    upload.content_length = content_length;
    upload.write_off      = 0U;
    snprintf(upload.name, sizeof(upload.name), "%s", name ? name : "");

    /* Say up front that the old blob is no longer wanted: a large user that
     * writes into space still holding data drags an erase onto every write,
     * which is exactly what the deferred delete exists to avoid. */
    (void)NvDb_Wipe(nvdbUser_fwuStored, NULL);

    /* Incoming upload invalidates whatever was stored */
    store_state.blob.valid         = false;
    store_state.status             = imgStore_uploading;
    store_state.bytes_transferred  = 0;
    store_state.total_bytes        = content_length;
    memset(store_state.name, 0, sizeof(store_state.name));
    memset(store_state.error_message, 0, sizeof(store_state.error_message));

    return true;
}

bool ImgStore_UploadWrite(const uint8_t *data, uint32_t len)
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
                ImgStore_UploadAbort("Flash write error");
                return false;
            }
        }
    }

    store_state.bytes_transferred = upload.bytes_received;
    return true;
}

bool ImgStore_UploadFinish(void)
{
    if (!upload.active) {
        return false;
    }

    bool flushed = flush_upload_page();
    upload.active = false;

    if (!flushed) {
        store_state.status = imgStore_error;
        strcpy(store_state.error_message, "Final flash write failed");
        return false;
    }

    /* Validate the received blob: manifest + CRC32 (keyless) */
    ImgStore_ScanArea(nvdbUser_fwuStored, &store_state.blob);

    if (store_state.blob.valid) {
        meta_store(upload.name);
        strcpy(store_state.name, upload.name);
        store_state.status = imgStore_ready;
        return true;
    }

    store_state.status = imgStore_error;
    strcpy(store_state.error_message, "Invalid blob (manifest/CRC)");
    return false;
}

void ImgStore_UploadAbort(const char *reason)
{
    if (upload.active) {
        upload.active = false;
        store_state.status = imgStore_error;
        snprintf(store_state.error_message, sizeof(store_state.error_message),
                 "%s", reason ? reason : "Upload aborted");
    }
}

/* --------------------------------------------------------------------------
 * Stored blob access
 * -------------------------------------------------------------------------- */

void ImgStore_DownloadBegin(void)
{
    store_state.status            = imgStore_downloading;
    store_state.bytes_transferred = 0;
    store_state.total_bytes       = store_state.blob.blob_size;
}

void ImgStore_DownloadEnd(bool ok, const char *err)
{
    if (ok) {
        store_state.status = imgStore_ready;
    } else {
        store_state.status = imgStore_error;
        snprintf(store_state.error_message, sizeof(store_state.error_message),
                 "%s", err ? err : "Download failed");
    }
}

bool ImgStore_Read(uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (NvDb_Read(nvdbUser_fwuStored, buf, offset, len) != nvdbRes_ok) {
        return false;
    }
    if (store_state.status == imgStore_downloading) {
        store_state.bytes_transferred = offset + len;
    }
    return true;
}

eImgStoreRes ImgStore_Delete(void)
{
    if (store_state.status == imgStore_uploading ||
        store_state.status == imgStore_downloading || read_held) {
        return imgRes_busy;
    }

    /* Kill the manifest magic first: nvDb's wipe is eventual, and "deleted"
     * has to be true the moment this returns.  Clearing bits is a program,
     * not an erase, so it lands synchronously and costs nothing. */
    uint32_t dead = 0U;

    if (NvDb_Write(nvdbUser_fwuStored, &dead, 0U, sizeof(dead)) != nvdbRes_ok ||
        NvDb_Write(nvdbUser_imageMeta, &dead, 0U, sizeof(dead)) != nvdbRes_ok) {
        return imgRes_flashErr;
    }
    if (NvDb_Wipe(nvdbUser_fwuStored, NULL) != nvdbRes_ok ||
        NvDb_Wipe(nvdbUser_imageMeta, NULL) != nvdbRes_ok) {
        return imgRes_flashErr;
    }

    memset(&store_state.blob, 0, sizeof(store_state.blob));
    memset(store_state.name, 0, sizeof(store_state.name));
    store_state.status            = imgStore_empty;
    store_state.bytes_transferred = 0;
    store_state.total_bytes       = 0;
    memset(store_state.error_message, 0, sizeof(store_state.error_message));

    return imgRes_ok;
}

/* --------------------------------------------------------------------------
 * Read hold
 * -------------------------------------------------------------------------- */

bool ImgStore_AcquireRead(void)
{
    if (!store_state.blob.valid || store_state.status != imgStore_ready) {
        return false;
    }
    read_held = true;
    return true;
}

void ImgStore_ReleaseRead(void)
{
    read_held = false;
}

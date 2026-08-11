#ifndef IMAGE_STORE_H
#define IMAGE_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "dfu_types.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Image management: persistent storage of ONE uploaded .pnfw blob in the
 * external-flash staged area, plus a small metadata record (original file
 * name) in its own sector.  Blobs are opaque encrypted containers — this
 * module checks only the cleartext manifest + trailing CRC32.
 *
 * This module knows nothing about the FWU process (boot status, install,
 * golden image).  FWU code consumes the stored image through the read
 * API below; a read hold keeps upload/delete from pulling the blob out
 * from under a long-running reader.
 */

#define IMG_STORE_NAME_MAX  64

typedef enum {
    imgStore_empty = 0,        /* no valid blob stored                    */
    imgStore_uploading,
    imgStore_ready,            /* valid blob stored (persistent)          */
    imgStore_downloading,
    imgStore_error,
    imgStore_last          /* sentinel */
} eImgStoreStatus;

typedef enum {
    imgRes_ok = 0,
    imgRes_busy,             /* transfer in progress / read hold active */
    imgRes_flashErr,
    imgRes_last          /* sentinel */
} eImgStoreRes;

/* Metadata of a blob area, read from its cleartext manifest and verified
 * against the trailing CRC32. */
typedef struct {
    bool       valid;
    sFwVerArea version;
    char       version_str[24];   /* e.g. "Pl1.0.0" */
    uint32_t   image_size;
    uint32_t   blob_size;
    uint32_t   blob_crc32;        /* trailing CRC word */
} sBlobInfo;

typedef struct {
    eImgStoreStatus status;
    sBlobInfo blob;                        /* stored image (staged area)   */
    char      name[IMG_STORE_NAME_MAX];    /* original file name ("" if
                                              unknown), persisted in flash */
    uint32_t  bytes_transferred;
    uint32_t  total_bytes;
    char      error_message[64];
} sImageStoreState;

/** Rescan the stored image + its metadata and reset transfer state.  Call
 *  once at startup (before the HTTP server accepts connections). */
void ImgStore_Init(void);

const sImageStoreState *ImgStore_GetState(void);

/** Scan any blob area (manifest sanity + whole-blob CRC32).  Also used by
 *  FWU code for the golden area — the blob layout is storage knowledge. */
void ImgStore_ScanArea(uint32_t base, uint32_t area_size, sBlobInfo *out);

/* ---- Upload session (single, sequential — HTTP task is the only user) ---
 * Begin() → Write() for each body chunk → Finish() / Abort().             */

/** @param name  original file name (may be NULL/empty), kept in metadata.
 *  @return true if the session may start; on false *err says why. */
bool ImgStore_UploadBegin(uint32_t content_length, const char *name,
                          const char **err);
/** Buffer + write body bytes to flash. false = flash error. */
bool ImgStore_UploadWrite(const uint8_t *data, uint32_t len);
/** Flush, validate the blob, persist metadata.  @return true if valid. */
bool ImgStore_UploadFinish(void);
/** Abandon the session (client vanished / flash error). */
void ImgStore_UploadAbort(const char *reason);

/* ---- Stored blob access ---- */

/** Mark download started/finished (status reporting only). */
void ImgStore_DownloadBegin(void);
void ImgStore_DownloadEnd(bool ok, const char *err);
/** Read from the stored blob.  @return false on flash error. */
bool ImgStore_Read(uint32_t offset, uint8_t *buf, uint32_t len);

/** Erase the stored image (manifest + metadata). */
eImgStoreRes ImgStore_Delete(void);

/* ---- Read hold (for external consumers, e.g. FWU golden promotion) ----
 * While held, upload and delete are refused so the blob stays intact.   */

/** @return false if no valid image is stored. */
bool ImgStore_AcquireRead(void);
void ImgStore_ReleaseRead(void);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_STORE_H */

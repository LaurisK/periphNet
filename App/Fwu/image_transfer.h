#ifndef IMAGE_TRANSFER_H
#define IMAGE_TRANSFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "dfu_types.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Protocol-agnostic FWU staging / control logic.  HTTP glue lives in
 * http_server.c; this module only deals with blobs, flash and boot status.
 */

typedef enum {
    IMG_STATUS_IDLE = 0,        /* no valid staged blob                    */
    IMG_STATUS_UPLOADING,
    IMG_STATUS_STAGED,          /* valid blob in staged area (persistent)  */
    IMG_STATUS_DOWNLOADING,
    IMG_STATUS_ERROR
} image_status_t;

/* Metadata of a blob area (staged / golden), read from its cleartext
 * manifest and verified against the trailing CRC32.  The app never sees
 * the plaintext image — blobs are opaque encrypted containers. */
typedef struct {
    bool       valid;
    sFwVerArea version;
    char       version_str[24];   /* e.g. "Pl1.0.0" */
    uint32_t   image_size;
    uint32_t   blob_size;
    uint32_t   blob_crc32;        /* trailing CRC word */
} sBlobInfo;

typedef struct {
    image_status_t status;
    uint32_t  bytes_transferred;
    uint32_t  total_bytes;
    char      error_message[64];
    sBlobInfo staged;
    sBlobInfo golden;
} image_state_t;

/** Rescan staged + golden areas and reset transfer state.  Call once at
 *  startup (before the HTTP server accepts connections). */
void image_transfer_init(void);

const image_state_t *image_transfer_get_status(void);

/* ---- Upload session (single, sequential — HTTP task is the only user) ---
 * begin() → write() for each body chunk → finish() / abort().            */

/** @return true if the session may start; on false *err says why. */
bool img_upload_begin(uint32_t content_length, const char **err);
/** Buffer + write body bytes to the staged area. false = flash error. */
bool img_upload_write(const uint8_t *data, uint32_t len);
/** Flush, rescan the staged area.  @return true if a valid blob is staged. */
bool img_upload_finish(void);
/** Abandon the session (client vanished / flash error). */
void img_upload_abort(const char *reason);

/* ---- Staged blob access (download) ---- */

/** Mark download started/finished (status reporting only). */
void img_download_begin(void);
void img_download_end(bool ok, const char *err);
/** Read from the staged blob.  @return false on flash error. */
bool img_read_staged(uint32_t offset, uint8_t *buf, uint32_t len);

/* ---- FWU control ---- */

typedef enum {
    IMG_CTL_OK = 0,
    IMG_CTL_NO_IMAGE,       /* no valid staged blob                */
    IMG_CTL_BUSY,           /* transfer / promotion in progress    */
    IMG_CTL_FLASH_ERR,      /* boot status / ext flash write error */
    IMG_CTL_ALREADY,        /* confirm: nothing pending            */
} eImgCtlRes;

/** Arm the FWU flag; on success a reboot is scheduled (defaultTask polls
 *  image_transfer_reboot_pending). */
eImgCtlRes img_install_request(void);

/** Outside-actor confirmation.  On success *promote tells whether a
 *  staged→golden promotion was queued. */
eImgCtlRes img_confirm(bool *promote);

/** Erase the staged blob manifest. */
eImgCtlRes img_delete(void);

/** Authenticate the RUNNING internal image via the BL API (HMAC-SHA256).
 *  @return FWU_OK if authentic; FWU_ERR_* otherwise (incl. unsigned/no
 *  header/BL API unavailable mapped to eFwuRes codes). */
eFwuRes img_verify_running(void);

/* ---- defaultTask-side jobs ---- */

bool  image_transfer_reboot_pending(void);
bool  image_transfer_promote_pending(void);
void  image_transfer_run_promotion(void);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_TRANSFER_H */

/**
 ******************************************************************************
 * @file    image_transfer.h
 * @brief   HTTP-based image transfer handlers (upload/download)
 ******************************************************************************
 * Provides HTTP endpoints for firmware image transfer:
 * - POST /api/firmware/upload  - Upload firmware binary to external flash
 * - GET  /api/firmware/download - Download firmware binary from external flash
 * - GET  /api/firmware/status   - Get current upload/download status
 ******************************************************************************
 */

#ifndef IMAGE_TRANSFER_H
#define IMAGE_TRANSFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/tcp.h"
#include <stdint.h>

/* Upload/download status */
typedef enum {
    IMG_STATUS_IDLE = 0,           /* No operation in progress */
    IMG_STATUS_UPLOADING,          /* Upload in progress */
    IMG_STATUS_UPLOAD_COMPLETE,    /* Upload complete, ready to install */
    IMG_STATUS_DOWNLOAD_READY,     /* Download ready */
    IMG_STATUS_DOWNLOADING,        /* Download in progress */
    IMG_STATUS_ERROR               /* Error occurred */
} image_status_t;

/* Upload/download state */
typedef struct {
    image_status_t status;
    uint32_t bytes_transferred;
    uint32_t total_bytes;
    uint32_t flash_address;
    uint32_t crc32;
    char error_message[64];
} image_state_t;

/**
 * @brief  Initialize image transfer module
 */
void image_transfer_init(void);

/**
 * @brief  Returns 1 if an upload session is active on this pcb
 */
int image_upload_has_active_session(struct tcp_pcb *pcb);

/**
 * @brief  Abort upload session by pcb (call when pcb is still valid)
 */
void image_upload_abort_session(struct tcp_pcb *pcb);

/**
 * @brief  Abort upload session by session pointer from tcp_arg
 *         (call from err callback where pcb is already freed)
 */
void image_upload_abort_session_ptr(void *session_ptr);

/**
 * @brief  Get current image transfer status
 * @return Pointer to current state (read-only)
 */
const image_state_t* image_transfer_get_status(void);

/**
 * @brief  Handle image upload (POST /api/firmware/upload)
 * @param  pcb: TCP control block
 * @param  p: Received packet buffer containing upload data
 * @retval ERR_OK on success, error code otherwise
 */
err_t image_upload_handler(struct tcp_pcb *pcb, struct pbuf *p);

/**
 * @brief  Handle image download (GET /api/firmware/download)
 * @param  pcb: TCP control block
 * @retval ERR_OK on success, error code otherwise
 */
err_t image_download_handler(struct tcp_pcb *pcb);

/**
 * @brief  Handle image status query (GET /api/firmware/status)
 * @param  pcb: TCP control block
 * @retval ERR_OK on success, error code otherwise
 */
err_t image_status_handler(struct tcp_pcb *pcb);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_TRANSFER_H */

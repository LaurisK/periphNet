/**
 ******************************************************************************
 * @file    firmware_update_http.h
 * @brief   HTTP-based firmware update handlers (upload/download)
 ******************************************************************************
 * Provides HTTP endpoints for OTA firmware update:
 * - POST /api/firmware/upload  - Upload firmware binary to external flash
 * - GET  /api/firmware/download - Download firmware binary from external flash
 * - GET  /api/firmware/status   - Get current upload/download status
 ******************************************************************************
 */

#ifndef FIRMWARE_UPDATE_HTTP_H
#define FIRMWARE_UPDATE_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/tcp.h"
#include <stdint.h>

/* Upload/download status */
typedef enum {
    FW_STATUS_IDLE = 0,           /* No operation in progress */
    FW_STATUS_UPLOADING,          /* Upload in progress */
    FW_STATUS_UPLOAD_COMPLETE,    /* Upload complete, ready to install */
    FW_STATUS_DOWNLOAD_READY,     /* Download ready */
    FW_STATUS_DOWNLOADING,        /* Download in progress */
    FW_STATUS_ERROR               /* Error occurred */
} firmware_status_t;

/* Upload/download state */
typedef struct {
    firmware_status_t status;
    uint32_t bytes_transferred;
    uint32_t total_bytes;
    uint32_t flash_address;
    uint32_t crc32;
    char error_message[64];
} firmware_state_t;

/**
 * @brief  Initialize firmware update HTTP module
 */
void firmware_update_http_init(void);

/**
 * @brief  Returns 1 if an upload session is active on this pcb
 */
int firmware_upload_has_active_session(struct tcp_pcb *pcb);

/**
 * @brief  Abort upload session by pcb (call when pcb is still valid)
 */
void firmware_upload_abort_session(struct tcp_pcb *pcb);

/**
 * @brief  Abort upload session by session pointer from tcp_arg
 *         (call from err callback where pcb is already freed)
 */
void firmware_upload_abort_session_ptr(void *session_ptr);

/**
 * @brief  Get current firmware update status
 * @return Pointer to current state (read-only)
 */
const firmware_state_t* firmware_update_get_status(void);

/**
 * @brief  Handle firmware upload (POST /api/firmware/upload)
 * @param  pcb: TCP control block
 * @param  p: Received packet buffer containing upload data
 * @retval ERR_OK on success, error code otherwise
 */
err_t firmware_upload_handler(struct tcp_pcb *pcb, struct pbuf *p);

/**
 * @brief  Handle firmware download (GET /api/firmware/download)
 * @param  pcb: TCP control block
 * @retval ERR_OK on success, error code otherwise
 */
err_t firmware_download_handler(struct tcp_pcb *pcb);

/**
 * @brief  Handle firmware status query (GET /api/firmware/status)
 * @param  pcb: TCP control block
 * @retval ERR_OK on success, error code otherwise
 */
err_t firmware_status_handler(struct tcp_pcb *pcb);

#ifdef __cplusplus
}
#endif

#endif /* FIRMWARE_UPDATE_HTTP_H */

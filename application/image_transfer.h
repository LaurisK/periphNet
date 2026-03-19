#ifndef IMAGE_TRANSFER_H
#define IMAGE_TRANSFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/tcp.h"
#include <stdint.h>

typedef enum {
    IMG_STATUS_IDLE = 0,
    IMG_STATUS_UPLOADING,
    IMG_STATUS_UPLOAD_COMPLETE,
    IMG_STATUS_DOWNLOAD_READY,
    IMG_STATUS_DOWNLOADING,
    IMG_STATUS_ERROR
} image_status_t;

typedef struct {
    image_status_t status;
    uint32_t bytes_transferred;
    uint32_t total_bytes;
    uint32_t flash_address;
    uint32_t crc32;
    char error_message[64];
} image_state_t;

void image_transfer_init(void);
int image_upload_has_active_session(struct tcp_pcb *pcb);
void image_upload_abort_session(struct tcp_pcb *pcb);
void image_upload_abort_session_ptr(void *session_ptr);
const image_state_t* image_transfer_get_status(void);
err_t image_upload_handler(struct tcp_pcb *pcb, struct pbuf *p);
err_t image_download_handler(struct tcp_pcb *pcb);
err_t image_status_handler(struct tcp_pcb *pcb);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_TRANSFER_H */

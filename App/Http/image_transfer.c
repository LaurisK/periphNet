#include "App/Http/image_transfer.h"
#include "w25q128.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "trice.h"
#include <string.h>
#include <stdio.h>

#define IMG_UPDATE_FLASH_ADDR   0x00001000U
#define IMG_MAX_SIZE            (480U * 1024U)
#define DOWNLOAD_CHUNK_SIZE     512U

#define UPLOAD_QUEUE_DEPTH      12
#define UPLOAD_TASK_STACK       1024  /* words */
#define UPLOAD_TASK_PRIORITY    (tskIDLE_PRIORITY + 2)

static image_state_t fw_state = {
    .status            = IMG_STATUS_IDLE,
    .bytes_transferred = 0,
    .total_bytes       = 0,
    .flash_address     = IMG_UPDATE_FLASH_ADDR,
    .crc32             = 0,
    .error_message     = {0}
};

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t content_length;
    uint32_t bytes_received;
    uint32_t flash_write_addr;
    uint8_t  header_parsed;
    uint8_t  buffer[256];
    uint16_t buffer_pos;
} sUploadSession;

static sUploadSession *active_upload = NULL;

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t bytes_sent;
    uint32_t total_bytes;
} sDownloadCtx;

static sDownloadCtx dl_ctx;

typedef struct {
    uint8_t  data[256];
    uint16_t len;
    uint32_t flash_addr;
    uint8_t  is_last;
    struct tcp_pcb *pcb;
    uint32_t total_bytes;
} sUploadPage;

static QueueHandle_t s_upload_queue = NULL;

typedef struct { struct tcp_pcb *pcb; uint16_t len; } sTcpRecvArg;
typedef struct {
    struct tcp_pcb *pcb;
    char     buf[128];
    uint16_t buf_len;
} sTcpRespArg;

/* --------------------------------------------------------------------------
 * tcpip_thread callbacks (called via tcpip_callback)
 * -------------------------------------------------------------------------- */

static void tcpip_do_recved(void *arg)
{
    sTcpRecvArg *a = (sTcpRecvArg *)arg;
    tcp_recved(a->pcb, a->len);
    vPortFree(a);
}

static void tcpip_do_upload_response(void *arg)
{
    sTcpRespArg *a = (sTcpRespArg *)arg;
    tcp_write(a->pcb, a->buf, a->buf_len, TCP_WRITE_FLAG_COPY);
    tcp_output(a->pcb);
    tcp_close(a->pcb);
    vPortFree(a);
}

/* --------------------------------------------------------------------------
 * Upload task: flash erase + write off tcpip_thread
 * -------------------------------------------------------------------------- */

static void image_upload_task(void *arg)
{
    (void)arg;
    sUploadPage msg;
    uint32_t current_sector = 0xFFFFFFFFU;

    for (;;) {
        if (xQueueReceive(s_upload_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        uint8_t write_ok = 1;

        if (msg.len > 0) {
            uint32_t sector = msg.flash_addr & ~0xFFFU;

            if (sector != current_sector) {
                write_ok = (W25Q128_EraseSector(sector) == W25Q128_OK);
                current_sector = write_ok ? sector : 0xFFFFFFFFU;
            }

            if (write_ok) {
                write_ok = (W25Q128_WritePage(msg.flash_addr, msg.data, msg.len) == W25Q128_OK);
            }
        }

        /* Advance TCP window via tcpip_thread */
        sTcpRecvArg *rarg = (sTcpRecvArg *)pvPortMalloc(sizeof(sTcpRecvArg));
        if (rarg) {
            rarg->pcb = msg.pcb;
            rarg->len = msg.len;
            if (tcpip_callback(tcpip_do_recved, rarg) != ERR_OK)
                vPortFree(rarg);
        }

        fw_state.bytes_transferred = msg.total_bytes;

        if (msg.is_last || !write_ok) {
            sTcpRespArg *resp = (sTcpRespArg *)pvPortMalloc(sizeof(sTcpRespArg));
            if (resp) {
                resp->pcb = msg.pcb;
                if (!write_ok) {
                    fw_state.status = IMG_STATUS_ERROR;
                    strcpy(fw_state.error_message, "Flash write error");
                    resp->buf_len = (uint16_t)snprintf(resp->buf, sizeof(resp->buf),
                        "HTTP/1.1 500 Internal Server Error\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"error\":\"flash write failed\"}\r\n");
                } else {
                    fw_state.status = IMG_STATUS_UPLOAD_COMPLETE;
                    fw_state.bytes_transferred = msg.total_bytes;
                    resp->buf_len = (uint16_t)snprintf(resp->buf, sizeof(resp->buf),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"status\":\"success\",\"bytes\":%lu}\r\n",
                        (unsigned long)msg.total_bytes);
                }
                if (tcpip_callback(tcpip_do_upload_response, resp) != ERR_OK)
                    vPortFree(resp);
            }
            current_sector = 0xFFFFFFFFU;
        }
    }
}

static void create_upload_task(void)
{
    if (s_upload_queue != NULL)
        return;
    s_upload_queue = xQueueCreate(UPLOAD_QUEUE_DEPTH, sizeof(sUploadPage));
    xTaskCreate(image_upload_task, "ImgUp",
                UPLOAD_TASK_STACK, NULL, UPLOAD_TASK_PRIORITY, NULL);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void image_transfer_init(void)
{
    if (active_upload != NULL) {
        vPortFree(active_upload);
        active_upload = NULL;
    }

    fw_state.status            = IMG_STATUS_IDLE;
    fw_state.bytes_transferred = 0;
    fw_state.total_bytes       = 0;
    fw_state.flash_address     = IMG_UPDATE_FLASH_ADDR;
    fw_state.crc32             = 0;
    memset(fw_state.error_message, 0, sizeof(fw_state.error_message));

    dl_ctx.pcb = NULL;

    create_upload_task();
}

const image_state_t *image_transfer_get_status(void)
{
    return &fw_state;
}

void image_upload_abort_session(struct tcp_pcb *pcb)
{
    if (active_upload != NULL && active_upload->pcb == pcb) {
        vPortFree(active_upload);
        active_upload = NULL;
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Connection aborted");
    }
}

void image_upload_abort_session_ptr(void *session_ptr)
{
    if (active_upload != NULL && active_upload == (sUploadSession *)session_ptr) {
        vPortFree(active_upload);
        active_upload = NULL;
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Connection aborted");
    }
}

/* --------------------------------------------------------------------------
 * Upload: HTTP header / body parsing
 * -------------------------------------------------------------------------- */

static uint32_t parse_content_length(const char *header, uint16_t header_len)
{
    const char *pattern = "Content-Length:";
    const uint16_t pattern_len = 15;

    if (header_len < pattern_len)
        return 0;

    for (uint16_t i = 0; i <= header_len - pattern_len; i++) {
        uint8_t match = 1;
        for (uint16_t j = 0; j < pattern_len; j++) {
            char c1 = header[i + j];
            char c2 = pattern[j];
            if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
            if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
            if (c1 != c2) { match = 0; break; }
        }
        if (match) {
            const char *num_start = &header[i + pattern_len];
            const char *num_end   = &header[header_len];
            while (num_start < num_end && (*num_start == ' ' || *num_start == '\t'))
                num_start++;
            uint32_t value = 0;
            while (num_start < num_end && *num_start >= '0' && *num_start <= '9')
                value = value * 10 + (*num_start++ - '0');
            return value;
        }
    }
    return 0;
}

static const char *find_http_body(const char *data, uint16_t data_len)
{
    for (uint16_t i = 0; i + 3 < data_len; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' &&
            data[i+2] == '\r' && data[i+3] == '\n') {
            return &data[i + 4];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Upload: page-buffer flush -> queue to upload task
 * -------------------------------------------------------------------------- */

static err_t queue_upload_page(sUploadSession *session, uint8_t is_last)
{
    if (session->buffer_pos == 0 && !is_last)
        return ERR_OK;

    /* Heap-allocate to avoid ~276 B on tcpip_thread stack (only 2048 B total) */
    sUploadPage *page = (sUploadPage *)pvPortMalloc(sizeof(sUploadPage));
    if (page == NULL) {
        strcpy(fw_state.error_message, "Upload page alloc failed");
        fw_state.status = IMG_STATUS_ERROR;
        return ERR_ABRT;
    }
    memcpy(page->data, session->buffer, session->buffer_pos);
    page->len         = session->buffer_pos;
    page->flash_addr  = session->flash_write_addr;
    page->is_last     = is_last;
    page->pcb         = session->pcb;
    page->total_bytes = session->bytes_received;

    err_t err = (xQueueSend(s_upload_queue, page, 0) == pdTRUE) ? ERR_OK : ERR_ABRT;
    vPortFree(page);
    if (err != ERR_OK) {
        strcpy(fw_state.error_message, "Upload queue full");
        fw_state.status = IMG_STATUS_ERROR;
        return ERR_ABRT;
    }

    session->flash_write_addr += session->buffer_pos;
    session->buffer_pos = 0;
    return ERR_OK;
}

static err_t process_upload_data(sUploadSession *session, const uint8_t *data, uint16_t len)
{
    uint16_t offset = 0;

    while (offset < len) {
        uint16_t chunk = len - offset;
        uint16_t space = sizeof(session->buffer) - session->buffer_pos;
        if (chunk > space) chunk = space;

        memcpy(&session->buffer[session->buffer_pos], &data[offset], chunk);
        session->buffer_pos += chunk;
        offset += chunk;
        session->bytes_received += chunk;

        if (session->buffer_pos >= sizeof(session->buffer)) {
            if (queue_upload_page(session, 0) != ERR_OK) {
                fw_state.status = IMG_STATUS_ERROR;
                return ERR_ABRT;
            }
        }
    }

    fw_state.bytes_transferred = session->bytes_received;
    return ERR_OK;
}

/* --------------------------------------------------------------------------
 * image_upload_handler
 * -------------------------------------------------------------------------- */

err_t image_upload_handler(struct tcp_pcb *pcb, struct pbuf *p)
{
    sUploadSession *session = active_upload;
    char *data     = (char *)p->payload;
    uint16_t data_len = p->len;

    if (session == NULL) {
        session = (sUploadSession *)pvPortMalloc(sizeof(sUploadSession));
        if (session == NULL) {
            strcpy(fw_state.error_message, "Out of memory");
            fw_state.status = IMG_STATUS_ERROR;
            tcp_recved(pcb, p->tot_len);
            tcp_close(pcb);
            return ERR_MEM;
        }

        memset(session, 0, sizeof(sUploadSession));
        session->pcb             = pcb;
        session->flash_write_addr = IMG_UPDATE_FLASH_ADDR;

        session->content_length = parse_content_length(data, data_len);
        if (session->content_length == 0 || session->content_length > IMG_MAX_SIZE) {
            vPortFree(session);
            strcpy(fw_state.error_message, "Invalid content length");
            fw_state.status = IMG_STATUS_ERROR;

            const char *err_resp =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"error\":\"invalid content-length\"}\r\n";
            tcp_recved(pcb, p->tot_len);
            tcp_write(pcb, err_resp, strlen(err_resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_close(pcb);
            return ERR_VAL;
        }

        fw_state.status            = IMG_STATUS_UPLOADING;
        fw_state.bytes_transferred = 0;
        fw_state.total_bytes       = session->content_length;
        fw_state.flash_address     = IMG_UPDATE_FLASH_ADDR;
        memset(fw_state.error_message, 0, sizeof(fw_state.error_message));

        active_upload = session;
        tcp_arg(pcb, session);
    }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        char *seg_data    = (char *)q->payload;
        uint16_t seg_len  = q->len;

        const char *body_start = seg_data;
        uint16_t body_len      = seg_len;

        if (!session->header_parsed) {
            body_start = find_http_body(seg_data, seg_len);
            if (body_start == NULL)
                continue;
            body_len = seg_len - (uint16_t)(body_start - seg_data);
            session->header_parsed = 1;
        }

        if (session->bytes_received < session->content_length) {
            uint32_t remaining = session->content_length - session->bytes_received;
            if (body_len > remaining)
                body_len = (uint16_t)remaining;
        } else {
            body_len = 0;
        }

        if (body_len > 0) {
            if (process_upload_data(session, (const uint8_t *)body_start, body_len) != ERR_OK) {
                tcp_arg(pcb, NULL);
                vPortFree(session);
                active_upload = NULL;

                const char *err_resp =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: application/json\r\nConnection: close\r\n\r\n"
                    "{\"error\":\"flash write error\"}\r\n";
                tcp_recved(pcb, p->tot_len);
                tcp_write(pcb, err_resp, strlen(err_resp), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_close(pcb);
                return ERR_ABRT;
            }
        }
    }

    if (session->bytes_received >= session->content_length) {
        if (queue_upload_page(session, 1) != ERR_OK) {
            strcpy(fw_state.error_message, "Final flash write failed");
            fw_state.status = IMG_STATUS_ERROR;

            tcp_arg(pcb, NULL);
            vPortFree(session);
            active_upload = NULL;

            const char *err_resp =
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: application/json\r\nConnection: close\r\n\r\n"
                "{\"error\":\"final flash write failed\"}\r\n";
            tcp_recved(pcb, p->tot_len);
            tcp_write(pcb, err_resp, strlen(err_resp), TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            tcp_close(pcb);
            return ERR_ABRT;
        }

        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        vPortFree(session);
        active_upload = NULL;
        return ERR_OK;
    }

    /* tcp_recved is called by the upload task in batches */
    return ERR_OK;
}

/* --------------------------------------------------------------------------
 * Download handler
 * -------------------------------------------------------------------------- */

static err_t send_download_chunk(struct tcp_pcb *pcb)
{
    if (dl_ctx.pcb != pcb)
        return ERR_OK;

    if (dl_ctx.bytes_sent >= dl_ctx.total_bytes) {
        fw_state.status = IMG_STATUS_DOWNLOAD_READY;
        dl_ctx.pcb = NULL;
        tcp_close(pcb);
        return ERR_OK;
    }

    uint32_t remaining = dl_ctx.total_bytes - dl_ctx.bytes_sent;
    uint16_t sndbuf    = tcp_sndbuf(pcb);
    uint32_t chunk     = remaining < (uint32_t)sndbuf ? remaining : (uint32_t)sndbuf;
    if (chunk > DOWNLOAD_CHUNK_SIZE) chunk = DOWNLOAD_CHUNK_SIZE;

    if (chunk == 0) {
        tcp_output(pcb);
        return ERR_OK;
    }

    uint8_t read_buf[DOWNLOAD_CHUNK_SIZE];
    if (W25Q128_Read(IMG_UPDATE_FLASH_ADDR + dl_ctx.bytes_sent, read_buf, chunk) != W25Q128_OK) {
        strcpy(fw_state.error_message, "Flash read error");
        fw_state.status = IMG_STATUS_ERROR;
        dl_ctx.pcb = NULL;
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    err_t err = tcp_write(pcb, read_buf, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        if (err == ERR_MEM) {
            tcp_output(pcb);
            return ERR_OK;
        }
        strcpy(fw_state.error_message, "TCP write error");
        fw_state.status = IMG_STATUS_ERROR;
        dl_ctx.pcb = NULL;
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    dl_ctx.bytes_sent += chunk;
    fw_state.bytes_transferred = dl_ctx.bytes_sent;
    tcp_output(pcb);
    return ERR_OK;
}

static err_t image_download_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg;
    (void)len;
    return send_download_chunk(pcb);
}

err_t image_download_handler(struct tcp_pcb *pcb)
{
    if (fw_state.status != IMG_STATUS_UPLOAD_COMPLETE &&
        fw_state.status != IMG_STATUS_DOWNLOAD_READY) {
        const char *response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\nConnection: close\r\n\r\n"
            "No firmware available for download\r\n";
        tcp_write(pcb, response, strlen(response), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        tcp_close(pcb);
        return ERR_OK;
    }

    dl_ctx.pcb        = pcb;
    dl_ctx.bytes_sent = 0;
    dl_ctx.total_bytes = fw_state.total_bytes;

    fw_state.status            = IMG_STATUS_DOWNLOADING;
    fw_state.bytes_transferred = 0;

    tcp_sent(pcb, image_download_sent);

    char headers[256];
    int header_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lu\r\n"
        "Content-Disposition: attachment; filename=\"firmware.bin\"\r\n"
        "Connection: close\r\n"
        "\r\n",
        (unsigned long)fw_state.total_bytes);

    err_t err = tcp_write(pcb, headers, header_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        dl_ctx.pcb = NULL;
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Header write error");
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    tcp_output(pcb);
    return send_download_chunk(pcb);
}

/* --------------------------------------------------------------------------
 * Status handler
 * -------------------------------------------------------------------------- */

err_t image_status_handler(struct tcp_pcb *pcb)
{
    char *response = (char *)pvPortMalloc(512);
    if (response == NULL) {
        const char *err = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
        tcp_write(pcb, err, strlen(err), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        return ERR_OK;
    }

    const char *status_str;
    switch (fw_state.status) {
        case IMG_STATUS_IDLE:            status_str = "idle";            break;
        case IMG_STATUS_UPLOADING:       status_str = "uploading";      break;
        case IMG_STATUS_UPLOAD_COMPLETE: status_str = "upload_complete"; break;
        case IMG_STATUS_DOWNLOAD_READY:  status_str = "download_ready"; break;
        case IMG_STATUS_DOWNLOADING:     status_str = "downloading";    break;
        case IMG_STATUS_ERROR:           status_str = "error";          break;
        default:                         status_str = "unknown";        break;
    }

    uint32_t progress_pct = 0;
    if (fw_state.total_bytes > 0)
        progress_pct = (fw_state.bytes_transferred * 100) / fw_state.total_bytes;

    int len = snprintf(response, 512,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"status\":\"%s\","
        "\"bytes_transferred\":%lu,"
        "\"total_bytes\":%lu,"
        "\"progress\":%lu,"
        "\"error\":\"%s\"}\r\n",
        status_str,
        (unsigned long)fw_state.bytes_transferred,
        (unsigned long)fw_state.total_bytes,
        (unsigned long)progress_pct,
        fw_state.error_message);

    tcp_write(pcb, response, len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    vPortFree(response);
    return ERR_OK;
}

#include "image_transfer.h"
#include "w25q128.h"
#include "bl_app_contract.h"
#include "lwip/tcp.h"
#include "FreeRTOS.h"
#include <string.h>
#include <stdio.h>

/* In production builds, flash operations run in a dedicated FreeRTOS task so
 * they don't block tcpip_thread.  Unit tests define TEST_MODE to keep the
 * original synchronous behaviour so that all existing tests continue to work
 * without needing queue/task mocks. */
#ifndef TEST_MODE
#include "task.h"
#include "queue.h"
#include "lwip/tcpip.h"
#include "trice.h"
#endif

#define IMG_UPDATE_FLASH_ADDR   EXT_FLASH_FWU_IMG_ADDR
#define IMG_MAX_SIZE            (480 * 1024)
#define DOWNLOAD_CHUNK_SIZE     512

static image_state_t fw_state = {
    .status = IMG_STATUS_IDLE,
    .bytes_transferred = 0,
    .total_bytes = 0,
    .flash_address = IMG_UPDATE_FLASH_ADDR,
    .crc32 = 0,
    .error_message = {0}
};

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t content_length;
    uint32_t bytes_received;
    uint32_t flash_write_addr;
    uint8_t header_parsed;
    uint8_t buffer[256];
    uint16_t buffer_pos;
} upload_session_t;

static upload_session_t *active_upload = NULL;

/* Incremented at each major step in image_upload_handler/queue_upload_page
 * (tcpip_thread context).  Read by HardFault_Handler_C to pinpoint crash. */
volatile uint32_t g_upload_progress = 0;

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t bytes_sent;
    uint32_t total_bytes;
} download_ctx_t;

static download_ctx_t dl_ctx;

/* ============================================================================
 * Task-based flash upload (production only)
 * ============================================================================ */
#ifndef TEST_MODE

#define UPLOAD_QUEUE_DEPTH   12
#define UPLOAD_TASK_STACK    1024  /* words */
#define UPLOAD_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

/* One entry in the upload queue: one flash page worth of data */
typedef struct {
    uint8_t  data[256];
    uint16_t len;
    uint32_t flash_addr;
    uint8_t  is_last;       /* 1 = final page, send HTTP response after write */
    struct tcp_pcb *pcb;
    uint32_t total_bytes;   /* total upload size, used in response */
} sUploadPage;

static QueueHandle_t s_upload_queue = NULL;

/* Arguments passed to tcpip_callback helpers (heap-allocated, freed inside) */
typedef struct { struct tcp_pcb *pcb; uint16_t len; } sTcpRecvArg;

/* Response is pre-built by the upload task (large stack) so tcpip_do_upload_response
 * runs with minimal stack in tcpip_thread. */
typedef struct {
    struct tcp_pcb *pcb;
    char  buf[128];
    uint16_t buf_len;
} sTcpRespArg;

/* Called by tcpip_thread via tcpip_callback to advance the TCP receive window */
static void tcpip_do_recved(void *arg)
{
    sTcpRecvArg *a = (sTcpRecvArg *)arg;
    tcp_recved(a->pcb, a->len);
    vPortFree(a);
}

/* Called by tcpip_thread via tcpip_callback to send the pre-built HTTP upload response */
static void tcpip_do_upload_response(void *arg)
{
    sTcpRespArg *a = (sTcpRespArg *)arg;
    tcp_write(a->pcb, a->buf, a->buf_len, TCP_WRITE_FLAG_COPY);
    tcp_output(a->pcb);
    tcp_close(a->pcb);
    vPortFree(a);
}

/* FreeRTOS task: performs flash erase + write off the tcpip_thread.
 * After each page write it uses tcpip_callback to advance the TCP window.
 * When the final page is written it uses tcpip_callback to send the HTTP
 * response and close the connection. */
static void image_upload_task(void *arg)
{
    (void)arg;
    sUploadPage msg;
    uint32_t current_sector = 0xFFFFFFFF;

    for (;;) {
        if (xQueueReceive(s_upload_queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        /* SEQ: page dequeued — Phase 1→2 transition */
        TRice(iD(4757), "FWU: page addr=0x%X len=%u last=%u total=%u\n",
              (unsigned)msg.flash_addr, (unsigned)msg.len,
              (unsigned)msg.is_last, (unsigned)msg.total_bytes);
        TRice(iD(1932), "FWU: heap=%u hwm=%u\n",
              (unsigned)xPortGetFreeHeapSize(),
              (unsigned)uxTaskGetStackHighWaterMark(NULL));

        uint8_t write_ok = 1;

        /* Zero-length is_last means upload size was an exact multiple of 256:
         * the page buffer was empty when queue_upload_page(session, 1) was called.
         * Nothing to write; just send the response. */
        if (msg.len > 0) {
            uint32_t sector = msg.flash_addr & ~0xFFFU;

            /* Erase sector before the first write to it */
            if (sector != current_sector) {
                /* SEQ: Phase 2 — sector erase start; TCP window stalls here (~50ms) */
                TRice(iD(2980), "FWU: erase_start 0x%X\n", (unsigned)sector);
                write_ok = (W25Q128_EraseSector(sector) == W25Q128_OK);
                /* SEQ: Phase 2→3 transition — erase done, writes can now proceed */
                if (write_ok)
                    TRice(iD(2943), "FWU: erase_done 0x%X\n", (unsigned)sector);
                else
                    TRice(iD(3640), "FWU: erase_FAIL 0x%X\n", (unsigned)sector);
                current_sector = write_ok ? sector : 0xFFFFFFFF;
            }

            if (write_ok) {
                write_ok = (W25Q128_WritePage(msg.flash_addr, msg.data, msg.len) == W25Q128_OK);
                /* SEQ: Phase 3 — page written; tcp_recved will follow to open the window */
                if (write_ok)
                    TRice(iD(4905), "FWU: write_ok 0x%X %uB\n", (unsigned)msg.flash_addr, (unsigned)msg.len);
                else
                    TRice(iD(6107), "FWU: write_FAIL 0x%X\n", (unsigned)msg.flash_addr);
            }
        }

        /* Advance TCP window: tells lwIP the application consumed msg.len bytes,
         * allowing the peer to send the next chunk. */
        /* SEQ: Phase 3 — dispatch tcp_recved to tcpip_thread; window opens by len bytes.
         * lwIP only sends a window-update ACK once cumulative freed >= TCP_WND_UPDATE_THRESHOLD
         * (536B), so the client unblocks after every 3rd call here (3*256=768 >= 536). */
        TRice(iD(2788), "FWU: tcp_recved %u -> tcpip_thread\n", (unsigned)msg.len);
        sTcpRecvArg *rarg = (sTcpRecvArg *)pvPortMalloc(sizeof(sTcpRecvArg));
        if (rarg) {
            rarg->pcb  = msg.pcb;
            rarg->len  = msg.len;
            err_t rarg_err = tcpip_callback(tcpip_do_recved, rarg);
            if (rarg_err != ERR_OK) {
                TRice(iD(7465), "FWU: tcp_recved cb FAIL err=%d\n", (int)rarg_err);
                vPortFree(rarg);
            }
        } else {
            TRice(iD(5410), "FWU: tcp_recved malloc FAIL\n");
        }

        if (msg.is_last || !write_ok) {
            sTcpRespArg *resp = (sTcpRespArg *)pvPortMalloc(sizeof(sTcpRespArg));
            if (resp) {
                resp->pcb = msg.pcb;
                if (!write_ok) {
                    /* SEQ: Phase 6 error — dispatch HTTP 500 via tcpip_thread */
                    TRice(iD(2396), "FWU: upload_err -> HTTP 500\n");
                    fw_state.status = IMG_STATUS_ERROR;
                    strcpy(fw_state.error_message, "Flash write error");
                    resp->buf_len = (uint16_t)snprintf(resp->buf, sizeof(resp->buf),
                        "HTTP/1.1 500 Internal Server Error\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"error\":\"flash write failed\"}\r\n");
                } else {
                    /* SEQ: Phase 6 — all pages written; dispatch HTTP 200 via tcpip_thread */
                    TRice(iD(7073), "FWU: upload_done %u bytes -> HTTP 200\n", (unsigned)msg.total_bytes);
                    fw_state.status = IMG_STATUS_UPLOAD_COMPLETE;
                    fw_state.bytes_transferred = msg.total_bytes;
                    resp->buf_len = (uint16_t)snprintf(resp->buf, sizeof(resp->buf),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"status\":\"success\",\"bytes\":%lu}\r\n",
                        (unsigned long)msg.total_bytes);
                }
                err_t resp_err = tcpip_callback(tcpip_do_upload_response, resp);
                if (resp_err != ERR_OK) {
                    TRice(iD(5780), "FWU: resp cb FAIL err=%d heap=%u\n",
                          (int)resp_err, (unsigned)xPortGetFreeHeapSize());
                    vPortFree(resp);
                }
            } else {
                TRice(iD(1977), "FWU: resp malloc FAIL heap=%u\n",
                      (unsigned)xPortGetFreeHeapSize());
            }
            current_sector = 0xFFFFFFFF;
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

#endif /* !TEST_MODE */

/* ============================================================================
 * Shared helpers
 * ============================================================================ */

void image_transfer_init(void)
{
    if (active_upload != NULL) {
        vPortFree(active_upload);
        active_upload = NULL;
    }

    fw_state.status = IMG_STATUS_IDLE;
    fw_state.bytes_transferred = 0;
    fw_state.total_bytes = 0;
    fw_state.flash_address = IMG_UPDATE_FLASH_ADDR;
    fw_state.crc32 = 0;
    memset(fw_state.error_message, 0, sizeof(fw_state.error_message));

    dl_ctx.pcb = NULL;

#ifndef TEST_MODE
    create_upload_task();
#endif
}

const image_state_t* image_transfer_get_status(void)
{
    return &fw_state;
}

int image_upload_has_active_session(struct tcp_pcb *pcb)
{
    return (active_upload != NULL && active_upload->pcb == pcb) ? 1 : 0;
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
    if (active_upload != NULL && active_upload == (upload_session_t *)session_ptr) {
        vPortFree(active_upload);
        active_upload = NULL;
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Connection aborted");
    }
}

/* ============================================================================
 * Upload: HTTP header / body parsing helpers
 * ============================================================================ */

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
            const char *num_end = &header[header_len];

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

static const char* find_http_body(const char *data, uint16_t data_len)
{
    for (uint16_t i = 0; i + 3 < data_len; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' &&
            data[i+2] == '\r' && data[i+3] == '\n') {
            return &data[i + 4];
        }
    }
    return NULL;
}

/* ============================================================================
 * Upload: page-buffer flush
 *
 * In TEST_MODE: writes to flash synchronously (no task, no queue).
 * In production: sends the filled page to the upload task queue.
 * The upload task handles erase + write and advances the TCP window via
 * tcpip_callback — so tcp_recved must NOT be called here in production.
 * ============================================================================ */

static err_t queue_upload_page(upload_session_t *session, uint8_t is_last)
{
    if (session->buffer_pos == 0 && !is_last)
        return ERR_OK;

#ifndef TEST_MODE
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

    g_upload_progress = 100u | ((uint32_t)is_last << 8) | ((page->flash_addr & 0xFFF) << 16);
    err_t send_err = (xQueueSend(s_upload_queue, page, 0) == pdTRUE) ? ERR_OK : ERR_ABRT;
    vPortFree(page);
    if (send_err != ERR_OK) {
        strcpy(fw_state.error_message, "Upload queue full");
        fw_state.status = IMG_STATUS_ERROR;
        return ERR_ABRT;
    }
#else
    /* Synchronous path for unit tests */
    W25Q128_Status_t status = W25Q128_WritePage(
        session->flash_write_addr,
        session->buffer,
        session->buffer_pos
    );
    if (status != W25Q128_OK) {
        strcpy(fw_state.error_message, "Flash write error");
        fw_state.status = IMG_STATUS_ERROR;
        return ERR_ABRT;
    }
#endif

    session->flash_write_addr += session->buffer_pos;
    session->buffer_pos = 0;
    return ERR_OK;
}

static err_t process_upload_data(upload_session_t *session, const uint8_t *data, uint16_t len)
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

/* ============================================================================
 * image_upload_handler
 * ============================================================================ */

err_t image_upload_handler(struct tcp_pcb *pcb, struct pbuf *p)
{
    upload_session_t *session = active_upload;

    char *data = (char *)p->payload;
    uint16_t data_len = p->len;

    if (session == NULL) {
        session = (upload_session_t *)pvPortMalloc(sizeof(upload_session_t));
        if (session == NULL) {
            strcpy(fw_state.error_message, "Out of memory");
            fw_state.status = IMG_STATUS_ERROR;
            tcp_recved(pcb, p->tot_len);
            tcp_close(pcb);
            return ERR_MEM;
        }

        g_upload_progress = 1;
        memset(session, 0, sizeof(upload_session_t));
        session->pcb = pcb;
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

#ifdef TEST_MODE
        /* In tests, erase synchronously so erase-failure tests still work */
        uint32_t sectors_needed = (session->content_length + 4095) / 4096;
        for (uint32_t i = 0; i < sectors_needed; i++) {
            if (W25Q128_EraseSector(IMG_UPDATE_FLASH_ADDR + (i * 4096)) != W25Q128_OK) {
                vPortFree(session);
                strcpy(fw_state.error_message, "Flash erase failed");
                fw_state.status = IMG_STATUS_ERROR;

                const char *err_resp =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: application/json\r\nConnection: close\r\n\r\n"
                    "{\"error\":\"flash erase failed\"}\r\n";
                tcp_recved(pcb, p->tot_len);
                tcp_write(pcb, err_resp, strlen(err_resp), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                tcp_close(pcb);
                return ERR_ABRT;
            }
        }
#endif /* TEST_MODE */

        g_upload_progress = 2;
        fw_state.status = IMG_STATUS_UPLOADING;
        fw_state.bytes_transferred = 0;
        fw_state.total_bytes = session->content_length;
        fw_state.flash_address = IMG_UPDATE_FLASH_ADDR;

        active_upload = session;
        tcp_arg(pcb, session);
        g_upload_progress = 3;
    }

    g_upload_progress = 4;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        char *seg_data = (char *)q->payload;
        uint16_t seg_len = q->len;

        const char *body_start = seg_data;
        uint16_t body_len = seg_len;

        if (!session->header_parsed) {
            body_start = find_http_body(seg_data, seg_len);
            if (body_start == NULL) {
                continue;
            }
            body_len = seg_len - (uint16_t)(body_start - seg_data);
            session->header_parsed = 1;
        }

        if (session->bytes_received < session->content_length) {
            uint32_t remaining = session->content_length - session->bytes_received;
            if (body_len > remaining) {
                body_len = (uint16_t)remaining;
            }
        } else {
            body_len = 0;
        }

        if (body_len > 0) {
            g_upload_progress = 5;
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
            g_upload_progress = 6;
        }
    }

    if (session->bytes_received >= session->content_length) {
        /* Flush any partial final page */
        g_upload_progress = 7;
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

        g_upload_progress = 8;
        tcp_arg(pcb, NULL);
#ifndef TEST_MODE
        /* Deregister recv callback so the FIN from curl doesn't cause a second
         * tcp_close racing with the task's tcpip_do_upload_response. */
        g_upload_progress = 9;
        tcp_recv(pcb, NULL);
#endif
        g_upload_progress = 10;
        vPortFree(session);
        active_upload = NULL;
        g_upload_progress = 11;

#ifdef TEST_MODE
        /* In tests, send the HTTP response immediately (no task) */
        fw_state.status = IMG_STATUS_UPLOAD_COMPLETE;
        fw_state.bytes_transferred = fw_state.total_bytes;

        char response[256];
        int len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{\"status\":\"success\",\"bytes\":%lu}\r\n",
            fw_state.bytes_transferred);

        tcp_recved(pcb, p->tot_len);
        tcp_write(pcb, response, len, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        tcp_close(pcb);
#else
        /* In production, the upload task sends the response via tcpip_callback.
         * Do NOT call tcp_recved here — the task will do it after the final write. */
#endif
        return ERR_OK;
    }

#ifdef TEST_MODE
    tcp_recved(pcb, p->tot_len);
#endif
    /* In production: tcp_recved is called by the task after each page write. */
    return ERR_OK;
}

/* ============================================================================
 * Download handler
 * ============================================================================ */

static err_t send_download_chunk(struct tcp_pcb *pcb)
{
    if (dl_ctx.pcb != pcb) return ERR_OK;

    if (dl_ctx.bytes_sent >= dl_ctx.total_bytes) {
        fw_state.status = IMG_STATUS_DOWNLOAD_READY;
        dl_ctx.pcb = NULL;
        tcp_close(pcb);
        return ERR_OK;
    }

    uint32_t remaining = dl_ctx.total_bytes - dl_ctx.bytes_sent;
    uint16_t sndbuf = tcp_sndbuf(pcb);
    uint32_t chunk = remaining < (uint32_t)sndbuf ? remaining : (uint32_t)sndbuf;
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

    dl_ctx.pcb = pcb;
    dl_ctx.bytes_sent = 0;
    dl_ctx.total_bytes = fw_state.total_bytes;

    fw_state.status = IMG_STATUS_DOWNLOADING;
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
        fw_state.total_bytes);

    err_t err = tcp_write(pcb, headers, header_len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        dl_ctx.pcb = NULL;
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Header write error");
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    tcp_output(pcb);  /* flush headers immediately so they arrive even if first chunk is delayed */
    return send_download_chunk(pcb);
}

/* ============================================================================
 * Status handler
 * ============================================================================ */

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
        case IMG_STATUS_IDLE:            status_str = "idle"; break;
        case IMG_STATUS_UPLOADING:       status_str = "uploading"; break;
        case IMG_STATUS_UPLOAD_COMPLETE: status_str = "upload_complete"; break;
        case IMG_STATUS_DOWNLOAD_READY:  status_str = "download_ready"; break;
        case IMG_STATUS_DOWNLOADING:     status_str = "downloading"; break;
        case IMG_STATUS_ERROR:           status_str = "error"; break;
        default:                         status_str = "unknown"; break;
    }

    uint32_t progress_pct = 0;
    if (fw_state.total_bytes > 0)
        progress_pct = (fw_state.bytes_transferred * 100) / fw_state.total_bytes;

    int len = snprintf(response, 512,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{"
        "\"status\":\"%s\","
        "\"bytes_transferred\":%lu,"
        "\"total_bytes\":%lu,"
        "\"progress\":%lu,"
        "\"error\":\"%s\""
        "}\r\n",
        status_str,
        fw_state.bytes_transferred,
        fw_state.total_bytes,
        progress_pct,
        fw_state.error_message);

    tcp_write(pcb, response, len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    vPortFree(response);

    return ERR_OK;
}

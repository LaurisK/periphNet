/**
 ******************************************************************************
 * @file    firmware_update_http.c
 * @brief   HTTP-based firmware update implementation
 ******************************************************************************
 */

#include "firmware_update_http.h"
#include "w25q128.h"
#include "bl_app_contract.h"
#include "lwip/tcp.h"
#include "FreeRTOS.h"
#include <string.h>
#include <stdio.h>

/* External flash firmware update image location */
#define FW_UPDATE_FLASH_ADDR    EXT_FLASH_FWU_IMG_ADDR  /* 0x00001000 */
#define FW_MAX_SIZE             (480 * 1024)            /* 480KB max */

/* Download chunk size - fits in lwIP send buffer */
#define DOWNLOAD_CHUNK_SIZE     512

/* Upload state tracking */
static firmware_state_t fw_state = {
    .status = FW_STATUS_IDLE,
    .bytes_transferred = 0,
    .total_bytes = 0,
    .flash_address = FW_UPDATE_FLASH_ADDR,
    .crc32 = 0,
    .error_message = {0}
};

/* Upload session state - pointer stored in tcp_arg for routing */
typedef struct {
    struct tcp_pcb *pcb;
    uint32_t content_length;
    uint32_t bytes_received;
    uint32_t flash_write_addr;
    uint8_t header_parsed;
    uint8_t buffer[256];  /* Write buffer for flash page alignment */
    uint16_t buffer_pos;
} upload_session_t;

static upload_session_t *active_upload = NULL;

/* Download state - uses tcp_sent callback for flow control */
typedef struct {
    struct tcp_pcb *pcb;
    uint32_t bytes_sent;
    uint32_t total_bytes;
} download_ctx_t;

static download_ctx_t dl_ctx;

/**
 * @brief  Initialize firmware update HTTP module
 */
void firmware_update_http_init(void)
{
    fw_state.status = FW_STATUS_IDLE;
    fw_state.bytes_transferred = 0;
    fw_state.total_bytes = 0;
    fw_state.flash_address = FW_UPDATE_FLASH_ADDR;
    fw_state.crc32 = 0;
    memset(fw_state.error_message, 0, sizeof(fw_state.error_message));

    active_upload = NULL;
    dl_ctx.pcb = NULL;
}

/**
 * @brief  Get current firmware update status
 */
const firmware_state_t* firmware_update_get_status(void)
{
    return &fw_state;
}

/**
 * @brief  Returns 1 if an upload session is active on this pcb
 */
int firmware_upload_has_active_session(struct tcp_pcb *pcb)
{
    return (active_upload != NULL && active_upload->pcb == pcb) ? 1 : 0;
}

/**
 * @brief  Abort upload session by pcb (call when pcb is still valid)
 */
void firmware_upload_abort_session(struct tcp_pcb *pcb)
{
    if (active_upload != NULL && active_upload->pcb == pcb) {
        vPortFree(active_upload);
        active_upload = NULL;
        fw_state.status = FW_STATUS_ERROR;
        strcpy(fw_state.error_message, "Connection aborted");
    }
}

/**
 * @brief  Abort upload session by session pointer (call from err callback
 *         where pcb is already freed but arg still holds the session)
 */
void firmware_upload_abort_session_ptr(void *session_ptr)
{
    if (active_upload != NULL && active_upload == (upload_session_t *)session_ptr) {
        vPortFree(active_upload);
        active_upload = NULL;
        fw_state.status = FW_STATUS_ERROR;
        strcpy(fw_state.error_message, "Connection aborted");
    }
}

/**
 * @brief  Parse Content-Length from HTTP header (safe, no strstr/atoi)
 */
static uint32_t parse_content_length(const char *header, uint16_t header_len)
{
    const char *pattern = "Content-Length:";
    const uint16_t pattern_len = 15;

    for (uint16_t i = 0; i < header_len - pattern_len; i++) {
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

/**
 * @brief  Find HTTP body start (after \r\n\r\n)
 */
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

/**
 * @brief  Write buffer to flash (handles page alignment)
 */
static W25Q128_Status_t flush_upload_buffer(upload_session_t *session)
{
    if (session->buffer_pos == 0)
        return W25Q128_OK;

    W25Q128_Status_t status = W25Q128_WritePage(
        session->flash_write_addr,
        session->buffer,
        session->buffer_pos
    );

    if (status == W25Q128_OK) {
        session->flash_write_addr += session->buffer_pos;
        session->buffer_pos = 0;
    }

    return status;
}

/**
 * @brief  Process received upload data into flash write buffer
 */
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
            if (flush_upload_buffer(session) != W25Q128_OK) {
                strcpy(fw_state.error_message, "Flash write error");
                fw_state.status = FW_STATUS_ERROR;
                return ERR_ABRT;
            }
        }
    }

    fw_state.bytes_transferred = session->bytes_received;
    return ERR_OK;
}

/**
 * @brief  Handle firmware upload (POST /api/firmware/upload)
 *
 * Called for both the first packet (session==NULL) and all subsequent
 * data packets (session attached to pcb via tcp_arg).
 * Caller must call pbuf_free(p) after this returns.
 * This function calls tcp_recved() internally on all paths.
 */
err_t firmware_upload_handler(struct tcp_pcb *pcb, struct pbuf *p)
{
    upload_session_t *session = active_upload;
    char *data = (char *)p->payload;
    uint16_t data_len = p->len;

    /* First packet - create session and parse headers */
    if (session == NULL) {
        session = (upload_session_t *)pvPortMalloc(sizeof(upload_session_t));
        if (session == NULL) {
            strcpy(fw_state.error_message, "Out of memory");
            fw_state.status = FW_STATUS_ERROR;
            tcp_recved(pcb, p->tot_len);
            tcp_close(pcb);
            return ERR_MEM;
        }

        memset(session, 0, sizeof(upload_session_t));
        session->pcb = pcb;
        session->flash_write_addr = FW_UPDATE_FLASH_ADDR;

        session->content_length = parse_content_length(data, data_len);
        if (session->content_length == 0 || session->content_length > FW_MAX_SIZE) {
            vPortFree(session);
            strcpy(fw_state.error_message, "Invalid content length");
            fw_state.status = FW_STATUS_ERROR;

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

        /* Erase flash sectors for the incoming firmware */
        uint32_t sectors_needed = (session->content_length + 4095) / 4096;
        for (uint32_t i = 0; i < sectors_needed; i++) {
            if (W25Q128_EraseSector(FW_UPDATE_FLASH_ADDR + (i * 4096)) != W25Q128_OK) {
                vPortFree(session);
                strcpy(fw_state.error_message, "Flash erase failed");
                fw_state.status = FW_STATUS_ERROR;

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

        fw_state.status = FW_STATUS_UPLOADING;
        fw_state.bytes_transferred = 0;
        fw_state.total_bytes = session->content_length;
        fw_state.flash_address = FW_UPDATE_FLASH_ADDR;

        active_upload = session;

        /* Attach session to pcb so subsequent data packets route here */
        tcp_arg(pcb, session);
    }

    /* Find body start (skip HTTP headers in first packet) */
    const char *body_start = data;
    uint16_t body_len = data_len;

    if (!session->header_parsed) {
        body_start = find_http_body(data, data_len);
        if (body_start == NULL) {
            /* Headers span multiple packets - wait for more data */
            tcp_recved(pcb, p->tot_len);
            return ERR_OK;
        }
        body_len = data_len - (uint16_t)(body_start - data);
        session->header_parsed = 1;
    }

    /* Process the firmware data */
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

    /* Check if upload complete */
    if (session->bytes_received >= session->content_length) {
        if (flush_upload_buffer(session) != W25Q128_OK) {
            strcpy(fw_state.error_message, "Final flash write failed");
            fw_state.status = FW_STATUS_ERROR;

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

        fw_state.status = FW_STATUS_UPLOAD_COMPLETE;
        fw_state.bytes_transferred = session->bytes_received;

        /* Detach session from pcb before freeing */
        tcp_arg(pcb, NULL);
        vPortFree(session);
        active_upload = NULL;

        /* Send success response and close connection */
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
        return ERR_OK;
    }

    /* More data expected - acknowledge this packet */
    tcp_recved(pcb, p->tot_len);
    return ERR_OK;
}

/* ---- Download with tcp_sent flow control ---- */

/**
 * @brief  Send next download chunk; called by tcp_sent callback and initially
 */
static err_t send_download_chunk(struct tcp_pcb *pcb)
{
    if (dl_ctx.pcb != pcb) return ERR_OK;  /* Stale, ignore */

    if (dl_ctx.bytes_sent >= dl_ctx.total_bytes) {
        fw_state.status = FW_STATUS_DOWNLOAD_READY;
        dl_ctx.pcb = NULL;
        tcp_close(pcb);
        return ERR_OK;
    }

    uint32_t remaining = dl_ctx.total_bytes - dl_ctx.bytes_sent;
    uint16_t sndbuf = tcp_sndbuf(pcb);
    uint32_t chunk = remaining < (uint32_t)sndbuf ? remaining : (uint32_t)sndbuf;
    if (chunk > DOWNLOAD_CHUNK_SIZE) chunk = DOWNLOAD_CHUNK_SIZE;

    if (chunk == 0) {
        /* Send buffer full - wait for tcp_sent callback */
        return ERR_OK;
    }

    uint8_t read_buf[DOWNLOAD_CHUNK_SIZE];
    if (W25Q128_Read(FW_UPDATE_FLASH_ADDR + dl_ctx.bytes_sent, read_buf, chunk) != W25Q128_OK) {
        strcpy(fw_state.error_message, "Flash read error");
        fw_state.status = FW_STATUS_ERROR;
        dl_ctx.pcb = NULL;
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    err_t err = tcp_write(pcb, read_buf, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        /* If buffer full, tcp_sent callback will retry */
        if (err == ERR_MEM) return ERR_OK;

        strcpy(fw_state.error_message, "TCP write error");
        fw_state.status = FW_STATUS_ERROR;
        dl_ctx.pcb = NULL;
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    dl_ctx.bytes_sent += chunk;
    fw_state.bytes_transferred = dl_ctx.bytes_sent;
    tcp_output(pcb);

    return ERR_OK;
}

/**
 * @brief  tcp_sent callback for download - sends next chunk when buffer drains
 */
static err_t firmware_download_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg;
    (void)len;
    return send_download_chunk(pcb);
}

/**
 * @brief  Handle firmware download (GET /api/firmware/download)
 *
 * Sets up tcp_sent callback for flow-controlled streaming.
 * The connection is closed by send_download_chunk when all data is sent.
 * Caller must NOT call tcp_close after this returns.
 */
err_t firmware_download_handler(struct tcp_pcb *pcb)
{
    if (fw_state.status != FW_STATUS_UPLOAD_COMPLETE &&
        fw_state.status != FW_STATUS_DOWNLOAD_READY) {
        const char *response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\nConnection: close\r\n\r\n"
            "No firmware available for download\r\n";
        tcp_write(pcb, response, strlen(response), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Initialize download context */
    dl_ctx.pcb = pcb;
    dl_ctx.bytes_sent = 0;
    dl_ctx.total_bytes = fw_state.total_bytes;

    fw_state.status = FW_STATUS_DOWNLOADING;
    fw_state.bytes_transferred = 0;

    /* Register sent callback for flow control */
    tcp_sent(pcb, firmware_download_sent);

    /* Send HTTP headers */
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
        fw_state.status = FW_STATUS_ERROR;
        strcpy(fw_state.error_message, "Header write error");
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    /* Kick off first chunk */
    return send_download_chunk(pcb);
}

/**
 * @brief  Handle firmware status query (GET /api/firmware/status)
 */
err_t firmware_status_handler(struct tcp_pcb *pcb)
{
    char response[512];
    const char *status_str;

    switch (fw_state.status) {
        case FW_STATUS_IDLE:            status_str = "idle"; break;
        case FW_STATUS_UPLOADING:       status_str = "uploading"; break;
        case FW_STATUS_UPLOAD_COMPLETE: status_str = "upload_complete"; break;
        case FW_STATUS_DOWNLOAD_READY:  status_str = "download_ready"; break;
        case FW_STATUS_DOWNLOADING:     status_str = "downloading"; break;
        case FW_STATUS_ERROR:           status_str = "error"; break;
        default:                        status_str = "unknown"; break;
    }

    uint32_t progress_pct = 0;
    if (fw_state.total_bytes > 0)
        progress_pct = (fw_state.bytes_transferred * 100) / fw_state.total_bytes;

    int len = snprintf(response, sizeof(response),
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

    return ERR_OK;
}

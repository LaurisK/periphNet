#include "image_transfer.h"
#include "w25q128.h"
#include "bl_app_contract.h"
#include "lwip/tcp.h"
#include "FreeRTOS.h"
#include <string.h>
#include <stdio.h>

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

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t bytes_sent;
    uint32_t total_bytes;
} download_ctx_t;

static download_ctx_t dl_ctx;

/**
 * @brief Initialize the image transfer module, freeing any active upload session.
 */
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
}

/**
 * @brief Return the current image transfer state.
 * @return Read-only pointer to the global transfer state.
 */
const image_state_t* image_transfer_get_status(void)
{
    return &fw_state;
}

/**
 * @brief Check whether an upload session is active on the given PCB.
 * @param pcb TCP PCB to check.
 * @return 1 if an upload session is active on pcb, 0 otherwise.
 */
int image_upload_has_active_session(struct tcp_pcb *pcb)
{
    return (active_upload != NULL && active_upload->pcb == pcb) ? 1 : 0;
}

/**
 * @brief Abort the upload session associated with a PCB while the PCB is still valid.
 * @param pcb TCP PCB whose session should be aborted.
 */
void image_upload_abort_session(struct tcp_pcb *pcb)
{
    if (active_upload != NULL && active_upload->pcb == pcb) {
        vPortFree(active_upload);
        active_upload = NULL;
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Connection aborted");
    }
}

/**
 * @brief Abort an upload session identified by its session pointer.
 * @param session_ptr Session pointer previously stored via tcp_arg; used when the
 *        PCB has already been freed by lwIP (e.g. from the err callback).
 */
void image_upload_abort_session_ptr(void *session_ptr)
{
    if (active_upload != NULL && active_upload == (upload_session_t *)session_ptr) {
        vPortFree(active_upload);
        active_upload = NULL;
        fw_state.status = IMG_STATUS_ERROR;
        strcpy(fw_state.error_message, "Connection aborted");
    }
}

/**
 * @brief Parse the Content-Length value from an HTTP header buffer.
 * @param header Pointer to the raw HTTP header bytes.
 * @param header_len Number of bytes in the header buffer.
 * @return Parsed content length, or 0 if the header is absent or malformed.
 */
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

/**
 * @brief Locate the start of the HTTP body by finding the \r\n\r\n delimiter.
 * @param data Pointer to the data buffer.
 * @param data_len Number of bytes in the buffer.
 * @return Pointer to the first byte after \r\n\r\n, or NULL if not found.
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
 * @brief Flush the session's write buffer to flash at the current write address.
 * @param session Active upload session whose buffer should be flushed.
 * @return W25Q128_OK on success, or a flash error code on failure.
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
 * @brief Accumulate received upload bytes into the page-aligned flash write buffer.
 * @param session Active upload session.
 * @param data Pointer to incoming data bytes.
 * @param len Number of bytes to process.
 * @return ERR_OK on success, ERR_ABRT if a flash write fails.
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
                fw_state.status = IMG_STATUS_ERROR;
                return ERR_ABRT;
            }
        }
    }

    fw_state.bytes_transferred = session->bytes_received;
    return ERR_OK;
}

/**
 * @brief Handle an HTTP firmware upload request (POST /api/firmware/upload).
 * @param pcb TCP PCB for the connection.
 * @param p Received pbuf chain; caller must call pbuf_free(p) after this returns.
 * @return ERR_OK while upload is in progress or on completion, or an lwIP error code on failure.
 * @note On the first call (no active session) headers are parsed, flash is erased,
 *       and the session is created. Subsequent calls receive body data.
 *       tcp_recved() is called internally on all paths.
 */
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

        fw_state.status = IMG_STATUS_UPLOADING;
        fw_state.bytes_transferred = 0;
        fw_state.total_bytes = session->content_length;
        fw_state.flash_address = IMG_UPDATE_FLASH_ADDR;

        active_upload = session;
        tcp_arg(pcb, session);
    }

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

    if (session->bytes_received >= session->content_length) {
        if (flush_upload_buffer(session) != W25Q128_OK) {
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

        fw_state.status = IMG_STATUS_UPLOAD_COMPLETE;
        fw_state.bytes_transferred = session->bytes_received;

        tcp_arg(pcb, NULL);
        vPortFree(session);
        active_upload = NULL;

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

    tcp_recved(pcb, p->tot_len);
    return ERR_OK;
}

/**
 * @brief Send the next chunk of download data, or close the connection when done.
 * @param pcb TCP PCB for the download connection.
 * @return ERR_OK on success or when waiting for buffer space, ERR_ABRT on flash error.
 */
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
        if (err == ERR_MEM) return ERR_OK;

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

/**
 * @brief tcp_sent callback that drives flow-controlled download chunk transmission.
 * @param arg Unused.
 * @param pcb TCP PCB for the download connection.
 * @param len Number of bytes acknowledged by the remote (unused).
 * @return Result of send_download_chunk().
 */
static err_t image_download_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg;
    (void)len;
    return send_download_chunk(pcb);
}

/**
 * @brief Handle a firmware download request (GET /api/firmware/download).
 * @param pcb TCP PCB for the connection.
 * @return ERR_OK on success, ERR_ABRT if the HTTP header cannot be written.
 * @note Responds with 404 if no completed upload is available.
 *       The connection is closed by send_download_chunk() after all data is sent;
 *       the caller must NOT call tcp_close() after this returns ERR_OK.
 */
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

    return send_download_chunk(pcb);
}

/**
 * @brief Handle a firmware status query (GET /api/firmware/status).
 * @param pcb TCP PCB for the connection.
 * @return ERR_OK always.
 */
err_t image_status_handler(struct tcp_pcb *pcb)
{
    char response[512];
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

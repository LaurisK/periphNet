#include "App/Http/http_server.h"
#include "App/Http/image_transfer.h"
#include "lwip/tcp.h"
#include "FreeRTOS.h"
#include <string.h>
#include <stdio.h>

static const char http_404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<html><body><h1>404 Not Found</h1></body></html>";

static void http_err_callback(void *arg, err_t err)
{
    (void)err;
    if (arg != NULL) {
        image_upload_abort_session_ptr(arg);
    }
}

static err_t http_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)err;

    if (p == NULL) {
        if (arg != NULL) {
            image_upload_abort_session(pcb);
        }
        tcp_close(pcb);
        return ERR_OK;
    }

    char *request = (char *)p->payload;

    /* Continuation of an active upload session */
    if (arg != NULL) {
        err_t result = image_upload_handler(pcb, p);
        pbuf_free(p);
        return result;
    }

    /* POST /api/firmware/upload */
    if (p->len >= 25 && strncmp(request, "POST /api/firmware/upload", 25) == 0) {
        err_t result = image_upload_handler(pcb, p);
        pbuf_free(p);
        return result;
    }

    /* GET /api/firmware/download */
    if (p->len >= 26 && strncmp(request, "GET /api/firmware/download", 26) == 0) {
        err_t result = image_download_handler(pcb);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return result;
    }

    /* POST /api/firmware/install */
    if (p->len >= 26 && strncmp(request, "POST /api/firmware/install", 26) == 0) {
        err_t result = image_install_handler(pcb);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return result;
    }

    /* GET /api/firmware/status */
    if (p->len >= 24 && strncmp(request, "GET /api/firmware/status", 24) == 0) {
        err_t result = image_status_handler(pcb);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        tcp_close(pcb);
        return result;
    }

    /* GET / — simple status page */
    if (p->len >= 6 && strncmp(request, "GET / ", 6) == 0) {
        const image_state_t *st = image_transfer_get_status();
        const char *status_str;
        switch (st->status) {
            case IMG_STATUS_IDLE:            status_str = "idle";            break;
            case IMG_STATUS_UPLOADING:       status_str = "uploading";      break;
            case IMG_STATUS_UPLOAD_COMPLETE: status_str = "upload_complete"; break;
            case IMG_STATUS_DOWNLOAD_READY:  status_str = "download_ready"; break;
            case IMG_STATUS_DOWNLOADING:     status_str = "downloading";    break;
            case IMG_STATUS_ERROR:           status_str = "error";          break;
            default:                         status_str = "unknown";        break;
        }

        char *buf = (char *)pvPortMalloc(512);
        if (buf == NULL) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
            tcp_close(pcb);
            return ERR_MEM;
        }

        int len = snprintf(buf, 512,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<html><body>"
            "<h1>PeriphNet</h1>"
            "<p>Status: <b>%s</b></p>"
            "<p>Transferred: %lu / %lu bytes</p>"
            "<hr>"
            "<p>POST /api/firmware/upload — upload binary</p>"
            "<p>GET /api/firmware/download — download stored image</p>"
            "<p>GET /api/firmware/status — JSON status</p>"
            "</body></html>",
            status_str,
            (unsigned long)st->bytes_transferred,
            (unsigned long)st->total_bytes);

        err_t werr = tcp_write(pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        if (werr == ERR_OK) {
            tcp_output(pcb);
            tcp_close(pcb);
        } else {
            tcp_abort(pcb);
        }
        vPortFree(buf);
        return (werr == ERR_OK) ? ERR_OK : ERR_ABRT;
    }

    /* Everything else → 404 */
    err_t werr = tcp_write(pcb, http_404, strlen(http_404), TCP_WRITE_FLAG_COPY);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (werr == ERR_OK) {
        tcp_output(pcb);
        tcp_close(pcb);
        return ERR_OK;
    } else {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
}

static err_t http_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }
    tcp_recv(newpcb, http_recv_callback);
    tcp_err(newpcb, http_err_callback);
    return ERR_OK;
}

void http_server_init(void)
{
    image_transfer_init();

    struct tcp_pcb *pcb = tcp_new();
    if (pcb != NULL) {
        err_t err = tcp_bind(pcb, IP_ADDR_ANY, HTTP_SERVER_PORT);
        if (err == ERR_OK) {
            pcb = tcp_listen(pcb);
            tcp_accept(pcb, http_accept_callback);
        } else {
            memp_free(MEMP_TCP_PCB, pcb);
        }
    }
}

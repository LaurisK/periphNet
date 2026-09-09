/*
 * HTTP server — dedicated FreeRTOS task using the lwIP netconn API.
 *
 * Connections are handled sequentially by one task, so plain synchronous
 * code replaces the old raw-callback state machines: headers are consumed
 * byte-wise from a small stream cursor (immune to TCP segmentation), the
 * upload body is streamed straight into the staging logic, and Trice
 * logging is allowed here (task context, not tcpip_thread).
 *
 * Domain logic is split between image_store.c (image management: upload/
 * download/delete of the stored blob) and fwu_control.c (FWU process:
 * install/confirm/verify/golden); this file owns all HTTP parsing and
 * response formatting.
 */

#include "App/Http/http_server.h"
#include "App/Http/web_ui.h"
#include "App/Img/image_store.h"
#include "json.h"
#include "nvdb.h"
#include "nvdb_config.h"
#include "nvdb_layout.h"
#include "App/Fwu/fwu_control.h"
#include "App/Log/crash.h"
#include "App/Log/trice_udp.h"
#include "App/Log/trice_consumer.h"
#include "App/Can/can_bridge.h"
#include "App/Can/can_log.h"
#include "App/Can/can_bus.h"
#include "App/Can/can_monitor.h"
#include "App/Gw/modbus_tcp.h"
#include "App/Mon/sysmon.h"
#include "usart.h"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include "App/Modbus/modbus.h"
#include "App/Modbus/modbus_trice_sink.h"
#include "App/Cluster/cluster.h"
#include "App/Pack/pack.h"
#include "App/Net/wg_link.h"
#include "App/Net/wg_platform.h"
#include "App/Net/wg_time.h"
#include "App/system.h"
#include "bl_app_contract.h"
#include "version.h"
#include "boot_status.h"
/* The Modbus module is reached through its ONE public header: a consumer may
 * name Shared/Modbus TYPES but must not call its flash accessors (§2.2). */
#include "lwip/api.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "trice.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define HTTP_IO_TIMEOUT_MS   10000
#define REQ_BUF_SIZE         1024
#define DOWNLOAD_CHUNK_SIZE  512

/* CPU-only buffers → CCM RAM (never handed to DMA: netconn_write here
 * always uses NETCONN_COPY, so lwIP copies into SRAM pbufs) */
#define CCMRAM_BSS __attribute__((section(".ccmram")))

/* Largest body still considered as a possible WireGuard .conf.  A real one is
 * ~300 B; a .pnfw is ~316 KB, so there is no ambiguity to resolve by size. */
#define WG_CONF_UPLOAD_MAX  4096u

static void wg_status_json(char *buf, size_t sz);

/* Single-task server: static buffers are safe and cheap */
static char req_buf[REQ_BUF_SIZE] CCMRAM_BSS;
static char resp_buf[1280] CCMRAM_BSS;

/* Body cap for the handlers that build resp_buf incrementally: appends stop
 * here so the closing fragment, which is appended against the full size,
 * always fits.  Truncated content is acceptable; unparseable content is not.
 * The reserve covers the longest tail any of them appends -- the Trice
 * destination replies' ",\"port\":65535}" at 14 bytes, not just "]}". */
#define RESP_BODY_CAP       (sizeof(resp_buf) - 24u)

/* Escaped worst case is six bytes out per byte in (\u00XX).  These names come
 * from an uploaded config and are re-emitted on every status response, so
 * they are escaped rather than trusted (docs/task_json_module.md §1.3). */
#define MB_POINT_NAME_ESC_LEN   (((MB_POINT_NAME_LEN - 1u) * 6u) + 1u)
#define MB_PREFIX_ESC_LEN       (((MB_NAME_LEN - 1u) * 6u) + 1u)
#define MB_CFG_ERR_ESC_LEN      ((sizeof(((sModbusCompileResult *)0)->field) - 1u) * 6u + 1u)

/* One size for every short, document-derived key or name echoed back in an
 * error.  The longest of them is nvDb's 48-byte field; six bytes out per
 * byte in is the \u00XX worst case. */
#define ESC_FIELD_LEN           (((48u - 1u) * 6u) + 1u)

/* THE FALLBACK PAGE, and deliberately nothing more.
 *
 * The real UI lives in external flash (App/Http/web_ui.c) because it had grown
 * to 15.6 KB of an image that is 99.65 % full.  What stays here is the answer
 * for a board that has never been given one -- a factory board, or one whose
 * page was erased -- so `GET /` is never a blank screen with no explanation.
 *
 * It is plain text in one literal on purpose: every byte here is image, and
 * the whole reason the page moved out was that those bytes are scarce.  A
 * board in this state is FULLY OPERABLE -- every /api/ route is in the image --
 * so this needs to say only that, and how to install the page. */
static const char fallback_html[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>PeriphNet</title><style>"
    "body{font-family:system-ui,sans-serif;margin:2em auto;max-width:40em;"
    "line-height:1.5;color:#222}code{background:#f2f2f2;padding:.1em .3em}"
    "pre{background:#f2f2f2;padding:1em;overflow-x:auto}</style></head><body>"
    "<h1>PeriphNet</h1>"
    "<p>The board is running. <b>No web interface is installed.</b></p>"
    "<p>The interface is stored in external flash and shipped separately from "
    "the firmware, so it is uploaded once per board:</p>"
    "<pre>curl -X POST --data-binary @periphnet_ui.pnui \\\n"
    "     http://HOST/api/ui</pre>"
    "<p><code>periphnet_ui.pnui</code> is produced by the build, next to "
    "<code>periphnet_fwu.pnfw</code>. It does not need a firmware update, and "
    "a firmware update does not replace it.</p>"
    "<p>Everything else already works without it &mdash; the whole "
    "<code>/api/</code> surface is in the firmware. For example "
    "<code>/api/system/status</code>, <code>/api/pack/status</code>, "
    "<code>/api/cluster/status</code>.</p>"
    "</body></html>";

/* --------------------------------------------------------------------------
 * Connection byte stream — hides netbuf/part boundaries from the parser
 * -------------------------------------------------------------------------- */

typedef struct {
    struct netconn *conn;
    struct netbuf  *nb;      /* current netbuf, NULL when exhausted */
    void           *data;    /* current part                        */
    u16_t           len;
    u16_t           off;
} sConnStream;

static void cs_init(sConnStream *s, struct netconn *conn)
{
    memset(s, 0, sizeof(*s));
    s->conn = conn;
}

static void cs_cleanup(sConnStream *s)
{
    if (s->nb != NULL) {
        netbuf_delete(s->nb);
        s->nb = NULL;
    }
}

/** Ensure the current part has unread bytes; pulls the next part/netbuf
 *  as needed.  Returns ERR_OK or the netconn error (incl. timeout). */
static err_t cs_fill(sConnStream *s)
{
    while (s->nb == NULL || s->off >= s->len) {
        if (s->nb != NULL) {
            if (netbuf_next(s->nb) >= 0) {
                netbuf_data(s->nb, &s->data, &s->len);
                s->off = 0;
                continue;
            }
            netbuf_delete(s->nb);
            s->nb = NULL;
        }

        err_t err = netconn_recv(s->conn, &s->nb);
        if (err != ERR_OK) {
            s->nb = NULL;
            return err;
        }
        netbuf_data(s->nb, &s->data, &s->len);
        s->off = 0;
    }
    return ERR_OK;
}

static int cs_read_byte(sConnStream *s)
{
    if (cs_fill(s) != ERR_OK) {
        return -1;
    }
    return ((uint8_t *)s->data)[s->off++];
}

/* --------------------------------------------------------------------------
 * Response helpers
 * -------------------------------------------------------------------------- */

/** Write the whole buffer, looping over partial writes.  With a send
 *  timeout set, lwIP treats writes as non-blocking and plain
 *  netconn_write() (NULL bytes_written) is rejected with ERR_VAL, so
 *  netconn_write_partly() is mandatory here. */
static bool send_all(struct netconn *conn, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len > 0U) {
        size_t written = 0U;
        err_t  err = netconn_write_partly(conn, p, len, NETCONN_COPY, &written);
        if (err != ERR_OK) {
            TRice("HTTP: write failed, err=%d\n", (int)err);
            return false;
        }
        if (written == 0U) {          /* no progress within send timeout */
            TRice("HTTP: write stalled\n");
            return false;
        }
        p   += written;
        len -= written;
    }
    return true;
}

static void send_body(struct netconn *conn, const char *status,
                      const char *content_type, const char *body)
{
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        status, content_type, (unsigned)strlen(body));
    send_all(conn, hdr, hlen);
    send_all(conn, body, strlen(body));
}

static void send_json(struct netconn *conn, const char *status, const char *json)
{
    send_body(conn, status, "application/json", json);
}

/* --------------------------------------------------------------------------
 * Header parsing
 * -------------------------------------------------------------------------- */

static uint32_t parse_content_length(const char *header)
{
    const char *pattern = "content-length:";
    for (const char *p = header; *p; p++) {
        const char *h = p, *n = pattern;
        while (*n && *h) {
            char c = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
            if (c != *n) break;
            h++; n++;
        }
        if (*n == '\0') {
            while (*h == ' ' || *h == '\t') h++;
            uint32_t value = 0;
            while (*h >= '0' && *h <= '9')
                value = value * 10 + (uint32_t)(*h++ - '0');
            return value;
        }
    }
    return 0;
}

static bool header_expects_continue(const char *header)
{
    const char *pattern = "100-continue";
    for (const char *p = header; *p; p++) {
        const char *h = p, *n = pattern;
        while (*n && *h) {
            char c = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
            if (c != *n) break;
            h++; n++;
        }
        if (*n == '\0') return true;
    }
    return false;
}

/** Copy the value of a header (name given lowercase incl. ':') into out.
 *  @return true if the header was found. */
static bool header_value(const char *header, const char *name,
                         char *out, size_t out_size)
{
    for (const char *p = header; *p; p++) {
        const char *h = p, *n = name;
        while (*n && *h) {
            char c = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
            if (c != *n) break;
            h++; n++;
        }
        if (*n == '\0') {
            while (*h == ' ' || *h == '\t') h++;
            size_t i = 0;
            while (*h && *h != '\r' && *h != '\n' && i < out_size - 1) {
                out[i++] = *h++;
            }
            while (i > 0 && (out[i-1] == ' ' || out[i-1] == '\t')) i--;
            out[i] = '\0';
            return true;
        }
    }
    return false;
}

/** Reduce a client-supplied file name to its basename with a safe
 *  character set (JSON/header friendly). */
static void sanitize_filename(const char *in, char *out, size_t out_size)
{
    const char *base = in;
    for (const char *p = in; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    size_t i = 0;
    for (const char *p = base; *p && i < out_size - 1; p++) {
        char c = *p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-' || c == '+';
        out[i++] = safe ? c : '_';
    }
    out[i] = '\0';
}

/** Read the request header (through \r\n\r\n) into req_buf.
 *  @return header length, or -1 on stream error / oversized header. */
static int read_request_header(sConnStream *s)
{
    int len = 0;

    while (len < REQ_BUF_SIZE - 1) {
        int c = cs_read_byte(s);
        if (c < 0) {
            return -1;
        }
        req_buf[len++] = (char)c;

        if (len >= 4 &&
            req_buf[len-4] == '\r' && req_buf[len-3] == '\n' &&
            req_buf[len-2] == '\r' && req_buf[len-1] == '\n') {
            req_buf[len] = '\0';
            return len;
        }
    }

    return -1;   /* header too large */
}

/* --------------------------------------------------------------------------
 * Endpoint handlers
 * -------------------------------------------------------------------------- */

/* Consume the body into the WireGuard .conf parser.  Nothing is written to
 * flash until the whole file has parsed, so a truncated upload cannot leave
 * the board holding half an identity. */
static void handle_wg_conf_upload(struct netconn *conn, sConnStream *s,
                                  uint32_t content_length)
{
    static sWgConfParser parser CCMRAM_BSS;   /* ~200 B, too big for the stack */
    const char *err = "malformed configuration";
    uint32_t    remaining = content_length;

    WgConf_Begin(&parser);

    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    while (remaining > 0u) {
        uint32_t n;

        if (cs_fill(s) != ERR_OK) {
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"connection lost during upload\"}");
            return;
        }
        n = (uint32_t)(s->len - s->off);
        if (n > remaining) {
            n = remaining;
        }
        WgConf_Feed(&parser, (const uint8_t *)s->data + s->off, n);
        s->off    += (u16_t)n;
        remaining -= n;
    }

    if (WgConf_Finish(&parser, &err) != 0) {
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"%s\",\"line\":%u}",
                 err, (unsigned)WgConf_ErrorLine(&parser));
        send_json(conn, "422 Unprocessable Entity", resp_buf);
        TRiceS("WG conf: rejected — %s\n", (char *)err);
        return;
    }

    /* Applying restarts the tunnel, so a response sent afterwards may not
     * reach a caller who came in over the tunnel itself. */
    if (WgLink_ApplyConf(WgConf_Result(&parser), 1) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"could not store or start the tunnel\"}");
        return;
    }

    TRice("WG conf: applied and stored\n");
    wg_status_json(resp_buf, sizeof(resp_buf));
    send_json(conn, "200 OK", resp_buf);
}

/* One upload endpoint, two kinds of file.  The type is decided by content,
 * not by the filename: a .pnfw always starts with its cleartext manifest
 * magic, and is three orders of magnitude larger than any .conf. */
static int body_is_fwu_blob(sConnStream *s)
{
    /* A short first segment is legal, so keep filling until the magic is
     * either present or ruled out. */
    while ((uint32_t)(s->len - s->off) < 4u) {
        if (cs_fill(s) != ERR_OK) {
            return 0;
        }
        if ((uint32_t)(s->len - s->off) >= 4u) {
            break;
        }
    }
    if ((uint32_t)(s->len - s->off) < 4u) {
        return 0;
    }
    /* FWU_BLOB_MAGIC as it appears on the wire (little-endian "PNFW"). */
    return (memcmp(s->data + s->off, "PNFW", 4u) == 0) ? 1 : 0;
}

static void handle_image_upload(struct netconn *conn, sConnStream *s)
{
    uint32_t content_length = parse_content_length(req_buf);

    char raw_name[IMG_STORE_NAME_MAX], name[IMG_STORE_NAME_MAX] = "";
    if (header_value(req_buf, "x-filename:", raw_name, sizeof(raw_name))) {
        sanitize_filename(raw_name, name, sizeof(name));
    }

    /* Peeking costs nothing: cs_fill() buffers without consuming, so the
     * bytes examined here are still delivered to whichever path wins. */
    if (content_length > 0u && content_length <= WG_CONF_UPLOAD_MAX &&
        !body_is_fwu_blob(s)) {
        handle_wg_conf_upload(conn, s, content_length);
        return;
    }

    const char *err;
    if (!ImgStore_UploadBegin(content_length, name, &err)) {
        snprintf(resp_buf, sizeof(resp_buf), "{\"error\":\"%s\"}", err);
        send_json(conn, "409 Conflict", resp_buf);
        return;
    }

    /* curl sends Expect: 100-continue for large bodies and stalls ~1s
     * waiting for the go-ahead */
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    uint32_t remaining = content_length;
    while (remaining > 0) {
        if (cs_fill(s) != ERR_OK) {
            ImgStore_UploadAbort("Connection lost");
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"connection lost during upload\"}");
            return;
        }

        uint32_t n = (uint32_t)(s->len - s->off);
        if (n > remaining) n = remaining;

        if (!ImgStore_UploadWrite((uint8_t *)s->data + s->off, n)) {
            send_json(conn, "500 Internal Server Error",
                      "{\"error\":\"flash write error\"}");
            return;
        }
        s->off += (u16_t)n;
        remaining -= n;
    }

    const sImageStoreState *st = ImgStore_GetState();
    if (ImgStore_UploadFinish()) {
        TRice("IMG: stored %u B\n", (unsigned)st->blob.blob_size);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\":\"stored\",\"name\":\"%s\","
                 "\"size\":%lu,\"version\":\"%s\"}",
                 st->name, (unsigned long)st->blob.blob_size,
                 st->blob.version_str);
        send_json(conn, "200 OK", resp_buf);
    } else {
        snprintf(resp_buf, sizeof(resp_buf), "{\"error\":\"%s\"}",
                 st->error_message);
        send_json(conn, "422 Unprocessable Entity", resp_buf);
    }
}

static void handle_image_download(struct netconn *conn)
{
    const sImageStoreState *st = ImgStore_GetState();

    if (st->status != imgStore_ready || !st->blob.valid) {
        send_body(conn, "404 Not Found", "text/plain",
                  "No image available for download\r\n");
        return;
    }

    ImgStore_DownloadBegin();

    uint32_t total = st->blob.blob_size;
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lu\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n\r\n",
        (unsigned long)total,
        st->name[0] != '\0' ? st->name : "image.pnfw");
    send_all(conn, hdr, hlen);

    uint8_t buf[DOWNLOAD_CHUNK_SIZE];
    for (uint32_t off = 0; off < total; off += DOWNLOAD_CHUNK_SIZE) {
        uint32_t n = total - off;
        if (n > DOWNLOAD_CHUNK_SIZE) n = DOWNLOAD_CHUNK_SIZE;

        if (!ImgStore_Read(off, buf, n)) {
            ImgStore_DownloadEnd(false, "Flash read error");
            return;
        }
        if (!send_all(conn, buf, n)) {
            ImgStore_DownloadEnd(false, "TCP write error");
            return;
        }
    }

    ImgStore_DownloadEnd(true, NULL);
}

static void handle_image_info(struct netconn *conn)
{
    const sImageStoreState *st = ImgStore_GetState();

    const char *status_str;
    switch (st->status) {
        case imgStore_empty:       status_str = "empty";       break;
        case imgStore_uploading:   status_str = "uploading";   break;
        case imgStore_ready:       status_str = "ready";       break;
        case imgStore_downloading: status_str = "downloading"; break;
        case imgStore_error:       status_str = "error";       break;
        default:                    status_str = "unknown";     break;
    }

    uint32_t progress_pct = 0;
    if (st->total_bytes > 0)
        progress_pct = (st->bytes_transferred * 100) / st->total_bytes;

    char version_field[32];
    if (st->blob.valid) {
        snprintf(version_field, sizeof(version_field), "\"%s\"",
                 st->blob.version_str);
    } else {
        strcpy(version_field, "null");
    }

    snprintf(resp_buf, sizeof(resp_buf),
        "{\"status\":\"%s\","
        "\"present\":%s,"
        "\"name\":\"%s\","
        "\"version\":%s,"
        "\"size\":%lu,"
        "\"image_size\":%lu,"
        "\"crc32\":\"0x%08lX\","
        "\"bytes_transferred\":%lu,"
        "\"total_bytes\":%lu,"
        "\"progress\":%lu,"
        "\"error\":\"%s\"}",
        status_str,
        st->blob.valid ? "true" : "false",
        st->name,
        version_field,
        (unsigned long)st->blob.blob_size,
        (unsigned long)st->blob.image_size,
        (unsigned long)st->blob.blob_crc32,
        (unsigned long)st->bytes_transferred,
        (unsigned long)st->total_bytes,
        (unsigned long)progress_pct,
        st->error_message);

    send_json(conn, "200 OK", resp_buf);
}

static void handle_image_delete(struct netconn *conn)
{
    switch (ImgStore_Delete()) {
    case imgRes_ok:
        send_json(conn, "200 OK", "{\"status\":\"deleted\"}");
        break;
    case imgRes_busy:
        send_json(conn, "409 Conflict",
                  "{\"error\":\"image in use (transfer or FWU)\"}");
        break;
    default:
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"flash erase failed\"}");
        break;
    }
}

static void handle_fwu_status(struct netconn *conn)
{
    char running_ver[24] = {0};
    const sAppInfo *app = (const sAppInfo *)APP_INFO_HEADER_ADDR;
    if (app->magic == APP_INFO_MAGIC) {
        ver_toString(&app->fw_version.ver, running_ver, sizeof(running_ver));
    }

    sBootStatus bs;
    bool     bs_ok       = (BootStatus_Read(&bs) == 0);
    bool     unconfirmed = BootStatus_IsUnconfirmed();
    uint8_t  attempts    = BootStatus_AttemptsRemaining();
    uint32_t last_result = bs_ok ? bs.last_fwu_result : (uint32_t)fwuRes_noResult;

    const sBlobInfo *golden = FwuCtl_GetGolden();
    char golden_field[32];
    if (golden->valid) {
        snprintf(golden_field, sizeof(golden_field), "\"%s\"",
                 golden->version_str);
    } else {
        strcpy(golden_field, "null");
    }

    uint32_t uptime = HAL_GetTick() / 1000U;

    /* `confirm_guard` is what tells an operator the board will recover on its
     * own: `remaining_sec` counting down means a lost tunnel ends in a
     * reboot, and `attempts_remaining` says how many are left before the
     * bootloader puts the golden image back. */
    sFwuConfirmGuard g;
    FwuCtl_GetConfirmGuard(&g);

    snprintf(resp_buf, sizeof(resp_buf),
        "{\"running_version\":\"%s\","
        "\"confirmed\":%s,"
        "\"attempts_remaining\":%u,"
        "\"last_fwu_result\":%lu,"
        "\"uptime\":%lu,"
        "\"golden_version\":%s,"
        "\"promote_pending\":%s,"
        "\"confirm_guard\":{\"armed\":%s,\"exempt\":%s,"
        "\"window_sec\":%lu,\"remaining_sec\":%lu,\"kicks\":%lu},"
        "\"reset_cause\":\"0x%08lX\"}",
        running_ver,
        unconfirmed ? "false" : "true",
        (unsigned)attempts,
        (unsigned long)last_result,
        (unsigned long)uptime,
        golden_field,
        FwuCtl_PromotePending() ? "true" : "false",
        g.armed ? "true" : "false",
        g.exempt ? "true" : "false",
        (unsigned long)g.window_sec,
        (unsigned long)g.remaining_sec,
        (unsigned long)g.kickCnt,
        (unsigned long)System_GetResetCause());

    send_json(conn, "200 OK", resp_buf);
}

/* ==========================================================================
 * nvDb endpoints under /api/nvdb — the operator surface of the store
 * ==========================================================================
 * An nvDb USER never asks any of this.  The person who loaded a layout and
 * rebooted the board asks it, because loading can fail even when validation
 * passed (docs/task_nv_db.md §2.6).
 * ========================================================================== */

/* GET /api/nvdb/layout — the layout in force, free space, and anything that
 * has come aboard but not been applied. */
static void handle_nvdb_layout_get(struct netconn *conn)
{
    sNvDbLayoutInfo info;
    sNvDbStatus     status;

    if (NvDb_GetLayout(&info) != nvdbRes_ok ||
        NvDb_GetStatus(&status) != nvdbRes_ok) {
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"nvDb is not initialised\"}");
        return;
    }
    if (NvDbCfg_Render(&info, &status, resp_buf, sizeof(resp_buf)) == 0u) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"layout does not fit the response buffer\"}");
        return;
    }
    send_json(conn, "200 OK", resp_buf);
}

/* POST /api/nvdb/layout — take a layout aboard, to be applied at the NEXT
 * boot.  Nothing moves now: a layout is loaded, validated and applied at
 * NvDb_Init() and only there, which is what makes the outcome something the
 * status has to be asked about afterwards. */
static void handle_nvdb_layout_post(struct netconn *conn, sConnStream *s)
{
    uint32_t       content_length = parse_content_length(req_buf);
    /* Main SRAM, not CCM: CCM is the constrained region here and a layout
     * document is not hot.  The parser wants the whole document, so it has to
     * land somewhere. */
    static char    body[1024];
    sNvDbLayoutCfg cfg;
    sNvDbCfgError  err;
    uint32_t       got = 0u;
    eNvDbRes       res;

    if (content_length == 0u || content_length >= sizeof(body)) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"JSON body required (max 1023 bytes)\"}");
        return;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    while (got < content_length) {
        int ch = cs_read_byte(s);
        if (ch < 0) {
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"incomplete body\"}");
            return;
        }
        body[got++] = (char)ch;
    }
    body[got] = '\0';

    if (NvDbCfg_Parse(body, got, &cfg, &err) != 0) {
        char fieldEsc[ESC_FIELD_LEN];

        /* `field` is a key copied out of the uploaded document; `reason` is
         * always one of this code's own literals. */
        (void)Json_Escape(fieldEsc, sizeof(fieldEsc), err.field);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"%s\",\"field\":\"%s\",\"offset\":%u}",
                 err.reason, fieldEsc, (unsigned)err.offset_bytes);
        send_json(conn, "422 Unprocessable Entity", resp_buf);
        return;
    }

    res = NvDb_SupplyLayout(&cfg);
    if (res == nvdbRes_refused) {
        /* The ADVISORY check said no.  Nothing was written, and the layout in
         * force is untouched — refusal is never a route to data loss. */
        send_json(conn, "409 Conflict",
                  "{\"error\":\"layout refused: it does not fit, it would "
                  "truncate a user, or it misplaces nvDb's own areas\"}");
        return;
    }
    if (res != nvdbRes_ok) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"could not store the layout\"}");
        return;
    }

    {
        char nameEsc[ESC_FIELD_LEN];

        (void)Json_Escape(nameEsc, sizeof(nameEsc), cfg.name);
        TRiceS("nvDb: layout '%s' taken aboard, applies at next boot\n",
               cfg.name);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\":\"onboard\",\"name\":\"%s\",\"version\":%u,"
                 "\"operation\":\"%s\","
                 "\"note\":\"applied at the next boot; check "
                 "lastApplyResult afterwards\"}",
                 nameEsc, (unsigned)cfg.version,
                 NvDbCfg_ModeName(cfg.operation));
    }
    send_json(conn, "202 Accepted", resp_buf);
}

/* DELETE /api/nvdb/layout — discard a layout that came aboard but has not
 * been applied.  It never touches the layout in force. */
static void handle_nvdb_layout_delete(struct netconn *conn)
{
    if (NvDb_DropSuppliedLayout() != nvdbRes_ok) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"could not drop the staged layout\"}");
        return;
    }
    TRice("nvDb: staged layout dropped\n");
    send_json(conn, "200 OK", "{\"status\":\"dropped\"}");
}

/* GET /api/nvdb/usage[?scan=1] — occupancy and wear.
 *
 * `scan` observes occupancy, which reads every area — for the two 488 KB blob
 * areas that is nearly a megabyte of SPI, so it is off by default and the
 * reply says which it was. */
#define NVDB_USAGE_JSON_CAP  2048u

/* Is `key` present in the query string of the request LINE?
 *
 * Deliberately not a search of req_buf: that holds the headers too, and a
 * client with the word in a User-Agent would otherwise trigger a scan of the
 * whole medium. */
/** `?key=N` from the request line, or @p dflt when absent or malformed.
 *  Bounded to the request line like query_has, so a header cannot supply it. */
static int query_int(const char *key, int dflt)
{
    const char *eol = strpbrk(req_buf, "\r\n");
    const char *q   = strchr(req_buf, '?');
    const char *hit;

    if ((q == NULL) || ((eol != NULL) && (q > eol))) {
        return dflt;
    }
    hit = strstr(q, key);
    if ((hit == NULL) || ((eol != NULL) && (hit >= eol))) {
        return dflt;
    }
    hit += strlen(key);
    if (*hit != '=') {
        return dflt;
    }
    hit++;
    if ((*hit < '0') || (*hit > '9')) {
        return dflt;
    }
    return atoi(hit);
}

static bool query_has(const char *key)
{
    const char *eol = strpbrk(req_buf, "\r\n");
    const char *q   = strchr(req_buf, '?');
    const char *hit;

    if (q == NULL || (eol != NULL && q > eol)) {
        return false;
    }
    hit = strstr(q, key);
    return (hit != NULL) && (eol == NULL || hit < eol);
}

/** `?key=<token>` from the request line, copied out NUL-terminated.
 *  Bounded to the request line for the same reason query_int is: a header
 *  must not be able to supply a parameter.
 *  @retval the token length, or negative when the key is absent. */
static int query_token(const char *key, char *out, size_t len)
{
    const char *eol = strpbrk(req_buf, "\r\n");
    const char *q   = strchr(req_buf, '?');
    const char *hit;
    size_t      n = 0u;

    if ((out == NULL) || (len == 0u) || (q == NULL) ||
        ((eol != NULL) && (q > eol))) {
        return -1;
    }
    hit = strstr(q, key);
    if ((hit == NULL) || ((eol != NULL) && (hit >= eol))) {
        return -1;
    }
    hit += strlen(key);
    if (*hit != '=') {
        return -1;
    }
    hit++;
    while ((*hit != '\0') && (*hit != '&') && (*hit != ' ') &&
           (*hit != '\r') && (*hit != '\n') && (n < (len - 1u))) {
        out[n] = *hit;
        n++;
        hit++;
    }
    out[n] = '\0';
    return (int)n;
}

static void handle_nvdb_usage(struct netconn *conn)
{
    bool  scan = query_has("scan=1");
    char *js;

    /* One object per user does not fit resp_buf, and CCM has nothing to
     * spare — the same reason the system-status handler builds its JSON in a
     * transient block. */
    js = (char *)pvPortMalloc(NVDB_USAGE_JSON_CAP);
    if (js == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }

    if (NvDbCfg_RenderUsage(scan, js, NVDB_USAGE_JSON_CAP) == 0u) {
        vPortFree(js);
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"nvDb is not initialised, or the report does "
                  "not fit\"}");
        return;
    }
    send_json(conn, "200 OK", js);
    vPortFree(js);
}

static void handle_fwu_install(struct netconn *conn)
{
    switch (FwuCtl_RequestInstall()) {
    case fwuCtlRes_ok: {
        const sImageStoreState *st = ImgStore_GetState();
        TRice("FWU: install requested, rebooting\n");
        snprintf(resp_buf, sizeof(resp_buf),
            "{\"status\":\"deploying\","
            "\"message\":\"Device will reboot in 2 seconds\","
            "\"version\":\"%s\"}", st->blob.version_str);
        send_json(conn, "200 OK", resp_buf);
        break;
    }
    case fwuCtlRes_noImage:
        send_json(conn, "409 Conflict",
                  "{\"error\":\"no image available\"}");
        break;
    case fwuCtlRes_busy:
        send_json(conn, "409 Conflict",
                  "{\"error\":\"golden promotion in progress\"}");
        break;
    case fwuCtlRes_blContract:
        /* The layout moved a blob out from under a bootloader that has no
         * way to be told.  Installing would brick the board, so it does not
         * happen (docs/task_nv_db.md §6). */
        send_json(conn, "409 Conflict",
                  "{\"error\":\"storage layout no longer matches the "
                  "bootloader\"}");
        break;
    default:
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"failed to write boot status\"}");
        break;
    }
}

static void handle_fwu_confirm(struct netconn *conn)
{
    bool promote;
    switch (FwuCtl_Confirm(&promote)) {
    case fwuCtlRes_ok:
        TRice("FWU: confirmed (promote=%d)\n", (int)promote);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\":\"confirmed\",\"promote\":%s}",
                 promote ? "true" : "false");
        send_json(conn, "200 OK", resp_buf);
        break;
    case fwuCtlRes_already:
        send_json(conn, "200 OK", "{\"status\":\"already_confirmed\"}");
        break;
    default:
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"failed to write boot status\"}");
        break;
    }
}

/* POST /api/fwu/kick[?window_sec=N] — reload the confirmation countdown.
 *
 * THE PROCEDURE this exists for: install, wait for the board to come back,
 * then kick while you check the new image over, and confirm when satisfied.
 * Stop kicking and the board reboots itself; do that three times and the
 * bootloader puts the golden image back.  An updater that loses its tunnel
 * to a bad build therefore recovers the board by doing nothing at all. */
static void handle_fwu_kick(struct netconn *conn)
{
    const int        want = query_int("window_sec", 0);
    sFwuConfirmGuard g;

    if (FwuCtl_KickConfirm((want > 0) ? (uint32_t)want : 0U) !=
        fwuCtlRes_ok) {
        FwuCtl_GetConfirmGuard(&g);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"nothing is counting down\",\"exempt\":%s,"
                 "\"confirmed\":%s}",
                 g.exempt ? "true" : "false",
                 BootStatus_IsUnconfirmed() ? "false" : "true");
        send_json(conn, "409 Conflict", resp_buf);
        return;
    }

    FwuCtl_GetConfirmGuard(&g);
    TRice("FWU: kicked, %u s\n", (unsigned)g.window_sec);
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"status\":\"kicked\",\"window_sec\":%lu,"
             "\"remaining_sec\":%lu,\"kicks\":%lu,"
             "\"attempts_remaining\":%u}",
             (unsigned long)g.window_sec, (unsigned long)g.remaining_sec,
             (unsigned long)g.kickCnt, (unsigned)g.attemptsLeft);
    send_json(conn, "200 OK", resp_buf);
}

static void handle_fwu_verify(struct netconn *conn)
{
    eFwuRes res = FwuCtl_VerifyRunning();

    const char *reason = NULL;
    switch (res) {
        case fwuRes_ok:             break;
        case fwuRes_errWrongMagic: reason = "no app header";      break;
        case fwuRes_errImageSize:  reason = "unsigned image";     break;
        case fwuRes_errNoImage:    reason = "BL API unavailable"; break;
        default:                  reason = "HMAC mismatch";      break;
    }

    if (reason) {
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"verified\":false,\"reason\":\"%s\",\"code\":%d}",
                 reason, (int)res);
    } else {
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"verified\":true,\"code\":0}");
    }
    send_json(conn, "200 OK", resp_buf);
}

/* --------------------------------------------------------------------------
 * Crash log endpoints
 * -------------------------------------------------------------------------- */

/* The crash report outgrew resp_buf once CRASH_LOG_MAX_TASKS went to 16 (~1.6 kB
 * of JSON), so it is built in a transient heap block instead of enlarging a CCM
 * buffer -- the same trade /api/system/status makes, and CCM is the scarce
 * region here.
 *
 * Every append in this file goes through Json_Cat() (Shared/Json).  The idiom
 * it replaced accumulated snprintf()'s return, which is the length it WOULD
 * have written: past the end that makes `pos` exceed the buffer and
 * `cap - pos` underflow to a huge size_t, i.e. the first truncated field
 * turned into an overflowing write.  Json_Cat() saturates at `cap` instead,
 * and `pos == cap` is the "it did not fit" signal a loop rolls back on. */
#define CRASH_JSON_CAP  2560u

static void handle_crash_get(struct netconn *conn)
{
    sCrashLog *log = (sCrashLog *)pvPortMalloc(sizeof(sCrashLog));
    char      *js;
    size_t     pos = 0u;

    if (log == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }

    if (!Crash_ReadFromFlash(log)) {
        vPortFree(log);
        send_json(conn, "200 OK", "{\"valid\":false}");
        return;
    }

    js = (char *)pvPortMalloc(CRASH_JSON_CAP);
    if (js == NULL) {
        vPortFree(log);
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }

    /* The names belong to the enum, not to this adapter: Crash_TypeName()
     * is range-checked against crashType_last, which the private table here
     * was not -- its hand-maintained bound had already fallen two enumerators
     * behind and reported StackOverflow/Assert as "Unknown". */
    const char *type_str = Crash_TypeName(log->crash_type);

    pos = Json_Cat(js, CRASH_JSON_CAP, pos,
        "{\"valid\":true,\"type\":\"%s\",\"tick\":%lu,"
        "\"pc\":\"%08lX\",\"lr\":\"%08lX\",\"sp\":\"%08lX\","
        "\"r0\":\"%08lX\",\"r12\":\"%08lX\",\"psr\":\"%08lX\","
        "\"cfsr\":\"%08lX\",\"hfsr\":\"%08lX\","
        "\"mmfar\":\"%08lX\",\"bfar\":\"%08lX\","
        "\"task\":\"%s\",\"task_count\":%u,\"scan\":[",
        type_str, (unsigned long)log->tick,
        (unsigned long)log->pc, (unsigned long)log->lr,
        (unsigned long)log->sp, (unsigned long)log->r0,
        (unsigned long)log->r12, (unsigned long)log->psr,
        (unsigned long)log->cfsr, (unsigned long)log->hfsr,
        (unsigned long)log->mmfar, (unsigned long)log->bfar,
        log->task_name, (unsigned)log->task_count);

    /* Return-address candidates, not an ordered trace: stale LRs from calls
     * that already returned sit among them, so this is filtered host-side.
     * It was persisted but reachable only over Trice until now, which on a
     * tunnel-only board meant the call path needed physical access. */
    for (int i = 0; i < log->scan_count && i < CRASH_LOG_MAX_SCAN; i++) {
        pos = Json_Cat(js, CRASH_JSON_CAP, pos, "%s\"%08lX\"",
                       (i > 0) ? "," : "", (unsigned long)log->scan_lr[i]);
    }

    pos = Json_Cat(js, CRASH_JSON_CAP, pos, "],\"tasks\":[");
    for (int i = 0; i < log->task_count && i < CRASH_LOG_MAX_TASKS; i++) {
        /* The same accessor the live task view uses (SysMon_StateName).  The
         * private five-entry copy this replaced could never disagree with it
         * -- uxTaskGetSystemState() never stamps eInvalid, so its `< 5` bound
         * was a dead branch -- so this is maintenance, not a bug fix. */
        const char *st = SysMon_StateName(log->tasks[i].state);
        pos = Json_Cat(js, CRASH_JSON_CAP, pos,
            "%s{\"name\":\"%s\",\"state\":\"%s\","
            "\"pc\":\"%08lX\",\"lr\":\"%08lX\",\"free_stack\":%u}",
            (i > 0) ? "," : "",
            log->tasks[i].name, st,
            (unsigned long)log->tasks[i].pc,
            (unsigned long)log->tasks[i].lr,
            log->tasks[i].free_stack);
    }
    (void)Json_Cat(js, CRASH_JSON_CAP, pos, "]}");

    vPortFree(log);
    send_json(conn, "200 OK", js);
    vPortFree(js);
}

static void handle_crash_delete(struct netconn *conn)
{
    Crash_ClearFlash();
    send_json(conn, "200 OK", "{\"status\":\"cleared\"}");
}

/* --------------------------------------------------------------------------
 * System monitor (/api/system/status, /api/system/reset-peaks)
 *
 * The whole task table does not fit resp_buf, and growing that buffer would
 * cost CCM — the constrained region on this part.  A transient heap block
 * costs nothing when nobody is asking, which is the normal case.
 * -------------------------------------------------------------------------- */

/* 4096, not 3072.  ONE TASK OBJECT IS ~210 BYTES AND THERE ARE UP TO
 * SYSMON_MAX_TASKS (16) OF THEM, so 3072 held about eleven and the roll-back
 * below silently dropped the rest -- with "tasks_total":14 still in the same
 * reply saying they existed.  The tasks that fell off the end were whichever
 * FreeRTOS happened to enumerate last, which on this board included
 * tcpip_thread: the one task carrying the WireGuard ChaCha20-Poly1305 chain,
 * and so the single most interesting stack figure on a tunnel-only device.
 * A stack report that omits tasks without saying so is worse than no report,
 * because it reads as a complete one.  16 * 210 + the ~390-byte summary is
 * ~3.8 KB; this is a transient pvPortMalloc, so it costs nothing when nobody
 * is asking. */
#define SYS_STATUS_BUF_SIZE 4096u

/* Body cap: appends stop here so the closing "]}" -- appended against the
 * full size -- always fits.  See RESP_BODY_CAP. */
#define SYS_STATUS_BODY_CAP (SYS_STATUS_BUF_SIZE - 8u)

static void handle_system_status(struct netconn *conn)
{
    const size_t    bodyCap = SYS_STATUS_BODY_CAP;
    sSysMonTaskInfo tasks[SYSMON_MAX_TASKS];
    sSysMonSummary  sum;
    char           *buf;
    size_t          off;
    uint8_t         n;
    uint8_t         listed = 0u;

    buf = (char *)pvPortMalloc(SYS_STATUS_BUF_SIZE);
    if (buf == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }

    SysMon_GetSummary(&sum);
    n = SysMon_GetTasks(tasks, SYSMON_MAX_TASKS);

    off = Json_Cat(buf, bodyCap, 0u,
        "{\"uptime_sec\":%lu,"
        "\"cpu_load_permille\":%u,"
        "\"cpu_peak_permille\":%u,"
        "\"idle_permille\":%u,"
        "\"runtime_counter_ok\":%s,"
        "\"samples\":%lu,"
        "\"reset_cause\":\"0x%08lX\","
        "\"heap\":{\"size\":%lu,\"free\":%lu,\"free_min\":%lu,\"used\":%lu},"
        "\"iwdg\":{\"gap_max_ms\":%lu,\"since_kick_ms\":%lu,"
        "\"timeout_ms\":16400},"
        "\"tasks_total\":%u,\"tasks_stale\":%u,\"stack_warnings\":%u,"
        "\"stack_warn_words\":%u,\"tasks_sampled\":%u,"
        "\"tasks\":[",
        (unsigned long)sum.uptime_sec,
        (unsigned)sum.cpuLoad_permille,
        (unsigned)sum.cpuPeakLoad_permille,
        (unsigned)sum.idle_permille,
        sum.runTimeCounterOk ? "true" : "false",
        (unsigned long)sum.sampleCnt,
        (unsigned long)System_GetResetCause(),
        (unsigned long)sum.heapSize_bytes,
        (unsigned long)sum.heapFree_bytes,
        (unsigned long)sum.heapFreeMin_bytes,
        (unsigned long)(sum.heapSize_bytes - sum.heapFree_bytes),
        (unsigned long)sum.iwdgGapMax_ms,
        (unsigned long)sum.iwdgSinceKick_ms,
        (unsigned)sum.taskCnt, (unsigned)sum.staleCnt,
        (unsigned)sum.stackWarnCnt, (unsigned)SYSMON_STACK_WARN_WORDS,
        (unsigned)n);

    for (uint8_t i = 0u; i < n; i++) {
        const sSysMonTaskInfo *t = &tasks[i];
        const size_t           mark = off;

        off = Json_Cat(buf, bodyCap, off,
            "%s{\"name\":\"%s\",\"state\":\"%s\",\"prio\":%u,"
            "\"stack_free_min_words\":%u,\"stack_size_words\":%u,"
            "\"cpu_permille\":%u,\"cpu_peak_permille\":%u,\"run_ms\":%lu,"
            "\"checkins\":%lu,\"since_checkin_ms\":%lu,\"deadline_ms\":%lu,"
            "\"stale\":%s,\"stale_cnt\":%lu,\"present\":%s}",
            (i == 0u) ? "" : ",",
            t->name, SysMon_StateName(t->state), (unsigned)t->priority,
            (unsigned)t->stackFreeMin_words, (unsigned)t->stackSize_words,
            (unsigned)t->cpuLoad_permille, (unsigned)t->cpuPeak_permille,
            (unsigned long)t->runTime_ms,
            (unsigned long)t->checkinCnt,
            (unsigned long)t->sinceCheckin_ms,
            (unsigned long)t->deadline_ms,
            t->stale ? "true" : "false",
            (unsigned long)t->staleCnt,
            t->present ? "true" : "false");

        /* Truncating mid-object would emit invalid JSON — roll the whole
         * entry back and stop on the last one that fits instead. */
        if (off >= bodyCap) {
            off = mark;
            buf[off] = '\0';
            break;
        }
        listed++;
    }

    /* "tasks_listed" vs "tasks_sampled" vs "tasks_total" is how a reader
     * tells a complete report from a clipped one.  It used to be guesswork. */
    (void)Json_Cat(buf, SYS_STATUS_BUF_SIZE, off, "],\"tasks_listed\":%u}",
                   (unsigned)listed);
    send_json(conn, "200 OK", buf);
    vPortFree(buf);
}

static void handle_system_reset_peaks(struct netconn *conn)
{
    SysMon_ResetPeaks();
    TRice("SysMon: peaks cleared over HTTP\n");
    send_json(conn, "200 OK", "{\"status\":\"cleared\"}");
}

/* POST /api/system/reboot[?delay_ms=N]
 *
 * The route that was missing.  Until now the only ways to restart a deployed
 * board were the CLI -- USB CDC or UART1, i.e. a site visit -- and abusing
 * POST /api/fwu/install to make the bootloader do it, which arms FWU state
 * nobody wanted and leaves last_fwu_result carrying a failure that never
 * happened (docs/issue_modbus_engine_stall.md 10.7).
 *
 * It answers BEFORE it acts: the reset is armed on a deadline and performed by
 * defaultTask, so the caller gets a 200 rather than a dropped connection it
 * has to interpret.
 */
static void handle_system_reboot(struct netconn *conn)
{
    int delay = query_int("delay_ms", 1000);

    if (delay < 0 || delay > 60000) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"delay_ms out of range (0..60000)\"}");
        return;
    }

    System_RequestReboot((uint32_t)delay);
    TRice("System: reboot requested over HTTP\n");

    snprintf(resp_buf, sizeof(resp_buf),
             "{\"status\":\"rebooting\",\"delay_ms\":%u}",
             (unsigned)((delay < (int)SYSTEM_REBOOT_MIN_DELAY_MS)
                            ? (int)SYSTEM_REBOOT_MIN_DELAY_MS : delay));
    send_json(conn, "200 OK", resp_buf);
}

/* --------------------------------------------------------------------------
 * Modbus observation (/api/modbus/dump, /api/modbus/monitor)
 *
 * Same reason the wg endpoints exist: these toggles were reachable only from
 * the `modbus` CLI, i.e. only over USB CDC / UART1, i.e. only with physical
 * access -- which is exactly what the tunnel removes the need for.
 *
 * The dump toggle is not merely a log level.  It is a SUBSCRIPTION, and in
 * this module a subscription is what creates the per-device timers, so
 * turning it on is what puts frames on the wire at all.  A provisioned board
 * with no subscriber polls nothing by design, per docs/modbus.md 4.2 and 5.2.
 * That makes this the remote form of "is the slave actually there?": the sink
 * logs a decoded value per point when one answers and a failed-txn line when
 * one does not.
 * -------------------------------------------------------------------------- */

static void handle_modbus_dump(struct netconn *conn, int enable)
{
    ModbusTriceSink_Set(enable);

    /* Report what the module ended up at, not what was asked for: a subscribe
     * can fail on a full table, and the sink logs that but still returns. */
    snprintf(resp_buf, sizeof(resp_buf), "{\"dump\":%s}",
             ModbusTriceSink_Get() ? "true" : "false");
    send_json(conn, "200 OK", resp_buf);
}

static void handle_modbus_monitor(struct netconn *conn, int enable)
{
    Modbus_SetMonitor(enable);
    snprintf(resp_buf, sizeof(resp_buf), "{\"monitor\":%s}",
             Modbus_GetMonitor() ? "true" : "false");
    send_json(conn, "200 OK", resp_buf);
}

/* --------------------------------------------------------------------------
 * Line occupancy (/api/modbus/bus)
 *
 * How much of each RS485 pair is already spoken for.  It is reported per PORT
 * and not per device or per plan on purpose: the wire is what is shared, so
 * the wire is the only thing a budget can be kept against.
 *
 * Read `win_permille` first -- lifetime `duty_permille` averages away the
 * bursts that actually collide.  A `max_ms` sitting near the response timeout
 * means some slave is not answering, and a silent slave is the most expensive
 * traffic there is: it holds the line for the whole timeout and returns
 * nothing.
 * -------------------------------------------------------------------------- */

static void handle_modbus_bus(struct netconn *conn)
{
    size_t off = 0u;

    off = Json_Cat(resp_buf, RESP_BODY_CAP, off, "{\"ports\":[");

    for (uint8_t id = 0u; id < (uint8_t)mbPort_last; id++) {
        sModbusBusStats st;
        const size_t    mark = off;

        if (Modbus_BusStats(id, &st) != 0) {
            continue;
        }
        off = Json_Cat(resp_buf, RESP_BODY_CAP, off,
                     "%s{\"port\":\"%s\",\"registered\":%s,\"txns\":%lu,"
                     "\"busy_ms\":%lu,\"elapsed_ms\":%lu,"
                     "\"duty_permille\":%u,\"win_permille\":%u,"
                     "\"window_sec\":%u,\"last_ms\":%u,\"max_ms\":%u}",
                     (id == 0u) ? "" : ",",
                     Modbus_PortName(id),
                     st.registered ? "true" : "false",
                     (unsigned long)st.txns,
                     (unsigned long)st.busy_ms,
                     (unsigned long)st.elapsed_ms,
                     (unsigned)st.duty_permille,
                     (unsigned)st.win_permille,
                     (unsigned)st.window_sec,
                     (unsigned)st.last_ms,
                     (unsigned)st.max_ms);
        if (off >= RESP_BODY_CAP) {
            off = mark;
            resp_buf[off] = '\0';
            break;              /* never truncate mid-object */
        }
    }

    (void)Json_Cat(resp_buf, sizeof(resp_buf), off, "]}");
    send_json(conn, "200 OK", resp_buf);
}

/* GET /api/modbus/gw -- is Home Assistant actually talking to this board?
 *
 * The one reading that matters is `listening` together with `bind_ip`: a board
 * whose tunnel was never provisioned serves NOTHING by design, and from the
 * outside that is indistinguishable from a firewall problem unless the board
 * says so itself.  `busy_exceptions` is the back-pressure counter -- non-zero
 * means the far end was told to come back, which is healthy in small numbers
 * and a bus-budget question in large ones. */
static void handle_modbus_gw(struct netconn *conn)
{
    sMbTcpStats g;

    if (MbTcp_GetStats(&g) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"gateway state unavailable\"}");
        return;
    }

    size_t off = Json_Cat(resp_buf, RESP_BODY_CAP, 0u,
                          "{\"listening\":%s,\"port\":%u,"
                          "\"bind_ip\":\"%u.%u.%u.%u\","
                          "\"connected\":%s,\"connections\":%lu,"
                          "\"requests\":%lu,\"exceptions\":%lu,"
                          "\"busy_exceptions\":%lu,",
                          g.listening ? "true" : "false",
                          (unsigned)MBTCP_PORT,
                          g.bindIp[0], g.bindIp[1], g.bindIp[2], g.bindIp[3],
                          g.connected ? "true" : "false",
                          (unsigned long)g.connections,
                          (unsigned long)g.requests,
                          (unsigned long)g.exceptions,
                          (unsigned long)g.busyExceptions);

    if (g.lastRequestAge_ms == MBTCP_AGE_NEVER) {
        off = Json_Cat(resp_buf, RESP_BODY_CAP, off,
                       "\"last_request_age_ms\":null,");
    } else {
        off = Json_Cat(resp_buf, RESP_BODY_CAP, off,
                       "\"last_request_age_ms\":%lu,",
                       (unsigned long)g.lastRequestAge_ms);
    }

    /* Said in the payload rather than left to be inferred: the gateway is
     * transparent, so this port is an ungated write path and the tunnel is
     * the whole of its authorization (design_solis_modbus_link.md §7.2). */
    (void)Json_Cat(resp_buf, RESP_BODY_CAP, off,
                   "\"budget_ms\":%u,\"transparent\":true,"
                   "\"bound_to\":\"tunnel\"}",
                   (unsigned)MBTCP_TXN_BUDGET_MS);
    send_json(conn, "200 OK", resp_buf);
}

/* POST /api/modbus/bus/reset -- zero every port's counters and restart the
 * window, so an occupancy figure can be pinned to one deliberate interval
 * instead of to uptime. */
static void handle_modbus_bus_reset(struct netconn *conn)
{
    for (uint8_t id = 0u; id < (uint8_t)mbPort_last; id++) {
        Modbus_BusStatsReset(id);
    }
    TRice("Modbus: bus counters cleared over HTTP\n");
    send_json(conn, "200 OK", "{\"status\":\"cleared\"}");
}

/* --------------------------------------------------------------------------
 * Modbus writes (/api/modbus/write)
 *
 * The module API is Modbus_Request, not Modbus_Write, because THE CONFIG
 * decides what an item means: r reads and ignores the value, w writes, rw
 * writes and reads back.  A route called /write therefore has one obligation
 * the module cannot discharge for it -- REFUSE a point the config did not
 * make writable, instead of submitting it and reporting the read that comes
 * back as a successful write.  That is what Modbus_PointInfo is for, and it
 * is why validation happens here before anything is submitted.
 *
 * Values are in the SCALED-INTEGER domain -- the same domain samples arrive in
 * and writeMin/writeMax are authored in, i.e. cell_ovp at scale 0.001 is
 * written as 3550, not 3.550.  No float is accepted anywhere on this path: a
 * threshold that silently became 3.549 V because of a decimal round trip is
 * exactly the failure this endpoint must not have.  Bounds are NOT re-checked
 * here -- the module owns that rule and enforces it per item (docs/modbus.md
 * 4.6); duplicating it would be a second copy to drift.
 * -------------------------------------------------------------------------- */

/* Defined with the wg handlers further down; the modbus handlers are kept
 * together here rather than moved to follow it. */
static int json_uint(const char *body, const char *key, uint32_t *out);

/* 16 items covers aligning a pack pair or pushing a corrected profile in one
 * call, and keeps the reply inside resp_buf.  Larger batches are a 422 rather
 * than a truncated answer. */
#define HTTP_WRITE_MAX_ITEMS   16u
#define HTTP_WRITE_BODY_MAX    1024u

static sModbusReqItem s_wrItems[HTTP_WRITE_MAX_ITEMS] CCMRAM_BSS;
static char           s_wrBody[HTTP_WRITE_BODY_MAX] CCMRAM_BSS;
static osSemaphoreId_t s_wrSem;
/* Set only if a completion callback failed to arrive, which the API forbids.
 * The item array stays borrowed forever in that case, so the endpoint retires
 * rather than hand the same memory to a second request. */
static int             s_wrPoisoned;

static void write_done_cb(const sModbusReqReply *rep, void *ctx)
{
    (void)rep; (void)ctx;
    /* Runs on the modbus task under the non-blocking rule: signal only. The
     * items are this file's array and are read after the wait returns. */
    (void)osSemaphoreRelease(s_wrSem);
}

/* Bounded key lookup INSIDE one object.  Unbounded scanning would let the
 * "id" of the next item answer for this one's missing field. */
static int obj_int(const char *p, const char *end, const char *key, int32_t *out)
{
    size_t klen = strlen(key);

    while (p < end) {
        if (*p == '"' && (size_t)(end - p) > klen + 1u &&
            strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = p + 2 + klen;
            while (q < end && (*q == ' ' || *q == ':' || *q == '\t')) q++;
            if (q >= end || (*q != '-' && (*q < '0' || *q > '9'))) return 0;
            *out = (int32_t)strtol(q, NULL, 10);
            return 1;
        }
        p++;
    }
    return 0;
}

static void handle_modbus_write(struct netconn *conn, sConnStream *s)
{
    uint32_t content_length = parse_content_length(req_buf);
    uint32_t got = 0u;
    uint32_t device = 0u, timeout_ms = 5000u;
    uint16_t count = 0u;
    const char *p, *end;
    size_t off;
    int rc;

    if (s_wrPoisoned) {
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"write path retired: a completion callback was "
                  "lost and the request buffer can never be reused\"}");
        return;
    }
    if (content_length == 0u || content_length >= sizeof(s_wrBody)) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"JSON body required (max 1023 bytes)\"}");
        return;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    while (got < content_length) {
        int ch = cs_read_byte(s);
        if (ch < 0) {
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"incomplete body\"}");
            return;
        }
        s_wrBody[got++] = (char)ch;
    }
    s_wrBody[got] = '\0';

    if (!json_uint(s_wrBody, "device", &device) || device > 255u) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"missing or invalid \\\"device\\\"\"}");
        return;
    }
    (void)json_uint(s_wrBody, "timeout_ms", &timeout_ms);
    if (timeout_ms < MB_REQ_TIMEOUT_MIN_MS || timeout_ms > MB_REQ_TIMEOUT_MAX_MS) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"timeout_ms out of range\"}");
        return;
    }

    p = strstr(s_wrBody, "\"items\"");
    if (p != NULL) p = strchr(p, '[');
    if (p == NULL) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"missing \\\"items\\\" array\"}");
        return;
    }
    end = strchr(p, ']');
    if (end == NULL) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"unterminated \\\"items\\\" array\"}");
        return;
    }

    /* Parse and validate EVERY item before submitting any of them: a batch
     * that is going to be refused should change nothing on the wire. */
    while ((p = strchr(p, '{')) != NULL && p < end) {
        const char      *objEnd = strchr(p, '}');
        int32_t          id = 0, value = 0;
        sModbusPointMeta meta;

        if (objEnd == NULL || objEnd > end) break;
        if (count >= HTTP_WRITE_MAX_ITEMS) {
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"error\":\"too many items (max %u)\"}",
                     (unsigned)HTTP_WRITE_MAX_ITEMS);
            send_json(conn, "422 Unprocessable Entity", resp_buf);
            return;
        }
        if (!obj_int(p, objEnd, "id", &id) ||
            !obj_int(p, objEnd, "value", &value) ||
            id < 0 || id > 65535) {
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"error\":\"item %u needs integer \\\"id\\\" and "
                     "\\\"value\\\"\",\"index\":%u}",
                     (unsigned)count, (unsigned)count);
            send_json(conn, "422 Unprocessable Entity", resp_buf);
            return;
        }

        rc = Modbus_PointInfo((uint8_t)device, (uint16_t)id, &meta);
        if (rc != 0) {
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"error\":\"no such point\",\"device\":%u,\"id\":%d}",
                     (unsigned)device, (int)id);
            send_json(conn, "422 Unprocessable Entity", resp_buf);
            return;
        }
        if ((meta.flags & MB_PT_WRITE) == 0u) {
            /* The whole point of the endpoint: say no, name it, and do not
             * let a read masquerade as a write. */
            char nameEsc[MB_POINT_NAME_ESC_LEN];

            (void)Json_Escape(nameEsc, sizeof(nameEsc), meta.name);
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"error\":\"point is read-only\",\"device\":%u,"
                     "\"id\":%d,\"name\":\"%s\"}",
                     (unsigned)device, (int)id, nameEsc);
            send_json(conn, "422 Unprocessable Entity", resp_buf);
            return;
        }

        s_wrItems[count].id     = (uint16_t)id;
        s_wrItems[count].value  = value;
        s_wrItems[count].result = mbErr_pending;
        count++;
        p = objEnd + 1;
    }

    if (count == 0u) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"no items\"}");
        return;
    }

    if (s_wrSem == NULL) {
        s_wrSem = osSemaphoreNew(1, 0, NULL);
        if (s_wrSem == NULL) {
            send_json(conn, "500 Internal Server Error",
                      "{\"error\":\"no semaphore\"}");
            return;
        }
    }

    rc = Modbus_Request((uint8_t)device, s_wrItems, count, timeout_ms,
                        write_done_cb, NULL);
    if (rc != 0) {
        const char *why = (rc == mbErr_full)   ? "request FIFO full"
                        : (rc == mbErr_config) ? "engine not running "
                                                 "(unprovisioned config?)"
                                               : "rejected";
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"%s\",\"result\":%d}", why, rc);
        send_json(conn, (rc == mbErr_full) ? "409 Conflict"
                                           : "422 Unprocessable Entity",
                  resp_buf);
        return;
    }

    /* The callback is contracted to fire within timeout_ms; the margin covers
     * scheduling only.  If it does not, the module still owns s_wrItems and
     * this endpoint must never lend that memory again. */
    if (osSemaphoreAcquire(s_wrSem, timeout_ms + 2000u) != osOK) {
        s_wrPoisoned = 1;
        send_json(conn, "504 Gateway Timeout",
                  "{\"error\":\"no completion callback; write path retired\"}");
        return;
    }

    off = Json_Cat(resp_buf, RESP_BODY_CAP, 0u,
                   "{\"device\":%u,\"count\":%u,\"items\":[",
                   (unsigned)device, (unsigned)count);
    for (uint16_t i = 0; i < count; i++) {
        sModbusPointMeta meta;
        char             nameEsc[MB_POINT_NAME_ESC_LEN];
        const size_t     mark = off;
        const char *nm = (Modbus_PointInfo((uint8_t)device, s_wrItems[i].id,
                                           &meta) == 0) ? meta.name : "";

        (void)Json_Escape(nameEsc, sizeof(nameEsc), nm);
        off = Json_Cat(resp_buf, RESP_BODY_CAP, off,
                       "%s{\"id\":%u,\"name\":\"%s\",\"value\":%ld,"
                       "\"result\":%d}",
                       (i == 0u) ? "" : ",",
                       (unsigned)s_wrItems[i].id, nameEsc,
                       (long)s_wrItems[i].value,
                       (int)s_wrItems[i].result);
        if (off >= RESP_BODY_CAP) {
            off = mark;
            resp_buf[off] = '\0';
            break;
        }
    }
    (void)Json_Cat(resp_buf, sizeof(resp_buf), off, "]}");
    send_json(conn, "200 OK", resp_buf);
}

/* --------------------------------------------------------------------------
 * Modbus config endpoints under /api/modbus/config — upload compiles JSON
 * straight into the inactive LUT region (compile = validation, design §4/§9);
 * apply arms the swap flag and the walker commits at a lap boundary.
 * -------------------------------------------------------------------------- */

/* Last upload compile outcome, for /api/modbus/config/status */
static sModbusCompileResult s_lastCompile;
static bool             s_haveCompile;

/* Byte source feeding the config compiler from the connection body */
typedef struct {
    sConnStream *s;
    uint32_t     remaining;
} sBodySource;

static int body_source(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sBodySource *b = (sBodySource *)ctx;

    if (b->remaining == 0u) {
        return 0;
    }
    if (cs_fill(b->s) != ERR_OK) {
        return -1;
    }

    uint32_t n = (uint32_t)(b->s->len - b->s->off);
    if (n > b->remaining) n = b->remaining;
    if (n > maxLen)       n = maxLen;

    memcpy(buf, (uint8_t *)b->s->data + b->s->off, n);
    b->s->off    += (u16_t)n;
    b->remaining -= n;
    return (int)n;
}

static void send_compile_result(struct netconn *conn,
                                const sModbusCompileResult *res)
{
    if (res->ok) {
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\":\"compiled\",\"capabilities\":%u,"
                 "\"devices\":%u,\"plans\":%u,\"points\":%u}",
                 res->counts.capabilities, res->counts.devices,
                 res->counts.plans, res->counts.points);
        send_json(conn, "200 OK", resp_buf);
    } else {
        char fieldEsc[MB_CFG_ERR_ESC_LEN];

        (void)Json_Escape(fieldEsc, sizeof(fieldEsc), res->field);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"%s\",\"field\":\"%s\","
                 "\"capability\":%d,\"device\":%d,\"plan\":%d,"
                 "\"index\":%d}",
                 res->reason, fieldEsc,
                 res->capIdx, res->devIdx, res->planIdx, res->subIdx);
        send_json(conn, "422 Unprocessable Entity", resp_buf);
    }
}

static void handle_modbus_cfg_upload(struct netconn *conn, sConnStream *s)
{
    uint32_t content_length = parse_content_length(req_buf);

    if (content_length == 0u || content_length > 64u * 1024u) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"Content-Length required (max 64 KB)\"}");
        return;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    sBodySource src = { s, content_length };
    if (Modbus_ConfigCompile(body_source, &src, &s_lastCompile) == mbErr_busy) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"apply pending; config swap not yet committed\"}");
        return;
    }
    s_haveCompile = true;

    if (s_lastCompile.ok) {
        /* §8.4's landmark, in its field order: capabilities, plans, devices,
         * points. */
        TRice("Modbus config: staged %u capability %u plan %u device %u points\n",
              s_lastCompile.counts.capabilities, s_lastCompile.counts.plans,
              s_lastCompile.counts.devices, s_lastCompile.counts.points);
    } else {
        TRiceS("Modbus config: rejected: %s\n", s_lastCompile.reason);
    }
    send_compile_result(conn, &s_lastCompile);
}

static void handle_modbus_cfg_apply(struct netconn *conn)
{
    if (Modbus_ConfigApply() != 0) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"no valid staged config to apply\"}");
        return;
    }
    TRice("Modbus config: apply armed\n");
    send_json(conn, "200 OK", "{\"status\":\"pending\"}");
}

static void handle_modbus_cfg_status(struct netconn *conn)
{
    sModbusConfigStatus st;

    if (Modbus_ConfigStatus(&st) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"status failed\"}");
        return;
    }

    sModbusConfigCounts counts = st.counts;
    bool                valid  = (st.valid != 0u);

    size_t pos = Json_Cat(resp_buf, RESP_BODY_CAP, 0u,
        "{\"active_region\":%u,\"valid\":%s,\"state\":\"%s\","
        "\"capabilities\":%u,\"devices\":%u,\"plans\":%u,\"points\":%u,"
        "\"staged_valid\":%s,\"swap_pending\":%s",
        st.activeRegion,
        valid ? "true" : "false",
        valid ? "provisioned" : "unprovisioned",
        counts.capabilities, counts.devices, counts.plans, counts.points,
        st.stagedValid ? "true" : "false",
        st.swapPending ? "true" : "false");

    /* Scheduler health.  `ticks_armed` short of `ticks_live`, or a non-zero
     * `arm_failures`, is the ONE reading that distinguishes "the engine is
     * idle because nothing is subscribed" from "the engine is idle because its
     * clocks stopped" — every other field on this page looks identical in both
     * (docs/issue_modbus_engine_stall.md §1). */
    {
        sModbusScheduleStats sc;

        if (Modbus_ScheduleStats(&sc) == 0) {
            pos = Json_Cat(resp_buf, RESP_BODY_CAP, pos,
                ",\"scheduler\":{\"sequences\":%u,\"ticks_live\":%u,"
                "\"ticks_armed\":%u,\"arm_failures\":%lu,"
                "\"dropped_pokes\":%lu,\"healthy\":%s}",
                sc.sequences, sc.ticksLive, sc.ticksArmed,
                (unsigned long)sc.armFailures,
                (unsigned long)sc.droppedPokes,
                (sc.ticksArmed == sc.ticksLive && sc.armFailures == 0u)
                    ? "true" : "false");
        }
    }

    /* Per device: its port, whether that port has a driver, which plans cover
     * it and whether anything is actually polling it — "why is this device not
     * polled" is a question about SUBSCRIBERS (§4.3). */
    {
        sModbusDeviceInfo devs[MB_MAX_DEVICES];
        int               dn = Modbus_DeviceList(devs, MB_MAX_DEVICES);

        pos = Json_Cat(resp_buf, RESP_BODY_CAP, pos, ",\"devices_state\":[");
        for (int i = 0; i < dn; i++) {
            char prefixEsc[MB_PREFIX_ESC_LEN];

            (void)Json_Escape(prefixEsc, sizeof(prefixEsc),
                              devs[i].topicPrefix);
            pos = Json_Cat(resp_buf, RESP_BODY_CAP, pos,
                "%s{\"id\":%u,\"prefix\":\"%s\",\"slave\":%u,"
                "\"capability\":%u,\"port\":\"%s\",\"port_up\":%s,"
                "\"baud\":%lu,\"plans\":%u,\"polled\":%s}",
                i ? "," : "", devs[i].devOrd, prefixEsc,
                devs[i].slaveAddr, devs[i].capId,
                Modbus_PortName(devs[i].portId),
                devs[i].portUp ? "true" : "false",
                (unsigned long)devs[i].baud, devs[i].coveringPlans,
                devs[i].polled ? "true" : "false");
        }
        pos = Json_Cat(resp_buf, RESP_BODY_CAP, pos, "]");
    }

    if (s_haveCompile) {
        /* The compiler's `field` is copied out of the uploaded document, so
         * it carries whatever the author wrote there. */
        char fieldEsc[MB_CFG_ERR_ESC_LEN];

        (void)Json_Escape(fieldEsc, sizeof(fieldEsc), s_lastCompile.field);
        pos = Json_Cat(resp_buf, RESP_BODY_CAP, pos,
            ",\"last_upload\":{\"ok\":%s,\"error\":\"%s\",\"field\":\"%s\","
            "\"capability\":%d,\"device\":%d,\"plan\":%d,\"index\":%d}",
            s_lastCompile.ok ? "true" : "false",
            s_lastCompile.reason, fieldEsc,
            s_lastCompile.capIdx, s_lastCompile.devIdx,
            s_lastCompile.planIdx, s_lastCompile.subIdx);
    }
    (void)Json_Cat(resp_buf, sizeof(resp_buf), pos, "}");
    send_json(conn, "200 OK", resp_buf);
}

static int export_count_sink(void *ctx, const char *data, uint32_t len)
{
    (void)data;
    *(uint32_t *)ctx += len;
    return 0;
}

static int export_conn_sink(void *ctx, const char *data, uint32_t len)
{
    return send_all((struct netconn *)ctx, data, len) ? 0 : -1;
}

static void handle_modbus_cfg_download(struct netconn *conn)
{
    /* Pass 1: measure for Content-Length (export is a pure function of the
     * region; the walker never modifies regions, so two passes agree) */
    uint32_t total = 0;
    if (Modbus_ConfigExport(export_count_sink, &total) != 0) {
        send_json(conn, "404 Not Found", "{\"error\":\"no valid config\"}");
        return;
    }

    char hdr[224];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %lu\r\n"
        "Content-Disposition: attachment; filename=\"modbus_config.json\"\r\n"
        "Connection: close\r\n\r\n",
        (unsigned long)total);
    send_all(conn, hdr, hlen);

    /* Pass 2: stream it out */
    (void)Modbus_ConfigExport(export_conn_sink, conn);
}

/* POST /api/modbus/config/verify — validate without writing anything.
 * The upload path consumes the inactive region on success AND on failure, and
 * that region holds the previous config, so this is how an operator checks a
 * config without destroying the fallback (docs/modbus.md §4.9). */
static void handle_modbus_cfg_verify(struct netconn *conn, sConnStream *s)
{
    uint32_t content_length = parse_content_length(req_buf);

    if (content_length == 0u || content_length > 64u * 1024u) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"Content-Length required (max 64 KB)\"}");
        return;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    sModbusCompileResult res;
    sBodySource          src = { s, content_length };

    Modbus_ConfigVerify(body_source, &src, &res);
    send_compile_result(conn, &res);
}

/* --------------------------------------------------------------------------
 * Plans (/api/modbus/plans) — the one config object that changes at runtime
 *
 * A plan body is one element of the config's plans[] array, so the same JSON
 * an operator would paste into a config is what these take: one schema, one
 * validator (§8.1).  409 means the plan has a subscriber that NAMED it —
 * MB_PLAN_ALL subscribers do not lock a plan — and the body reports the
 * subscriber count so an operator can tell "something holds this" from "the
 * region is busy", which is the other 409.
 * -------------------------------------------------------------------------- */

static void handle_modbus_plans_list(struct netconn *conn)
{
    sModbusPlanInfo plans[MB_MAX_PLANS];
    int             n = Modbus_PlanList(plans, MB_MAX_PLANS);
    size_t          at;

    if (n < 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"plan list failed\"}");
        return;
    }

    at = Json_Cat(resp_buf, RESP_BODY_CAP, 0u, "{\"plans\":[");
    for (int i = 0; i < n; i++) {
        char nameEsc[MB_PREFIX_ESC_LEN];

        (void)Json_Escape(nameEsc, sizeof(nameEsc), plans[i].name);
        at = Json_Cat(resp_buf, RESP_BODY_CAP, at,
            "%s{\"id\":%u,\"name\":\"%s\",\"capability\":%u,"
            "\"devices\":%u,\"timeTables\":%u,\"subscribers\":%u}",
            i ? "," : "", plans[i].planId, nameEsc, plans[i].capId,
            plans[i].devices, plans[i].timeTables, plans[i].subscribers);
    }
    (void)Json_Cat(resp_buf, sizeof(resp_buf), at, "]}");
    send_json(conn, "200 OK", resp_buf);
}

/* Map a plan API return onto the status an operator should see. */
static void send_plan_result(struct netconn *conn, int r, int planId,
                             const char *okStatus)
{
    if (r == 0) {
        if (planId >= 0) {
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"status\":\"pending\",\"id\":%d}", planId);
        } else {
            snprintf(resp_buf, sizeof(resp_buf), "{\"status\":\"pending\"}");
        }
        send_json(conn, okStatus, resp_buf);
        return;
    }

    if (r == mbErr_busy) {
        sModbusPlanInfo info;
        unsigned subs = 0;
        if (planId >= 0 && Modbus_PlanGet((uint8_t)planId, &info) == 0) {
            subs = info.subscribers;
        }
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"plan busy\",\"subscribers\":%u}", subs);
        send_json(conn, "409 Conflict", resp_buf);
        return;
    }

    snprintf(resp_buf, sizeof(resp_buf), "{\"error\":\"rejected\",\"code\":%d}",
             r);
    send_json(conn, (r == mbErr_full) ? "507 Insufficient Storage"
                                      : "422 Unprocessable Entity", resp_buf);
}

/* Body -> spec, shared by POST (create) and PUT (modify). */
static int read_plan_body(struct netconn *conn, sConnStream *s,
                          sModbusPlanSpec *spec, int *authoredId)
{
    uint32_t content_length = parse_content_length(req_buf);

    if (content_length == 0u || content_length > 8u * 1024u) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"Content-Length required (max 8 KB)\"}");
        return -1;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    sModbusCompileResult res;
    sBodySource          src = { s, content_length };

    if (Modbus_PlanParse(body_source, &src, spec, authoredId, &res) != 0) {
        send_compile_result(conn, &res);
        return -1;
    }
    return 0;
}

static void handle_modbus_plan_create(struct netconn *conn, sConnStream *s)
{
    sModbusPlanSpec spec;
    int             authoredId = -1;

    if (read_plan_body(conn, s, &spec, &authoredId) != 0) {
        return;
    }

    uint8_t slot = 0;
    int     r    = Modbus_PlanCreate(&spec, &slot);
    send_plan_result(conn, r, (r == 0) ? (int)slot : authoredId,
                     "201 Created");
}

static void handle_modbus_plan_modify(struct netconn *conn, sConnStream *s,
                                      uint8_t planId)
{
    sModbusPlanSpec spec;
    int             authoredId = -1;

    if (read_plan_body(conn, s, &spec, &authoredId) != 0) {
        return;
    }
    send_plan_result(conn, Modbus_PlanModify(planId, &spec), (int)planId,
                     "200 OK");
}

static void handle_modbus_plan_delete(struct netconn *conn, uint8_t planId)
{
    send_plan_result(conn, Modbus_PlanDelete(planId), (int)planId, "200 OK");
}

/* DELETE /api/modbus/config — erase, not reset.  With no built-in default
 * there is nothing to reset TO (docs/modbus.md §4.9), so this makes the board
 * unprovisioned: no devices, no timers, no bus traffic, an empty catalogue. */
static void handle_modbus_cfg_erase(struct netconn *conn)
{
    if (Modbus_ConfigErase() != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"erase failed\"}");
        return;
    }

    TRice("Modbus config: erased, board is unprovisioned\n");
    send_json(conn, "200 OK", "{\"status\":\"unprovisioned\"}");
}

/* --------------------------------------------------------------------------
 * Request dispatch
 * -------------------------------------------------------------------------- */

static bool route_is(const char *method_path)
{
    return strncmp(req_buf, method_path, strlen(method_path)) == 0;
}

/* --------------------------------------------------------------------------
 * WireGuard configuration
 *
 * These exist because the `wg` CLI is reachable only over USB CDC / UART1,
 * i.e. only with physical access to the board — which is exactly what the
 * tunnel is supposed to remove the need for.  HTTP is the one remote channel
 * that keeps working when the tunnel itself is misconfigured, so it is the
 * only place a tunnel misconfiguration can actually be repaired from.
 * -------------------------------------------------------------------------- */

/* Minimal field lookup: finds "key" and returns the first non-space character
 * after the following colon.  Adequate for the small flat objects accepted
 * here — no nesting, no escapes, no duplicate keys. */
static const char *json_value(const char *body, const char *key)
{
    char pattern[32];
    const char *p;

    (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(body, pattern);
    if (p == NULL) {
        return NULL;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static int json_uint(const char *body, const char *key, uint32_t *out)
{
    const char *p = json_value(body, key);

    if (p == NULL || *p < '0' || *p > '9') {
        return 0;
    }
    *out = (uint32_t)strtoul(p, NULL, 10);
    return 1;
}

static int json_bool(const char *body, const char *key, int dflt)
{
    const char *p = json_value(body, key);

    if (p == NULL) {
        return dflt;
    }
    if (strncmp(p, "true", 4) == 0) {
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        return 0;
    }
    return dflt;
}

/* Accepts a quoted dotted-quad and rejects anything else, including octets
 * above 255 — a silently truncated address would be worse than a 422. */
static int json_ipv4(const char *body, const char *key, uint8_t ip[4])
{
    const char *p = json_value(body, key);
    unsigned    o[4];
    int         n;

    if (p == NULL || *p != '"') {
        return 0;
    }
    n = sscanf(p + 1, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]);
    if (n != 4) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        if (o[i] > 255u) {
            return 0;
        }
        ip[i] = (uint8_t)o[i];
    }
    return 1;
}

static void wg_status_json(char *buf, size_t sz)
{
    const sWgLinkCfg *cfg         = WgLink_ActiveCfg();
    uint32_t          now         = 0u;
    uint32_t          persisted   = 0u;
    uint32_t          rngFailures = 0u;
    int               flashOk     = 0;
    int               hwSeeded    = 0;

    WgTime_GetStatus(&now, &persisted, &flashOk);
    WgPlatform_GetRngStatus(&hwSeeded, &rngFailures);

    char pubKey[WG_KEY_B64_SIZE]  = "";
    char peerKey[WG_KEY_B64_SIZE] = "";
    char allowed[64]              = "";
    size_t n = 0u;
    uint8_t i;

    /* The public key is what the operator pastes into the hub.  The private
     * key is never reported by any endpoint. */
    (void)WgLink_GetPublicKeyB64(pubKey, sizeof(pubKey));
    (void)WgLink_GetPeerKeyB64(peerKey, sizeof(peerKey));

    for (i = 0u; i < cfg->allowedCount; i++) {
        n = Json_Cat(allowed, sizeof(allowed), n,
                      "%s\"%u.%u.%u.%u/%u.%u.%u.%u\"", (i > 0u) ? "," : "",
                      cfg->allowed[i].ip[0], cfg->allowed[i].ip[1],
                      cfg->allowed[i].ip[2], cfg->allowed[i].ip[3],
                      cfg->allowed[i].mask[0], cfg->allowed[i].mask[1],
                      cfg->allowed[i].mask[2], cfg->allowed[i].mask[3]);
    }

    sWgPeerStats st;
    char peerStats[320] = "";

    if (WgLink_GetPeerStats(&st) == 0) {
        /* alive_age_ms is the number session_up is a threshold on, and
         * keypair_valid/prev_valid are the raw port state it deliberately
         * does not trust: prev_valid staying 1 while alive_age_ms climbs is
         * exactly the stuck peer this reports on. */
        (void)snprintf(peerStats, sizeof(peerStats),
            ",\"peer_last_rx_ms\":%u,\"peer_last_tx_ms\":%u,"
            "\"keypair_valid\":%s,\"prev_keypair_valid\":%s,"
            "\"keypair_age_ms\":%u,\"alive_age_ms\":%d,"
            "\"recoveries\":%u,"
            "\"tx_packets\":%u,\"rx_counter\":%u,"
            "\"live_endpoint\":\"%u.%u.%u.%u:%u\",\"now_ms\":%u",
            (unsigned)st.lastRx_ms, (unsigned)st.lastTx_ms,
            st.sessionValid ? "true" : "false",
            st.prevValid    ? "true" : "false",
            (unsigned)st.keypairAge_ms,
            (st.aliveAge_ms == WG_LINK_AGE_NEVER) ? -1 : (int)st.aliveAge_ms,
            (unsigned)WgLink_RecoveryCount(),
            (unsigned)st.txPackets, (unsigned)st.rxCounter,
            st.endpointIp[0], st.endpointIp[1],
            st.endpointIp[2], st.endpointIp[3],
            (unsigned)st.endpointPort, (unsigned)sys_now());
    }

    (void)snprintf(buf, sz,
        "{\"running\":%s,\"session_up\":%s,\"provisioned\":%s,"
        "\"config_source\":\"%s\",\"config_version\":%u,"
        "\"public_key\":\"%s\",\"peer_public_key\":\"%s\","
        "\"tunnel_ip\":\"%u.%u.%u.%u\",\"tunnel_mask\":\"%u.%u.%u.%u\","
        "\"allowed_ips\":[%s],"
        "\"endpoint_ip\":\"%u.%u.%u.%u\",\"endpoint_port\":%u,"
        "\"keepalive\":%u,\"rng_hw_seeded\":%s,\"rng_failures\":%u,"
        "\"time_now\":%u,\"time_persisted\":%u,\"time_flash_backed\":%s%s}",
        WgLink_IsRunning() ? "true" : "false",
        WgLink_IsUp()      ? "true" : "false",
        WgLink_HasIdentity() ? "true" : "false",
        WgLink_CfgIsStored() ? "stored" : "none",
        (unsigned)WgLink_CfgStoredVersion(),
        pubKey, peerKey,
        cfg->tunnelIp[0], cfg->tunnelIp[1], cfg->tunnelIp[2], cfg->tunnelIp[3],
        cfg->tunnelMask[0], cfg->tunnelMask[1],
        cfg->tunnelMask[2], cfg->tunnelMask[3],
        allowed,
        cfg->endpointIp[0], cfg->endpointIp[1],
        cfg->endpointIp[2], cfg->endpointIp[3],
        (unsigned)cfg->endpointPort, (unsigned)cfg->keepAlive_sec,
        hwSeeded ? "true" : "false", (unsigned)rngFailures,
        (unsigned)now, (unsigned)persisted, flashOk ? "true" : "false",
        peerStats);
}


/* ==========================================================================
 * Battery packs (docs/design_battery_pack.md §10)
 *
 * READ-ONLY except for the configuration routes.  There is deliberately no
 * generic write-through: a command is a named, capability-gated, bounded
 * thing or it is nothing (§16 item 1).
 * ========================================================================== */

#define PACK_STATUS_JSON_CAP    4096u
#define PACK_CELLS_JSON_CAP     2048u

/* Room for the closing "],\"truncated\":true}" and its NUL.  Every body
 * append below is made against a cap that stops this short, so the tail
 * always fits: what a client receives is well-formed JSON whether or not
 * everything fitted, which is the property the old 512-byte reserve was
 * reaching for and could not deliver (docs/task_json_module.md §2.1 -- the
 * object it reserved for measured 519 bytes of format text alone).
 *
 * Truncation is REPORTED, not hidden.  "count":8 with four entries is a
 * client-visible contradiction otherwise. */
#define PACK_JSON_TAIL          24u

/* /api/pack/cells closes with more than the others: the array (possibly with
 * "truncated"), then the pack-level estimator summary, then "}".  ~55 bytes
 * at worst, so it gets its own reserve rather than sharing the 24. */
#define PACK_CELLS_JSON_TAIL    64u

/* Worst case for an escaped name is six bytes out per byte in (\u00XX).
 * Only a name written with \u escapes reaches it -- which is exactly the
 * input that gets sent. */
#define PACK_NAME_ESC_LEN       (((PACK_NAME_LEN - 1u) * 6u) + 1u)
#define PACK_STAT_NAME_ESC_LEN  (((PACK_STAT_NAME_LEN - 1u) * 6u) + 1u)

/** An ePackAlarm mask as an array of names.
 *
 *  THE ALARM VOCABULARY BELONGS TO App/Pack, so this calls Pack_AlarmName
 *  rather than growing a second spelling here -- which is exactly the mistake
 *  this file and cmd_parser.c made with ePackCondition, and the reason
 *  ePackAlarm reached the tunnel as a bare integer until 2026-09-07.
 *
 *  Emitted unescaped: pack_cfg.c states as a constraint on its table that no
 *  name contains a quote or a backslash. */
static size_t pack_alarm_names(char *js, size_t cap, size_t pos, uint32_t bits)
{
    uint32_t b;
    int      first = 1;

    pos = Json_Cat(js, cap, pos, "[");
    for (b = 1u; b <= (uint32_t)packAlarm_protectionOpen; b <<= 1) {
        if ((bits & b) == 0u) {
            continue;
        }
        pos = Json_Cat(js, cap, pos, "%s\"%s\"", first ? "" : ",",
                       Pack_AlarmName(b));
        first = 0;
    }
    return Json_Cat(js, cap, pos, "]");
}

/** Close a pack document, saying so if a loop had to stop early. */
static void pack_json_close(char *js, size_t cap, size_t pos, int truncated)
{
    (void)Json_Cat(js, cap, pos,
                   truncated ? "],\"truncated\":true}" : "]}");
}

/** GET /api/pack/status — every pack, plus the module's own counters. */
static void handle_pack_status(struct netconn *conn)
{
    const size_t bodyCap = PACK_STATUS_JSON_CAP - PACK_JSON_TAIL;
    char    *js;
    size_t   pos = 0u;
    int      truncated = 0;
    int      firstPack = 1;
    uint8_t  i;
    sPackStats stats;

    js = (char *)pvPortMalloc(PACK_STATUS_JSON_CAP);
    if (js == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    (void)Pack_Stats(&stats);

    pos = Json_Cat(js, bodyCap, pos,
        "{\"provisioned\":%s,\"count\":%d,"
        "\"stats\":{\"updates\":%u,\"stale\":%u,\"bindFailures\":%u,"
        "\"cmdAccepted\":%u,\"cmdOk\":%u,\"cmdFailed\":%u,"
        "\"cmdUnknown\":%u,\"cmdRefused\":%u,\"lateCompletes\":%u},"
        "\"packs\":[",
        stats.provisioned ? "true" : "false", Pack_Count(),
        (unsigned)stats.updates, (unsigned)stats.staleEvents,
        (unsigned)stats.bindFailures, (unsigned)stats.cmdAccepted,
        (unsigned)stats.cmdOk, (unsigned)stats.cmdFailed,
        (unsigned)stats.cmdUnknown, (unsigned)stats.cmdRefused,
        (unsigned)stats.lateCompletes);

    for (i = 0u; i < PACK_MAX; i++) {
        sPackState   st;
        sPackSocDiag diag;
        char       nameEsc[PACK_NAME_ESC_LEN];
        size_t     mark = pos;
        uint32_t   g;
        int        first = 1;

        if (Pack_GetState(i, &st) != packErr_ok) {
            continue;
        }
        /* Diagnostics live outside sPackState (which is size-budgeted per
         * instance), so they are a second read -- zeroed on failure, which
         * reads as "not measured". */
        if (Pack_SocDiag(i, &diag) != packErr_ok) {
            (void)memset(&diag, 0, sizeof(diag));
        }
        /* The name is operator-supplied and reaches flash through the config
         * parser, so it is not safe to interpolate raw. */
        (void)Json_Escape(nameEsc, sizeof(nameEsc), st.name);

        pos = Json_Cat(js, bodyCap, pos,
            "%s{\"idx\":%u,\"name\":\"%s\",\"type\":\"%s\","
            "\"cond\":\"%s\",\"why\":%u,\"whyText\":\"%s\","
            "\"caps\":%u,\"cmds\":%u,\"flags\":%u,"
            "\"voltage_mV\":%u,\"current_mA\":%d,"
            "\"soc_pm\":%u,\"soh_pm\":%u,"
            "\"socConf_pm\":%u,\"sohConf_pm\":%u,\"socDrift_pm\":%d,"
            "\"dcRes_uOhm\":%lu,\"dcResSteps\":%lu,"
            "\"anchorSamples\":%lu,\"anchorIrSamples\":%lu,"
            "\"remaining_mAh\":%u,\"capacity_mAh\":%u,\"nameplate_mAh\":%u,"
            "\"chargeLimit_mA\":%u,\"dischargeLimit_mA\":%u,"
            "\"chargeVoltLimit_mV\":%u,\"dischargeVoltLimit_mV\":%u,"
            "\"chargeSwitch\":%u,\"dischargeSwitch\":%u,"
            "\"chargeSwitchText\":\"%s\",\"dischargeSwitchText\":\"%s\","
            "\"tempMin_dC\":%d,\"tempMax_dC\":%d,"
            "\"cellMin_mV\":%u,\"cellMax_mV\":%u,"
            "\"cellMinIdx\":%u,\"cellMaxIdx\":%u,"
            "\"alarms\":%u,\"vendorAlarms\":[%u,%u],"
            "\"alarmNames\":",
            firstPack ? "" : ",",
            (unsigned)st.idx, nameEsc, Pack_TypeName(st.typeId),
            Pack_CondName(st.cond),
            /* The numeric `why` stays for compatibility; `whyText` is the
             * field that makes a pack diagnosable over the tunnel.  Until
             * now the explanation existed only on the CLI, which needs
             * physical access -- backwards on a tunnel-only board.  Emitted
             * unescaped: Pack_WhyName's wording contains no '"' or '\\'
             * (pack_cfg.c states that as a constraint on the table). */
            (unsigned)st.why, Pack_WhyName(st.why),
            (unsigned)st.caps, (unsigned)st.cmds, (unsigned)st.flags,
            (unsigned)st.voltage_mV, (int)st.current_mA,
            (unsigned)st.soc_pm, (unsigned)st.soh_pm,
            (unsigned)st.socConf_pm, (unsigned)st.sohConf_pm,
            (int)st.socDrift_pm,
            (unsigned long)diag.dcRes_uOhm,
            (unsigned long)diag.dcResSteps,
            (unsigned long)diag.anchorSamples,
            (unsigned long)diag.anchorIrSamples,
            (unsigned)st.remaining_mAh, (unsigned)st.capacity_mAh,
            (unsigned)st.nameplate_mAh,
            (unsigned)st.chargeLimit_mA, (unsigned)st.dischargeLimit_mA,
            (unsigned)st.chargeVoltLimit_mV,
            (unsigned)st.dischargeVoltLimit_mV,
            (unsigned)st.chargeSwitch, (unsigned)st.dischargeSwitch,
            Pack_SwitchName(st.chargeSwitch),
            Pack_SwitchName(st.dischargeSwitch),
            (int)st.tempMin_dC, (int)st.tempMax_dC,
            (unsigned)st.cellMin_mV, (unsigned)st.cellMax_mV,
            (unsigned)st.cellMinIdx, (unsigned)st.cellMaxIdx,
            (unsigned)st.alarms,
            (unsigned)st.vendorAlarms[0], (unsigned)st.vendorAlarms[1]);

        /* The numeric `alarms` stays for compatibility; `alarmNames` is what
         * makes an alarm diagnosable over the tunnel, on the `why`/`whyText`
         * precedent above.  Until 2026-09-07 the ONLY rendering of ePackAlarm
         * anywhere was this bare integer. */
        pos = pack_alarm_names(js, bodyCap, pos, st.alarms);
        pos = Json_Cat(js, bodyCap, pos, ",\"age_ms\":[");

        /* PER-GROUP AGES.  A consumer applies its own staleness policy, and
         * on a JK the cell group runs 4-5 s behind the electrical one by
         * construction -- one age per pack would be a lie (§3). */
        for (g = 0u; g < (uint32_t)packGrp_last; g++) {
            if (st.age_ms[g] == PACK_AGE_NEVER) {
                pos = Json_Cat(js, bodyCap, pos, "%snull", first ? "" : ",");
            } else {
                pos = Json_Cat(js, bodyCap, pos, "%s%u", first ? "" : ",",
                               (unsigned)st.age_ms[g]);
            }
            first = 0;
        }
        pos = Json_Cat(js, bodyCap, pos, "]}");

        /* Whole object or none.  A pack half-written into a full buffer is
         * unparseable for the client, which is worse than a missing pack. */
        if (pos >= bodyCap) {
            pos = mark;
            js[pos] = '\0';
            truncated = 1;
            break;
        }
        /* The separator follows what was EMITTED, not the loop index: a pack
         * index with no pack behind it used to put a bare comma at the head
         * of the array. */
        firstPack = 0;
    }

    pack_json_close(js, PACK_STATUS_JSON_CAP, pos, truncated);
    send_json(conn, "200 OK", js);
    vPortFree(js);
}

/** GET /api/pack/cells?idx=N — cell detail, where the type has it. */
static void handle_pack_cells(struct netconn *conn, uint8_t idx)
{
    const size_t bodyCap = PACK_CELLS_JSON_CAP - PACK_CELLS_JSON_TAIL;
    sPackCells cl;
    char      *js;
    size_t     pos = 0u;
    int        truncated = 0;
    uint8_t    c;
    int        r;

    r = Pack_GetCells(idx, &cl);
    if (r == packErr_notSupported) {
        /* A capability that is absent is not an error in the pack -- this
         * type simply does not report cells (a Pylontech-speaking pack never
         * will).  Say which it is. */
        send_json(conn, "404 Not Found",
                  "{\"error\":\"this pack type reports no cell detail\"}");
        return;
    }
    if (r != packErr_ok) {
        send_json(conn, "404 Not Found", "{\"error\":\"no such pack\"}");
        return;
    }

    js = (char *)pvPortMalloc(PACK_CELLS_JSON_CAP);
    if (js == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    pos = Json_Cat(js, bodyCap, pos,
        "{\"idx\":%u,\"cellCount\":%u,\"age_ms\":%u,"
        "\"balance\":{\"active\":%u,\"current_mA\":%d,\"duty_pm\":%u,"
        "\"srcIdx\":%d,\"sinkIdx\":%d},\"cells\":[",
        (unsigned)idx, (unsigned)cl.cellCount, (unsigned)cl.age_ms,
        (unsigned)cl.balanceActive, (int)cl.balanceCurrent_mA,
        (unsigned)cl.balanceDuty_pm,
        (cl.balanceSrcIdx  == PACK_CELL_NONE) ? -1 : (int)cl.balanceSrcIdx,
        (cl.balanceSinkIdx == PACK_CELL_NONE) ? -1 : (int)cl.balanceSinkIdx);

    {
        sPackCellEstimate est;
        const int haveEst = (Pack_GetCellEstimate(idx, &est) == packErr_ok);

        for (c = 0u; c < cl.cellCount; c++) {
            const size_t mark = pos;

            pos = Json_Cat(js, bodyCap, pos,
                           "%s{\"mV\":%u,\"leadRes_mOhm\":%u",
                           (c == 0u) ? "" : ",",
                           (unsigned)cl.cell_mV[c],
                           (unsigned)cl.leadRes_mOhm[c]);
            if (haveEst != 0) {
                /* soc_pm -1 = not anchored; capacity 0 = not yet measured.
                 * Both are reported as-is rather than hidden, so a consumer
                 * can tell "unknown" from "zero". */
                pos = Json_Cat(js, bodyCap, pos,
                               ",\"soc_pm\":%d,\"capacity_mAh\":%d"
                               ",\"capConf_pm\":%u",
                               (int)est.soc_pm[c],
                               (int)est.capacity_mAh[c],
                               (unsigned)est.capConf_pm[c]);
            }
            pos = Json_Cat(js, bodyCap, pos, "}");

            if (pos >= bodyCap) {
                pos = mark;
                js[pos] = '\0';
                truncated = 1;
                break;
            }
        }
        /* Close the cells array, then the pack-level estimator summary
         * alongside it -- not inside it, where it would be a property of
         * cell 0.  These go against the FULL cap: after a roll-back `pos` can
         * sit one byte below bodyCap, and a tail that got dropped would leave
         * the array open. */
        pos = Json_Cat(js, PACK_CELLS_JSON_CAP, pos,
                       truncated ? "],\"truncated\":true" : "]");
        if (haveEst != 0) {
            pos = Json_Cat(js, PACK_CELLS_JSON_CAP, pos,
                           ",\"weakestIdx\":%d,\"measuredCells\":%u",
                           (int)est.weakestIdx,
                           (unsigned)est.measuredCount);
        }
    }
    (void)Json_Cat(js, PACK_CELLS_JSON_CAP, pos, "}");
    send_json(conn, "200 OK", js);
    vPortFree(js);
}

/** GET /api/pack/stats?idx=N -- what this pack knows about itself (§23.1).
 *  A GENERIC LIST: the renderer needs no per-vendor code, which is what lets
 *  a JK and a Dyness expose different sets through one endpoint. */
static void handle_pack_stats(struct netconn *conn, uint8_t idx)
{
    const size_t bodyCap = PACK_CELLS_JSON_CAP - PACK_JSON_TAIL;
    char    *js;
    size_t   pos = 0u;
    int      truncated = 0;
    int      first = 1;
    int      cnt = Pack_StatCount(idx);
    int      i;

    if (cnt < 0) {
        send_json(conn, "404 Not Found", "{\"error\":\"no such pack\"}");
        return;
    }
    js = (char *)pvPortMalloc(PACK_CELLS_JSON_CAP);
    if (js == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    pos = Json_Cat(js, bodyCap, pos,
                   "{\"idx\":%u,\"count\":%d,\"stats\":[",
                   (unsigned)idx, cnt);
    for (i = 0; i < cnt; i++) {
        sPackStat    st;
        char         nameEsc[PACK_STAT_NAME_ESC_LEN];
        const size_t mark = pos;

        if (Pack_StatGet(idx, (uint8_t)i, &st) != packErr_ok) {
            continue;
        }
        (void)Json_Escape(nameEsc, sizeof(nameEsc), st.name);

        pos = Json_Cat(js, bodyCap, pos,
            "%s{\"name\":\"%s\",\"value\":%d,\"unit\":%u,"
            "\"scale\":%d,\"flags\":%u}",
            first ? "" : ",", nameEsc, (int)st.value,
            (unsigned)st.unit, (int)st.scale_pow10, (unsigned)st.flags);

        if (pos >= bodyCap) {
            pos = mark;
            js[pos] = '\0';
            truncated = 1;
            break;
        }
        first = 0;
    }
    pack_json_close(js, PACK_CELLS_JSON_CAP, pos, truncated);
    send_json(conn, "200 OK", js);
    vPortFree(js);
}

/** GET /api/pack/balance?idx=N -- integrated balance transfer per cell. */
static void handle_pack_balance(struct netconn *conn, uint8_t idx)
{
    const size_t      bodyCap = PACK_CELLS_JSON_CAP - PACK_JSON_TAIL;
    sPackBalanceStats b;
    char             *js;
    size_t            pos = 0u;
    int               truncated = 0;
    uint8_t           c;
    int               r = Pack_BalanceStats(idx, &b);

    if (r == packErr_notSupported) {
        send_json(conn, "404 Not Found",
                  "{\"error\":\"this pack type has no balancer\"}");
        return;
    }
    if (r != packErr_ok) {
        send_json(conn, "404 Not Found", "{\"error\":\"no such pack\"}");
        return;
    }
    js = (char *)pvPortMalloc(PACK_CELLS_JSON_CAP);
    if (js == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    pos = Json_Cat(js, bodyCap, pos,
        "{\"idx\":%u,\"window_sec\":%u,\"activeSamples\":%u,"
        "\"cellCount\":%u,\"cells\":[",
        (unsigned)idx, (unsigned)b.window_sec,
        (unsigned)b.activeSamples, (unsigned)b.cellCount);
    for (c = 0u; c < b.cellCount; c++) {
        const size_t mark = pos;

        /* capacityDelta_mAh is RELATIVE TO THE PACK MEDIAN; negative means
         * the balancer keeps charging this cell, i.e. it holds less. */
        pos = Json_Cat(js, bodyCap, pos,
            "%s{\"in_mAs\":%d,\"out_mAs\":%d,\"capacityDelta_mAh\":%d}",
            (c == 0u) ? "" : ",", (int)b.in_mAs[c], (int)b.out_mAs[c],
            (int)b.capacityDelta_mAh[c]);

        if (pos >= bodyCap) {
            pos = mark;
            js[pos] = '\0';
            truncated = 1;
            break;
        }
    }
    pack_json_close(js, PACK_CELLS_JSON_CAP, pos, truncated);
    send_json(conn, "200 OK", js);
    vPortFree(js);
}

/** Report a pack config parse failure the way the Modbus one does: point at
 *  the offending pack and field rather than saying "invalid". */
static void send_pack_cfg_result(struct netconn *conn,
                                 const sPackCfgResult *res, int applied)
{
    char body[256];

    if (res->ok != 0) {
        (void)snprintf(body, sizeof(body),
                       "{\"ok\":true,\"packs\":%d,\"applied\":%s}",
                       Pack_Count(), applied ? "true" : "false");
        send_json(conn, "200 OK", body);
        return;
    }
    /* POINT AT THE OFFENDING PACK AND KEY, as the Modbus compiler does --
     * "invalid" is not a diagnosis an operator can act on. */
    {
        char fieldEsc[ESC_FIELD_LEN];

        (void)Json_Escape(fieldEsc, sizeof(fieldEsc), res->field);
        (void)snprintf(body, sizeof(body),
                       "{\"ok\":false,\"pack\":%d,\"field\":\"%s\","
                       "\"reason\":\"%s\"}",
                       res->packIdx, fieldEsc, res->reason);
    }
    send_json(conn, "422 Unprocessable Entity", body);
}

/** POST /api/pack/config[/verify] — upload a pack configuration.
 *
 *  Verify and apply share ONE parser and one result struct, so there is never
 *  a second validator that can disagree with the first (§10.10). */
static void handle_pack_cfg_post(struct netconn *conn, sConnStream *s,
                                 int apply)
{
    uint32_t       content_length = parse_content_length(req_buf);
    sPackCfgResult res;
    int            r;

    if ((content_length == 0u) || (content_length > 16u * 1024u)) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"Content-Length required (max 16 KB)\"}");
        return;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    sBodySource src = { s, content_length };
    (void)memset(&res, 0, sizeof(res));

    r = apply ? Pack_ConfigApply(body_source, &src, &res)
              : Pack_ConfigVerify(body_source, &src, &res);

    if (r == packErr_busy) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"a configuration is already being parsed\"}");
        return;
    }
    if ((r != packErr_ok) && (res.ok != 0)) {
        /* Parsed cleanly but could not be stored. */
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"could not persist the configuration\"}");
        return;
    }
    send_pack_cfg_result(conn, &res, apply);
}

/** Accumulate the exported document into a fixed buffer. */
typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t len;
} sPackExportSink;

static int pack_http_sink(void *ctx, const char *data, uint32_t len)
{
    sPackExportSink *sk = (sPackExportSink *)ctx;

    if ((sk->len + len) >= sk->cap) {
        return -1;                      /* refuse rather than truncate */
    }
    (void)memcpy(&sk->buf[sk->len], data, len);
    sk->len += len;
    return (int)len;
}

/** GET /api/pack/config — the active configuration, re-serialised.
 *
 *  Data-faithful, not byte-identical: what comes back out must parse to an
 *  identical sPackCfg, which is what the round-trip test asserts. */
static void handle_pack_cfg_get(struct netconn *conn)
{
    char           *body;
    sPackExportSink sk;

    body = (char *)pvPortMalloc(PACK_CELLS_JSON_CAP);
    if (body == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    sk.buf = body;
    sk.cap = PACK_CELLS_JSON_CAP;
    sk.len = 0u;

    if (Pack_ConfigExport(pack_http_sink, &sk) != packErr_ok) {
        vPortFree(body);
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"export failed\"}");
        return;
    }
    body[sk.len] = '\0';
    send_json(conn, "200 OK", body);
    vPortFree(body);
}

/** DELETE /api/pack/config — the board becomes unprovisioned. */
static void handle_pack_cfg_delete(struct netconn *conn)
{
    if (Pack_ConfigErase() != packErr_ok) {
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"erase failed\"}");
        return;
    }
    /* THERE IS NO BUILT-IN DEFAULT, so there is nothing to reset *to*: a pack
     * configuration describes hardware this board may not have. */
    send_json(conn, "200 OK",
              "{\"ok\":true,\"provisioned\":false}");
}

/* --------------------------------------------------------------------------
 * Battery cluster
 *
 * R4.9: board access is TUNNEL-ONLY, so "which pack is holding the limit down"
 * -- the question this module will be asked most -- has to be answerable from
 * a laptop over the tunnel.  A number the operator cannot see is a number they
 * cannot trust, and the cluster has no CLI surface by design.
 *
 * EVERY ENUM RENDERS AS A NAME, never a bare integer: the numeric `why` on
 * /api/pack/status needed a second lookup table in the reader's head, and that
 * is exactly the defect ePackAbsentReason had.
 * -------------------------------------------------------------------------- */

#define CLUSTER_JSON_CAP        4096u
/* Room for "],\"stats\":{...}}" and its NUL.  Every append below is made
 * against a cap that stops this short, so the tail always fits and what a
 * client receives is well-formed JSON whether or not everything fitted. */
#define CLUSTER_JSON_TAIL       640u
#define CLUSTER_NAME_ESC_LEN    (((CLUSTER_NAME_LEN - 1u) * 6u) + 1u)

/** Render a bitmask as an array of names.  A cluster alarm word an operator
 *  has to decode by hand is a diagnostic nobody uses. */
static size_t cluster_json_bits(char *js, size_t cap, size_t pos,
                                uint32_t bits, uint32_t highest,
                                const char *(*name)(uint32_t))
{
    uint32_t b;
    int      first = 1;

    pos = Json_Cat(js, cap, pos, "[");
    for (b = 1u; b <= highest; b <<= 1) {
        if ((bits & b) == 0u) {
            continue;
        }
        /* Emitted unescaped: cluster_cfg.c states as a constraint on its
         * tables that no name contains a '"' or a '\\', and the host test
         * asserts it. */
        pos = Json_Cat(js, cap, pos, "%s\"%s\"", first ? "" : ",", name(b));
        first = 0;
    }
    return Json_Cat(js, cap, pos, "]");
}

/** One direction of the four-number causal chain, so an adapter renders "why
 *  is the limit this" with no arithmetic of its own. */
static size_t cluster_json_dir(char *js, size_t cap, size_t pos,
                               const char *label, uint32_t loop_mA,
                               uint32_t derated_mA, uint32_t slewed_mA,
                               uint32_t published_mA, uint16_t loadMax_pm,
                               uint8_t state, uint8_t why, uint8_t bindingIdx)
{
    pos = Json_Cat(js, cap, pos,
        "\"%s\":{\"state\":\"%s\",\"loadMax_pm\":%u,"
        "\"loop_mA\":%u,\"derated_mA\":%u,\"slewed_mA\":%u,"
        "\"published_mA\":%u,\"why\":\"%s\",\"bindingPack\":",
        label, Cluster_LoopStateName(state), (unsigned)loadMax_pm,
        (unsigned)loop_mA, (unsigned)derated_mA, (unsigned)slewed_mA,
        (unsigned)published_mA, Cluster_LimitWhyName(why));
    /* null, not 255: at the start value NOBODY is binding, and a sentinel
     * index rendered as a number invites a reader to look up slot 255. */
    if (bindingIdx == CLUSTER_PACK_NONE) {
        pos = Json_Cat(js, cap, pos, "null}");
    } else {
        pos = Json_Cat(js, cap, pos, "%u}", (unsigned)bindingIdx);
    }
    return pos;
}

/** GET /api/cluster/status — the published figures, both loops and every
 *  member.  `?packs=0` omits the member array. */
static void handle_cluster_status(struct netconn *conn, int withPacks)
{
    const size_t   bodyCap = CLUSTER_JSON_CAP - CLUSTER_JSON_TAIL;
    sClusterOutput o;
    sClusterMember mem[CLUSTER_PACK_MAX];
    sClusterStats  st;
    char          *js;
    size_t         pos = 0u;
    uint8_t        written = 0u;
    uint8_t        i;
    int            truncated = 0;
    int            r;

    js = (char *)pvPortMalloc(CLUSTER_JSON_CAP);
    if (js == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    (void)Cluster_Stats(&st);
    /* ONE GENERATION for the output and the members, so a status page cannot
     * render a limit beside a share from a different tick. */
    r = Cluster_GetSnapshot(&o, mem, (uint8_t)CLUSTER_PACK_MAX, &written);

    if (r != cluErr_ok) {
        (void)memset(&o, 0, sizeof(o));
        written = 0u;
    }

    {
        char profile[CLUSTER_PROFILE_LEN];
        char profileEsc[(((CLUSTER_PROFILE_LEN - 1u) * 6u) + 1u)];

        (void)Cluster_ProfileToken(profile, sizeof(profile));
        (void)Json_Escape(profileEsc, sizeof(profileEsc), profile);

        pos = Json_Cat(js, bodyCap, pos,
            "{\"provisioned\":%s,\"ready\":%s,\"cond\":\"%s\",\"valid\":%s,"
            "\"age_ms\":%u,\"seq\":%u,\"members\":%d,\"online\":%u,"
            "\"profile\":\"%s\",",
            st.provisioned ? "true" : "false",
            (r == cluErr_ok) ? "true" : "false",
            Cluster_CondName(o.cond), o.valid ? "true" : "false",
            (unsigned)(osKernelGetTickCount() - o.tick_ms),
            (unsigned)o.seq, Cluster_Count(), (unsigned)o.onlineCnt,
            profileEsc);
    }

    pos = Json_Cat(js, bodyCap, pos,
        "\"published\":{\"voltage_mV\":%u,\"voltSpread_mV\":%u,"
        "\"current_mA\":%d,\"soc_pm\":%u,\"soh_pm\":%u,"
        "\"socConf_pm\":%u,\"sohConf_pm\":%u,"
        "\"remaining_mAh\":%u,\"capacity_mAh\":%u,\"nameplate_mAh\":%u,"
        "\"chargeLimit_mA\":%u,\"dischargeLimit_mA\":%u,"
        "\"chargeVoltLimit_mV\":%u,\"dischargeVoltLimit_mV\":%u,"
        "\"tempMax_dC\":%d,\"tempMin_dC\":%d,"
        "\"chargeAllowed\":%s,\"dischargeAllowed\":%s,\"alarms\":%u,",
        (unsigned)o.voltage_mV, (unsigned)o.voltSpread_mV,
        (int)o.current_mA, (unsigned)o.soc_pm, (unsigned)o.soh_pm,
        (unsigned)o.socConf_pm, (unsigned)o.sohConf_pm,
        (unsigned)o.remaining_mAh, (unsigned)o.capacity_mAh,
        (unsigned)o.nameplate_mAh,
        (unsigned)o.chargeLimit_mA, (unsigned)o.dischargeLimit_mA,
        (unsigned)o.chargeVoltLimit_mV, (unsigned)o.dischargeVoltLimit_mV,
        (int)o.tempMax_dC, (int)o.tempMin_dC,
        o.chargeAllowed ? "true" : "false",
        o.dischargeAllowed ? "true" : "false",
        (unsigned)o.alarms);

    /* `alarms` is the PACK MODULE'S vocabulary, OR'd over online packs and
     * republished as a mask.  The cluster is forbidden to spell ePackAlarm
     * (design constraint 13) and does not; this adapter renders it through
     * the owning module's accessor, which is what the constraint was waiting
     * for. */
    pos = Json_Cat(js, bodyCap, pos, "\"alarmNames\":");
    pos = pack_alarm_names(js, bodyCap, pos, o.alarms);
    pos = Json_Cat(js, bodyCap, pos, ",\"clusterAlarms\":");
    pos = cluster_json_bits(js, bodyCap, pos, o.clusterAlarms,
                            (uint32_t)cluAlarm_packCfgChanged,
                            Cluster_AlarmName);
    /* WHAT IS NOT BEING PUBLISHED, by name.  A cleared field bit means the
     * value reads zero and MEANS NOTHING -- a zero charge-voltage limit is not
     * "no limit", it is an instruction to stop charging -- so the absence has
     * to be as visible as the number. */
    pos = Json_Cat(js, bodyCap, pos, ",\"missing\":");
    pos = cluster_json_bits(js, bodyCap, pos,
                            (uint32_t)(~o.fields) &
                            ((uint32_t)cluField_switches * 2u - 1u),
                            (uint32_t)cluField_switches, Cluster_FieldName);
    pos = Json_Cat(js, bodyCap, pos, "},\"loop\":{\"lastRestart\":\"%s\",",
                   Cluster_RestartName(o.lastRestart));
    pos = cluster_json_dir(js, bodyCap, pos, "charge",
                           o.chargeLoop_mA, o.chargeDerated_mA,
                           o.chargeSlewed_mA, o.chargeLimit_mA,
                           o.chargeLoadMax_pm, o.chargeLoopState,
                           o.chargeWhy, o.chargeBindingIdx);
    pos = Json_Cat(js, bodyCap, pos, ",");
    pos = cluster_json_dir(js, bodyCap, pos, "discharge",
                           o.dischargeLoop_mA, o.dischargeDerated_mA,
                           o.dischargeSlewed_mA, o.dischargeLimit_mA,
                           o.dischargeLoadMax_pm, o.dischargeLoopState,
                           o.dischargeWhy, o.dischargeBindingIdx);
    pos = Json_Cat(js, bodyCap, pos, "},\"packs\":[");

    if (withPacks != 0) {
        int first = 1;

        for (i = 0u; i < written; i++) {
            const sClusterMember *m = &mem[i];
            char   name[CLUSTER_NAME_LEN];
            char   nameEsc[CLUSTER_NAME_ESC_LEN];
            size_t mark = pos;

            (void)Cluster_MemberName(i, name, sizeof(name));
            /* Operator text that reached flash through the config parser, so
             * it is not safe to interpolate raw. */
            (void)Json_Escape(nameEsc, sizeof(nameEsc), name);

            pos = Json_Cat(js, bodyCap, pos,
                "%s{\"slot\":%u,\"name\":\"%s\",\"packIdx\":",
                first ? "" : ",", (unsigned)i, nameEsc);
            if (m->packIdx == CLUSTER_PACK_NONE) {
                pos = Json_Cat(js, bodyCap, pos, "null");
            } else {
                pos = Json_Cat(js, bodyCap, pos, "%u", (unsigned)m->packIdx);
            }
            pos = Json_Cat(js, bodyCap, pos,
                ",\"state\":\"%s\",\"why\":\"%s\","
                "\"current_mA\":%d,\"share_pm\":%u,\"load_pm\":%u,"
                "\"soc_pm\":%u,\"chargeLimit_mA\":%u,"
                "\"dischargeLimit_mA\":%u,\"elecAge_ms\":%u,\"flags\":",
                Cluster_MemberStateName(m->state),
                Cluster_MemberWhyName(m->why),
                (int)m->current_mA, (unsigned)m->share_pm,
                (unsigned)m->load_pm, (unsigned)m->soc_pm,
                (unsigned)m->chargeLimit_mA, (unsigned)m->dischargeLimit_mA,
                (unsigned)m->elecAge_ms);
            pos = cluster_json_bits(js, bodyCap, pos, m->flags,
                                    (uint32_t)cluMemFlag_limitSaturated,
                                    Cluster_MemberFlagName);
            pos = Json_Cat(js, bodyCap, pos, "}");

            /* Whole object or none: a member half-written into a full buffer
             * is unparseable, which is worse than a missing member. */
            if (pos >= bodyCap) {
                pos = mark;
                js[pos] = '\0';
                truncated = 1;
                break;
            }
            first = 0;
        }
    }

    pos = Json_Cat(js, CLUSTER_JSON_CAP, pos,
                   truncated ? "],\"truncated\":true," : "],");
    /* bindingSample* IS EXPECTED TO BE A SMALL FRACTION OF ticks, and on a
     * quiet site zero: the loop only learns while something is actually asking
     * the battery for current.  It is reported so that is visible rather than
     * mistaken for a fault -- and so nobody "fixes" it by lowering
     * bindFrac_pm, which the parser refuses for exactly that reason. */
    (void)Json_Cat(js, CLUSTER_JSON_CAP, pos,
        "\"stats\":{\"ticks\":%u,\"publishes\":%u,\"packReadFail\":%u,"
        "\"nameUnresolved\":%u,\"packCfgSkip\":%u,"
        "\"noParticipantChg\":%u,\"noParticipantDsg\":%u,"
        "\"bindingSampleChg\":%u,\"bindingSampleDsg\":%u,"
        "\"restartChg\":%u,\"restartDsg\":%u,"
        "\"stepClamped\":%u,\"slewLimited\":%u,"
        "\"forbiddenChg\":%u,\"forbiddenDsg\":%u,\"voltLimitMissing\":%u,"
        "\"divergeSoc\":%u,\"divergeShare\":%u,\"sanitised\":%u,"
        "\"getBusy\":%u,\"getNotReady\":%u,\"cfgPending\":%s}}",
        (unsigned)st.ticks, (unsigned)st.publishes,
        (unsigned)st.packReadFailCnt, (unsigned)st.nameUnresolvedCnt,
        (unsigned)st.packCfgSkipCnt,
        (unsigned)st.noParticipantChgCnt, (unsigned)st.noParticipantDsgCnt,
        (unsigned)st.bindingSampleChgCnt, (unsigned)st.bindingSampleDsgCnt,
        (unsigned)st.restartChgCnt, (unsigned)st.restartDsgCnt,
        (unsigned)st.stepClampedCnt, (unsigned)st.slewLimitedCnt,
        (unsigned)st.forbiddenChgCnt, (unsigned)st.forbiddenDsgCnt,
        (unsigned)st.voltLimitMissingCnt,
        (unsigned)st.divergeSocCnt, (unsigned)st.divergeShareCnt,
        (unsigned)st.sanitisedCnt,
        (unsigned)st.getBusyCnt, (unsigned)st.getNotReadyCnt,
        st.cfgPending ? "true" : "false");

    send_json(conn, "200 OK", js);
    vPortFree(js);
}

/** Report a cluster config parse failure by member index and key. */
static void send_cluster_cfg_result(struct netconn *conn,
                                    const sClusterCfgResult *res, int applied)
{
    /* Sized so the compiler can PROVE the worst case fits: the 23-character
     * field at its \u00XX escape expansion is 138 bytes, the reason is 63,
     * and the literal and index are ~55.  A truncation warning here is not
     * cosmetic — the reply is what tells an operator which key they got
     * wrong. */
    char body[320];

    if (res->ok != 0) {
        /* 202, not 200, and the difference is load-bearing: NOTHING MOVES NOW.
         * A staged configuration is adopted by the next tick, and an operator
         * who read 200 would reasonably re-GET the status expecting the new
         * membership. */
        (void)snprintf(body, sizeof(body),
                       "{\"ok\":true,\"members\":%u,\"staged\":%s}",
                       (unsigned)res->members, applied ? "true" : "false");
        send_json(conn, applied ? "202 Accepted" : "200 OK", body);
        return;
    }
    {
        char fieldEsc[(((sizeof(res->field) - 1u) * 6u) + 1u)];

        (void)Json_Escape(fieldEsc, sizeof(fieldEsc), res->field);
        (void)snprintf(body, sizeof(body),
                       "{\"ok\":false,\"member\":%d,\"field\":\"%s\","
                       "\"reason\":\"%s\"}",
                       res->memberIdx, fieldEsc, res->reason);
    }
    send_json(conn, "422 Unprocessable Entity", body);
}

/** POST /api/cluster/config[/verify].  Verify and apply share ONE parser and
 *  one result struct, so there is never a second validator that can disagree
 *  with the first. */
static void handle_cluster_cfg_post(struct netconn *conn, sConnStream *s,
                                    int apply)
{
    uint32_t          content_length = parse_content_length(req_buf);
    sClusterCfgResult res;
    int               r;

    if ((content_length == 0u) || (content_length > 8u * 1024u)) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"Content-Length required (max 8 KB)\"}");
        return;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    {
        sBodySource src = { s, content_length };

        (void)memset(&res, 0, sizeof(res));
        r = apply ? Cluster_ConfigApply(body_source, &src, &res)
                  : Cluster_ConfigVerify(body_source, &src, &res);
    }

    if (r == cluErr_busy) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"a configuration is already staged\"}");
        return;
    }
    if (r == cluErr_transport) {
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"could not persist the configuration\"}");
        return;
    }
    send_cluster_cfg_result(conn, &res, apply);
}

/** GET /api/cluster/config — the active configuration, re-serialised.
 *  Data-faithful, not byte-identical. */
static void handle_cluster_cfg_get(struct netconn *conn)
{
    char           *body;
    sPackExportSink sk;

    body = (char *)pvPortMalloc(1024u);
    if (body == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    sk.buf = body;
    sk.cap = 1024u;
    sk.len = 0u;

    if (Cluster_ConfigExport(pack_http_sink, &sk) != cluErr_ok) {
        vPortFree(body);
        send_json(conn, "409 Conflict",
                  "{\"error\":\"unprovisioned\"}");
        return;
    }
    body[sk.len] = '\0';
    send_json(conn, "200 OK", body);
    vPortFree(body);
}

/** DELETE /api/cluster/config — the board becomes unprovisioned and the
 *  published limits fall to zero at the next tick. */
static void handle_cluster_cfg_delete(struct netconn *conn)
{
    if (Cluster_ConfigErase() != cluErr_ok) {
        send_json(conn, "503 Service Unavailable",
                  "{\"error\":\"erase failed\"}");
        return;
    }
    /* THERE IS NO BUILT-IN DEFAULT, so there is nothing to reset *to*: a
     * cluster configuration names packs this board may not have. */
    send_json(conn, "200 OK", "{\"ok\":true,\"provisioned\":false}");
}

/* --------------------------------------------------------------------------
 * CAN bridge
 *
 * The CLI reaches this module over USB CDC / UART1 only, i.e. with physical
 * access — which on a tunnel-only board is a site visit.  So everything the
 * bridge can be told to do has to be reachable here as well, including the
 * mode change that turns the board into the inverter's BMS.
 * -------------------------------------------------------------------------- */

#define CAN_JSON_CAP        3072u

/* Body cap, as everywhere else here: the tail is appended against the full
 * size so it always fits.  The CAN tails carry counters, so this is wider
 * than the plain "]}" reserve. */
#define CAN_JSON_BODY_CAP   (CAN_JSON_CAP - 48u)
#define CAN_TRACE_DEFAULT   32

/** 8 payload bytes as hex, always DLC-long. */
static void can_hex(char *out, const uint8_t *data, uint8_t dlc)
{
    static const char digits[] = "0123456789ABCDEF";
    uint8_t n = (dlc > 8u) ? 8u : dlc;

    for (uint8_t i = 0u; i < n; i++) {
        out[i * 2u]      = digits[(data[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = digits[data[i] & 0x0Fu];
    }
    out[n * 2u] = '\0';
}

static size_t can_bus_json(char *buf, size_t cap, size_t off, eCanBus bus)
{
    sCanBusStats st;
    const size_t mark = off;

    if (CanBus_GetStats(bus, &st) != 0) {
        return off;
    }
    off = Json_Cat(buf, cap, off,
        "{\"bus\":%u,\"running\":%s,\"bitrate_bps\":%lu,"
        "\"rx\":%lu,\"tx_done\":%lu,\"tx_accepted\":%lu,\"tx_dropped\":%lu,"
        "\"rx_overrun\":%lu,\"errors\":%lu,\"last_error\":\"0x%08lX\","
        "\"bus_off_count\":%lu,\"bus_off\":%s,\"rec\":%u,\"tec\":%u,"
        "\"tx_queue\":%u,\"tx_queue_peak\":%u}",
        (unsigned)bus + 1u, st.running ? "true" : "false",
        (unsigned long)st.bitrate_bps,
        (unsigned long)st.rxCnt, (unsigned long)st.txDoneCnt,
        (unsigned long)st.txAcceptedCnt, (unsigned long)st.txDroppedCnt,
        (unsigned long)st.rxOverrunCnt, (unsigned long)st.errorCnt,
        (unsigned long)st.lastError, (unsigned long)st.busOffCnt,
        st.busOff ? "true" : "false", st.rxErrorCnt, st.txErrorCnt,
        st.txQueueDepth, st.txQueuePeak);

    /* Whole object or none: half a bus is not a smaller answer, it is an
     * unparseable one. */
    if (off >= cap) {
        buf[mark] = '\0';
        return mark;
    }
    return off;
}

/* GET /api/can/status — mode, roles, forwarding counters and both cells. */
static void handle_can_status(struct netconn *conn)
{
    const size_t     canBodyCap = CAN_JSON_BODY_CAP;
    sCanBridgeStatus br;
    sCanMonStats     mon;
    char            *buf;
    size_t           off;

    if (CanBridge_GetStatus(&br) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"bridge state unavailable\"}");
        return;
    }
    CanMon_GetStats(&mon);

    buf = (char *)pvPortMalloc(CAN_JSON_CAP);
    if (buf == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }

    off = Json_Cat(buf, canBodyCap, 0u,
        "{\"mode\":\"%s\",\"battery_bus\":%u,\"inverter_bus\":%u,"
        "\"bitrate_bps\":%lu,"
        "\"source\":{\"bound\":%s,\"emits\":%lu,\"period_ms\":%lu},"
        "\"override\":{\"all\":%s,\"count\":%u,\"ids\":[",
        CanBridge_ModeName((eCanBrMode)br.mode),
        (unsigned)br.batteryBus + 1u, (unsigned)br.inverterBus + 1u,
        (unsigned long)br.bitrate_bps,
        br.sourceBound ? "true" : "false",
        (unsigned long)br.sourceEmitCnt, (unsigned long)br.sourcePeriod_ms,
        br.overrideAll ? "true" : "false", br.overrideCnt);
    for (uint8_t i = 0u; i < br.overrideCnt; i++) {
        off = Json_Cat(buf, canBodyCap, off, "%s\"0x%03lX\"",
                       (i == 0u) ? "" : ",",
                       (unsigned long)br.overrideId[i]);
    }

    off = Json_Cat(buf, canBodyCap, off,
        "]},\"forward\":{"
        "\"to_inverter\":{\"forwarded\":%lu,\"suppressed\":%lu,\"dropped\":%lu},"
        "\"to_battery\":{\"forwarded\":%lu,\"suppressed\":%lu,\"dropped\":%lu}},"
        "\"monitor\":{\"recorded\":%lu,\"id_overflow\":%lu,\"tracing\":%s,"
        "\"trace_dropped\":%lu},\"buses\":[",
        (unsigned long)br.toInverter.forwardedCnt,
        (unsigned long)br.toInverter.suppressedCnt,
        (unsigned long)br.toInverter.droppedCnt,
        (unsigned long)br.toBattery.forwardedCnt,
        (unsigned long)br.toBattery.suppressedCnt,
        (unsigned long)br.toBattery.droppedCnt,
        (unsigned long)mon.recordedCnt, (unsigned long)mon.idOverflowCnt,
        mon.tracing ? "true" : "false",
        (unsigned long)mon.traceDroppedCnt);

    off = can_bus_json(buf, canBodyCap, off, canBus_1);
    off = Json_Cat(buf, canBodyCap, off, ",");
    off = can_bus_json(buf, canBodyCap, off, canBus_2);
    (void)Json_Cat(buf, CAN_JSON_CAP, off, "]}");

    send_json(conn, "200 OK", buf);
    vPortFree(buf);
}

/* GET /api/can/traffic?bus=N — the identifier register of one bus. */
static void handle_can_traffic(struct netconn *conn, eCanBus bus)
{
    const size_t canBodyCap = CAN_JSON_BODY_CAP;
    uint32_t now_ms = HAL_GetTick();
    uint8_t  count  = CanMon_IdCount(bus);
    char    *buf;
    size_t   off;
    uint8_t  written = 0u;
    bool     truncated = false;

    buf = (char *)pvPortMalloc(CAN_JSON_CAP);
    if (buf == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    off = Json_Cat(buf, canBodyCap, 0u,
                   "{\"bus\":%u,\"count\":%u,\"ids\":[",
                   (unsigned)bus + 1u, count);

    for (uint8_t i = 0u; i < count; i++) {
        sCanMonId    r;
        char         hex[17];
        const size_t mark = off;

        if (CanMon_GetIdAt(bus, i, &r) != 0) {
            break;
        }
        can_hex(hex, r.data, r.dlc);
        off = Json_Cat(buf, canBodyCap, off,
            "%s{\"id\":\"0x%03lX\",\"ext\":%s,\"rtr\":%s,\"dlc\":%u,"
            "\"rx\":%lu,\"tx\":%lu,\"changes\":%lu,\"age_ms\":%lu,"
            "\"min_gap_ms\":%lu,\"max_gap_ms\":%lu,\"data\":\"%s\"}",
            (written == 0u) ? "" : ",", (unsigned long)r.id,
            r.ext ? "true" : "false", r.rtr ? "true" : "false", r.dlc,
            (unsigned long)r.rxCnt, (unsigned long)r.txCnt,
            (unsigned long)r.changeCnt,
            (unsigned long)(now_ms - r.lastStamp_ms),
            (unsigned long)r.minGap_ms, (unsigned long)r.maxGap_ms, hex);
        if (off >= canBodyCap) {
            off = mark;
            buf[off] = '\0';
            truncated = true;
            break;
        }
        written++;
    }

    (void)Json_Cat(buf, CAN_JSON_CAP, off, "],\"returned\":%u,"
                   "\"truncated\":%s}", written,
                   truncated ? "true" : "false");
    send_json(conn, "200 OK", buf);
    vPortFree(buf);
}

/* GET /api/can/trace?n=N — the newest N frames of the ring, oldest first. */
static void handle_can_trace(struct netconn *conn)
{
    const size_t canBodyCap = CAN_JSON_BODY_CAP;
    sCanMonStats mon;
    uint16_t     total = CanMon_TraceCount();
    int          want  = query_int("n", CAN_TRACE_DEFAULT);
    uint16_t     first = 0u;
    uint16_t     written = 0u;
    char        *buf;
    size_t       off;

    CanMon_GetStats(&mon);
    if ((want > 0) && (total > (uint16_t)want)) {
        first = (uint16_t)(total - (uint16_t)want);
    }

    buf = (char *)pvPortMalloc(CAN_JSON_CAP);
    if (buf == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }
    off = Json_Cat(buf, canBodyCap, 0u,
        "{\"tracing\":%s,\"held\":%u,\"dropped\":%lu,\"frames\":[",
        mon.tracing ? "true" : "false", total,
        (unsigned long)mon.traceDroppedCnt);

    for (uint16_t i = first; i < total; i++) {
        sCanMonTrace t;
        char         hex[17];
        const size_t mark = off;

        if (CanMon_GetTraceAt(i, &t) != 0) {
            break;
        }
        can_hex(hex, t.data, t.dlc);
        off = Json_Cat(buf, canBodyCap, off,
            "%s{\"t_ms\":%lu,\"bus\":%u,\"dir\":\"%s\",\"id\":\"0x%03lX\","
            "\"dlc\":%u,\"data\":\"%s\"}",
            (written == 0u) ? "" : ",", (unsigned long)t.stamp_ms,
            (unsigned)t.bus + 1u,
            (t.dir == (uint8_t)canDir_rx) ? "rx" : "tx",
            (unsigned long)t.id, t.dlc, hex);
        if (off >= canBodyCap) {
            off = mark;
            buf[off] = '\0';
            break;
        }
        written++;
    }

    (void)Json_Cat(buf, CAN_JSON_CAP, off, "],\"returned\":%u}", written);
    send_json(conn, "200 OK", buf);
    vPortFree(buf);
}

/* POST /api/can/mode?mode=<off|monitor|bridge|bms>[&bitrate=N]
 *
 * One route for start, stop and the live break, because they are one decision:
 * `off` stops the buses, anything else starts them if they are down and
 * changes policy in place if they are up. */
/* GET /api/can/log/status */
static void handle_can_log_status(struct netconn *conn)
{
    sCanLogStatus st;
    char          buf[512];

    if (CanLog_GetStatus(&st) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"log state unavailable\"}");
        return;
    }
    snprintf(buf, sizeof(buf),
        "{\"mode\":\"%s\",\"state\":\"%s\",\"min_gap_ms\":%lu,"
        "\"heartbeat_ms\":%lu,\"capacity_recs\":%lu,\"held_recs\":%lu,"
        "\"oldest_rec\":%lu,\"next_rec\":%lu,\"staged\":%u,"
        "\"stage_depth\":%u,\"area_size_bytes\":%lu,\"boot_id\":%lu,"
        "\"dropped\":%lu,\"stalled\":%lu,\"write_errors\":%lu}",
        CanLog_ModeName((eCanLogMode)st.mode),
        CanLog_StateName((eCanLogState)st.state),
        (unsigned long)st.minGap_ms, (unsigned long)st.heartbeat_ms,
        (unsigned long)st.capacityRecs, (unsigned long)st.heldRecs,
        (unsigned long)st.oldestRec, (unsigned long)st.nextRec,
        st.staged, (unsigned)CANLOG_STAGE_DEPTH,
        (unsigned long)st.areaSize_bytes, (unsigned long)st.bootId,
        (unsigned long)st.droppedCnt, (unsigned long)st.stalledCnt,
        (unsigned long)st.writeErrCnt);
    send_json(conn, "200 OK", buf);
}

/* POST /api/can/log/mode?mode=<off|changes|all>[&gap_ms=N][&heartbeat_ms=N]
 *
 * Runtime policy only -- not persisted, exactly like the bridge's own mode
 * (docs/design_can_bridge.md §9 item 1).  Omitted gap/heartbeat keep the
 * module's compiled defaults, not whatever was previously set, so a caller
 * that only wants to switch mode is not required to also restate them. */
static void handle_can_log_mode(struct netconn *conn)
{
    char        name[16];
    eCanLogMode mode;
    uint32_t    minGap_ms    = (uint32_t)query_int("gap_ms", CANLOG_MIN_GAP_MS);
    uint32_t    heartbeat_ms = (uint32_t)query_int("heartbeat_ms",
                                                   CANLOG_HEARTBEAT_MS);

    if (query_token("mode", name, sizeof(name)) <= 0) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"mode is required: off|changes|all\"}");
        return;
    }
    if (CanLog_ModeFromName(name, &mode) != 0) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"unknown mode\"}");
        return;
    }
    if (CanLog_SetPolicy(mode, minGap_ms, heartbeat_ms) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"policy change refused\"}");
        return;
    }
    handle_can_log_status(conn);
}

/* GET /api/can/log/read?rec=N -- one record by its global number. */
static void handle_can_log_read(struct netconn *conn)
{
    uint32_t   recNo = (uint32_t)query_int("rec", 0);
    sCanLogRec rec;
    char       hex[17];
    char       buf[256];
    int        rc = CanLog_ReadRec(recNo, &rec);

    if (rc != 0) {
        char err[96];

        snprintf(err, sizeof(err),
                 "{\"error\":\"record %lu unavailable (%d)\"}",
                 (unsigned long)recNo, rc);
        send_json(conn, "404 Not Found", err);
        return;
    }
    can_hex(hex, rec.data, CANLOG_META_DLC(rec.meta));
    snprintf(buf, sizeof(buf),
        "{\"rec\":%lu,\"t_ms\":%lu,\"bus\":%u,\"dir\":\"%s\","
        "\"id\":\"0x%03X\",\"ext\":%s,\"rtr\":%s,\"dlc\":%u,\"data\":\"%s\"}",
        (unsigned long)recNo, (unsigned long)rec.stamp_ms,
        (unsigned)CANLOG_META_BUS(rec.meta) + 1u,
        CANLOG_META_DIR(rec.meta) ? "tx" : "rx", rec.id,
        CANLOG_META_EXT(rec.meta) ? "true" : "false",
        CANLOG_META_RTR(rec.meta) ? "true" : "false",
        CANLOG_META_DLC(rec.meta), hex);
    send_json(conn, "200 OK", buf);
}

/* POST /api/can/log/wipe -- discard the flash trace and start at record 0. */
static void handle_can_log_wipe(struct netconn *conn)
{
    if (CanLog_Wipe() != 0) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"no usable flash area\"}");
        return;
    }
    send_json(conn, "200 OK", "{\"status\":\"wipe requested\"}");
}

static void handle_can_mode(struct netconn *conn)
{
    char       name[16];
    eCanBrMode mode;
    uint32_t   bitrate_bps = (uint32_t)query_int("bitrate", 0);

    if (query_token("mode", name, sizeof(name)) <= 0) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"mode is required: off|monitor|bridge|bms\"}");
        return;
    }
    if (CanBridge_ModeFromName(name, &mode) != 0) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"unknown mode\"}");
        return;
    }

    if (mode == canBrMode_off) {
        (void)CanBridge_Stop();
    } else if (CanBridge_GetMode() == canBrMode_off) {
        if (CanBridge_Start(mode, bitrate_bps) != 0) {
            send_json(conn, "500 Internal Server Error",
                      "{\"error\":\"could not bring the buses up\"}");
            return;
        }
    } else if (CanBridge_SetMode(mode) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"mode change refused\"}");
        return;
    } else {
        /* changed in place */
    }

    handle_can_status(conn);
}

/* POST /api/can/trace/<on|off> */
static void handle_can_trace_enable(struct netconn *conn, int on)
{
    CanMon_TraceEnable(on);
    send_json(conn, "200 OK", on ? "{\"tracing\":true}" : "{\"tracing\":false}");
}

/* POST /api/can/reset — zero every counter, keep the wire up. */
static void handle_can_reset(struct netconn *conn)
{
    CanBus_ResetStats();
    CanBridge_ResetStats();
    CanMon_Reset();
    send_json(conn, "200 OK", "{\"status\":\"cleared\"}");
}

/* POST /api/can/send?bus=N&id=351&data=3002F401F401C001
 *
 * `id` and `data` are hex, `data` is 0..8 bytes.  This is a diagnostic: it is
 * how an inverter's reaction to one frame can be tried from a laptop over the
 * tunnel, without the board pretending to be a battery first. */
static void handle_can_send(struct netconn *conn)
{
    char      idTok[12];
    char      dataTok[20];
    sCanFrame frame;
    int       bus = query_int("bus", 0);
    int       len;

    memset(&frame, 0, sizeof(frame));

    if ((bus < 1) || (bus > (int)canBus_last)) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"bus must be 1 or 2\"}");
        return;
    }
    if (query_token("id", idTok, sizeof(idTok)) <= 0) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"id is required (hex)\"}");
        return;
    }
    frame.id = (uint32_t)strtoul(idTok, NULL, 16);
    if (frame.id > 0x1FFFFFFFu) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"id out of range\"}");
        return;
    }
    frame.ext = (frame.id > 0x7FFu) ? 1u : 0u;
    frame.bus = (uint8_t)(bus - 1);

    len = query_token("data", dataTok, sizeof(dataTok));
    if (len > 0) {
        if (((len % 2) != 0) || (len > 16)) {
            send_json(conn, "422 Unprocessable Entity",
                      "{\"error\":\"data must be 0..8 hex bytes\"}");
            return;
        }
        for (int i = 0; i < (len / 2); i++) {
            char byte[3] = { dataTok[i * 2], dataTok[i * 2 + 1], '\0' };
            char *endp = NULL;

            frame.data[i] = (uint8_t)strtoul(byte, &endp, 16);
            if ((endp == NULL) || (*endp != '\0')) {
                send_json(conn, "422 Unprocessable Entity",
                          "{\"error\":\"data is not hex\"}");
                return;
            }
        }
        frame.dlc = (uint8_t)(len / 2);
    }

    if (CanBus_Send(&frame) != 0) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"bus is down or its queue is full\"}");
        return;
    }
    send_json(conn, "200 OK", "{\"sent\":true}");
}

static void handle_wg_status(struct netconn *conn)
{
    wg_status_json(resp_buf, sizeof(resp_buf));
    send_json(conn, "200 OK", resp_buf);
}

static void handle_wg_config(struct netconn *conn, sConnStream *s)
{
    uint32_t content_length = parse_content_length(req_buf);
    char     body[256];
    uint32_t got = 0u;
    uint8_t  ip[4];
    uint8_t  mask[4];
    uint8_t  ipEp[4];
    int      haveIp;
    int      haveMask;
    int      haveEp;
    uint32_t port = 0u;
    int      save;

    if (content_length == 0u || content_length >= sizeof(body)) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"JSON body required (max 255 bytes)\"}");
        return;
    }

    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    while (got < content_length) {
        int ch = cs_read_byte(s);
        if (ch < 0) {
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"incomplete body\"}");
            return;
        }
        body[got++] = (char)ch;
    }
    body[got] = '\0';

    haveIp   = json_ipv4(body, "tunnel_ip",   ip);
    haveMask = json_ipv4(body, "tunnel_mask", mask);
    haveEp   = json_ipv4(body, "endpoint_ip", ipEp);
    /* Unconditional: short-circuiting this into the check below would drop a
     * port supplied alongside a tunnel address. */
    (void)json_uint(body, "endpoint_port", &port);

    if (!haveIp && !haveMask && !haveEp && port == 0u) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"nothing to change; expected tunnel_ip, "
                  "tunnel_mask, endpoint_ip or endpoint_port\"}");
        return;
    }

    /* A mask alone is meaningless — WgLink_SetTunnelIp takes the pair. */
    if (haveMask && !haveIp) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"tunnel_mask requires tunnel_ip\"}");
        return;
    }

    save = json_bool(body, "save", 1);

    if (haveIp) {
        if (WgLink_SetTunnelIp(ip, haveMask ? mask : NULL) != 0) {
            send_json(conn, "422 Unprocessable Entity",
                      "{\"error\":\"invalid tunnel address\"}");
            return;
        }
    }

    if (haveEp || port != 0u) {
        const sWgLinkCfg *cur = WgLink_ActiveCfg();
        if (!haveEp) {
            memcpy(ipEp, cur->endpointIp, sizeof(ipEp));
        }
        if (port == 0u) {
            port = cur->endpointPort;
        }
        if (WgLink_SetEndpoint(ipEp, (uint16_t)port) != 0) {
            send_json(conn, "422 Unprocessable Entity",
                      "{\"error\":\"invalid endpoint\"}");
            return;
        }
    }

    if (save && WgLink_SaveCfg() != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"config applied but could not be saved\"}");
        return;
    }

    TRice("WG: config updated over HTTP (saved=%u)\n", (unsigned)save);

    /* Report the resulting state rather than an ack — the caller needs to see
     * what actually took effect, especially after a restart. */
    wg_status_json(resp_buf, sizeof(resp_buf));
    send_json(conn, "200 OK", resp_buf);
}

static void handle_wg_config_reset(struct netconn *conn)
{
    if (WgLink_ResetCfg() != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"could not clear stored config\"}");
        return;
    }
    TRice("WG: stored config erased\n");
    wg_status_json(resp_buf, sizeof(resp_buf));
    send_json(conn, "200 OK", resp_buf);
}

/* Trice health, for when the log stream itself is what is broken.
 *
 * Everything deferred — UART, USB CDC and UDP alike — is flushed by
 * TriceTransfer() in triceTask, which only runs while USART3's DMA reports
 * ready.  So a stuck UART state silences every sink at once, and that is
 * indistinguishable from "nothing is being logged" unless the counters are
 * exposed somewhere that does not itself depend on Trice. */
static size_t trice_dests_array(char *buf, size_t cap, size_t pos);

static void handle_trice_status(struct netconn *conn)
{
    uint32_t udpSent = 0u, udpFailed = 0u;
    unsigned usbTxState = 255u;
    size_t   off;

    Trice_UdpGetStats(&udpSent, &udpFailed);
    {
        extern USBD_HandleTypeDef hUsbDeviceFS;
        USBD_CDC_HandleTypeDef *cdc =
            (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
        if (cdc != NULL) {
            usbTxState = (unsigned)cdc->TxState;
        }
    }

    /* huart3 and the Trice counters are declared by usart.h / trice.h. */
    off = (size_t)snprintf(resp_buf, sizeof(resp_buf),
        "{\"uart3_gstate\":%u,\"uart3_ready\":%s,"
        "\"aux_fn_registered\":%s,\"consumer_pending\":%u,"
        "\"udp_consumer_registered\":%s,\"udp_pcb_ok\":%s,"
        "\"udp_sent\":%u,\"udp_failed\":%u,\"usb_tx_state\":%u,"
        "\"free_heap\":%u,"
        "\"trice_errors\":%u,\"deferred_overflow\":%u,"
        "\"half_buffer_depth_max\":%u,\"dests\":",
        (unsigned)huart3.gState,
        MX_USART3_Ready() ? "true" : "false",
        (UserNonBlockingDeferredWrite8AuxiliaryFn != NULL) ? "true" : "false",
        (unsigned)TriceConsumer_Pending(),
        TriceConsumer_IsRegistered(TRICE_CONSUMER_UDP) ? "true" : "false",
        Trice_UdpIsReady() ? "true" : "false",
        (unsigned)udpSent, (unsigned)udpFailed, usbTxState,
        (unsigned)xPortGetFreeHeapSize(),
        (unsigned)TriceErrorCount,
        (unsigned)TriceDeferredOverflowCount,
        (unsigned)TriceHalfBufferDepthMax);

    off = trice_dests_array(resp_buf, RESP_BODY_CAP, off);
    (void)Json_Cat(resp_buf, sizeof(resp_buf), off, "}");
    send_json(conn, "200 OK", resp_buf);
}

/* Render the Trice destination list as a JSON array: ["a.b.c.d",...] into
 * `buf` at `pos`, the Json_Cat shape so it composes with its callers. */
static size_t trice_dests_array(char *buf, size_t cap, size_t pos)
{
    ip_addr_t list[TRICE_UDP_MAX_DEST];
    uint32_t  n = Trice_UdpGetDests(list, TRICE_UDP_MAX_DEST);
    uint32_t  i;

    /* One byte of the cap is held back for the ']' so the array always
     * closes -- an unterminated one is not a shorter list, it is a broken
     * document. */
    pos = Json_Cat(buf, cap - 1u, pos, "[");
    for (i = 0u; i < n; i++) {
        pos = Json_Cat(buf, cap - 1u, pos, "%s\"%u.%u.%u.%u\"",
                       (i == 0u) ? "" : ",",
                       (unsigned)ip4_addr1(&list[i]),
                       (unsigned)ip4_addr2(&list[i]),
                       (unsigned)ip4_addr3(&list[i]),
                       (unsigned)ip4_addr4(&list[i]));
    }
    return Json_Cat(buf, cap, pos, "]");
}

/* Manage the Trice UDP destination list explicitly.
 *
 * Slot 0 is the LAN broadcast: it needs no configuration and, by definition,
 * never leaves the LAN.  This endpoint is for entries a connection cannot
 * express — a collector that is not the caller, or a second broadcast address.
 * A listener registering *itself* should use /api/trice/subscribe instead,
 * which needs no body and cannot name the wrong address.
 *
 * POST   {"ip":"a.b.c.d"}  add (or refresh) an entry
 * DELETE {"ip":"a.b.c.d"}  remove that entry
 * DELETE (no body)         reset to broadcast-only */
static void handle_trice_dest(struct netconn *conn, sConnStream *s, int add)
{
    uint32_t  content_length = parse_content_length(req_buf);
    char      body[96];
    uint32_t  got = 0u;
    uint8_t   ip[4];
    ip_addr_t addr;

    if (!add && content_length == 0u) {
        Trice_UdpResetDests();
        (void)Trice_UdpForgetDests();
        TRice("Trice UDP destinations reset to broadcast\n");
        size_t off = Json_Cat(resp_buf, RESP_BODY_CAP, 0u, "{\"dests\":");
        off = trice_dests_array(resp_buf, RESP_BODY_CAP, off);
        (void)Json_Cat(resp_buf, sizeof(resp_buf), off,
                       ",\"port\":%u}", (unsigned)TRICE_UDP_PORT);
        send_json(conn, "200 OK", resp_buf);
        return;
    }

    if (content_length == 0u || content_length >= sizeof(body)) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"JSON body required (max 95 bytes)\"}");
        return;
    }
    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    while (got < content_length) {
        int ch = cs_read_byte(s);
        if (ch < 0) {
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"incomplete body\"}");
            return;
        }
        body[got++] = (char)ch;
    }
    body[got] = '\0';

    if (!json_ipv4(body, "ip", ip)) {
        send_json(conn, "422 Unprocessable Entity",
                  "{\"error\":\"expected {\\\"ip\\\":\\\"a.b.c.d\\\"}\"}");
        return;
    }

    IP4_ADDR(&addr, ip[0], ip[1], ip[2], ip[3]);

    if (add) {
        if (Trice_UdpAddDest(&addr) < 0) {
            send_json(conn, "507 Insufficient Storage",
                      "{\"error\":\"destination list is full\"}");
            return;
        }
        TRice("Trice UDP dest added %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
    } else {
        if (Trice_UdpRemoveDest(&addr) != 0) {
            send_json(conn, "404 Not Found",
                      "{\"error\":\"address is not in the list\"}");
            return;
        }
        TRice("Trice UDP dest removed %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
    }

    /* An explicitly configured destination is meant to outlive a reset — that
     * it did not is the defect this closes.  A subscriber that added ITSELF
     * (the /subscribe path below) is not persisted: it can ask again, and a
     * board should not accumulate the addresses of laptops that have long
     * since gone home. */
    (void)Trice_UdpSaveDests();

    {
        size_t off = Json_Cat(resp_buf, RESP_BODY_CAP, 0u, "{\"dests\":");
        off = trice_dests_array(resp_buf, RESP_BODY_CAP, off);
        (void)Json_Cat(resp_buf, sizeof(resp_buf), off,
                       ",\"port\":%u}", (unsigned)TRICE_UDP_PORT);
    }
    send_json(conn, "200 OK", resp_buf);
}

/* Register (or drop) the caller as a Trice listener.
 *
 * The address is taken from the connection, never from a body: the caller does
 * not have to know which of its own addresses the board should use, and the
 * recorded address is return-routable by construction because a packet just
 * arrived from it.  LAN and tunnel behave identically here, since the HTTP
 * listener is bound to IP_ADDR_ANY and the connection's peer address is
 * whatever actually reached us.
 *
 * Only valid where no NAT sits between listener and board — behind one, the
 * peer address is the translated one and UDP will not find its way back; use
 * /api/trice/dest there. */
static void handle_trice_subscribe(struct netconn *conn, int add)
{
    ip_addr_t peer;
    u16_t     port = 0u;
    size_t    off;

    if (netconn_peer(conn, &peer, &port) != ERR_OK) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"could not read peer address\"}");
        return;
    }

    if (add) {
        if (Trice_UdpAddDest(&peer) < 0) {
            send_json(conn, "507 Insufficient Storage",
                      "{\"error\":\"destination list is full\"}");
            return;
        }
        TRice("Trice UDP subscriber %d.%d.%d.%d added\n",
              (unsigned)ip4_addr1(&peer), (unsigned)ip4_addr2(&peer),
              (unsigned)ip4_addr3(&peer), (unsigned)ip4_addr4(&peer));
    } else {
        if (Trice_UdpRemoveDest(&peer) != 0) {
            send_json(conn, "404 Not Found",
                      "{\"error\":\"caller is not subscribed\"}");
            return;
        }
        TRice("Trice UDP subscriber %d.%d.%d.%d removed\n",
              (unsigned)ip4_addr1(&peer), (unsigned)ip4_addr2(&peer),
              (unsigned)ip4_addr3(&peer), (unsigned)ip4_addr4(&peer));
    }

    off = Json_Cat(resp_buf, RESP_BODY_CAP, 0u,
                   "{\"you\":\"%u.%u.%u.%u\",\"dests\":",
                   (unsigned)ip4_addr1(&peer), (unsigned)ip4_addr2(&peer),
                   (unsigned)ip4_addr3(&peer), (unsigned)ip4_addr4(&peer));
    off = trice_dests_array(resp_buf, RESP_BODY_CAP, off);
    (void)Json_Cat(resp_buf, sizeof(resp_buf), off,
                   ",\"port\":%u}", (unsigned)TRICE_UDP_PORT);
    send_json(conn, "200 OK", resp_buf);
}

/* Mint a fresh identity on-device.  The private key never leaves the board;
 * the caller gets the public half to register with the hub. */
static void handle_wg_keygen(struct netconn *conn)
{
    if (WgLink_GenerateKey(1) < 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"could not generate or store a key\"}");
        return;
    }
    TRice("WG: new identity key generated on-device\n");
    wg_status_json(resp_buf, sizeof(resp_buf));
    send_json(conn, "200 OK", resp_buf);
}

static void handle_wg_restart(struct netconn *conn)
{
    if (WgLink_Restart() != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"tunnel restart failed\"}");
        return;
    }
    wg_status_json(resp_buf, sizeof(resp_buf));
    send_json(conn, "200 OK", resp_buf);
}

/* `GET /`.  The stored page is served STILL COMPRESSED -- the board never
 * decompresses it, it sets one header and streams the bytes it holds -- so
 * gzip costs nothing here and saves about two thirds of the segments on a
 * tunnel where TCP_MSS is 536.
 *
 * `Accept-Encoding` is deliberately not consulted: honouring it would mean
 * storing an uncompressed copy too, which is the cost the compression exists
 * to avoid, and every browser that can reach this board sends gzip.  Note
 * this makes `curl http://HOST/` return binary -- use `curl --compressed`.
 * Only this one route is affected; every /api/ reply stays plain JSON. */
static void handle_index(struct netconn *conn)
{
    static uint8_t buf[256];
    char           hdr[160];
    uint32_t       size, off;
    int            hlen;

    if (!WebUi_Present()) {
        hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n\r\n",
            (unsigned)(sizeof(fallback_html) - 1));
        send_all(conn, hdr, hlen);
        send_all(conn, fallback_html, sizeof(fallback_html) - 1);
        return;
    }

    size = WebUi_PayloadSize();
    hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Encoding: gzip\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n\r\n",
        (unsigned long)size);
    if (!send_all(conn, hdr, hlen)) {
        return;
    }

    for (off = 0u; off < size; ) {
        uint32_t n = size - off;

        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        if (!WebUi_Read(off, buf, n)) {
            return;         /* header already sent; dropping is all we can do */
        }
        if (!send_all(conn, buf, n)) {
            return;
        }
        off += n;
    }
}

/** `GET /api/ui` — is a page stored, and which one. */
static void handle_ui_status(struct netconn *conn)
{
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"present\":%s,\"size\":%lu,\"crc32\":\"%08lX\","
             "\"encoding\":\"gzip\",\"max_bytes\":%lu}",
             WebUi_Present() ? "true" : "false",
             (unsigned long)WebUi_PayloadSize(),
             (unsigned long)WebUi_Crc32(),
             (unsigned long)WEBUI_BLOB_MAX);
    send_json(conn, "200 OK", resp_buf);
}

/** `POST /api/ui` — upload a .pnui blob, streamed straight to the medium. */
static void handle_ui_upload(struct netconn *conn, sConnStream *s)
{
    uint32_t    content_length = parse_content_length(req_buf);
    uint32_t    remaining;
    const char *err;

    if (content_length == 0u) {
        send_json(conn, "411 Length Required",
                  "{\"error\":\"Content-Length required\"}");
        return;
    }

    err = WebUi_UploadBegin(content_length);
    if (err != NULL) {
        snprintf(resp_buf, sizeof(resp_buf), "{\"error\":\"%s\"}", err);
        send_json(conn, "409 Conflict", resp_buf);
        return;
    }

    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    remaining = content_length;
    while (remaining > 0u) {
        uint32_t n;

        if (cs_fill(s) != ERR_OK) {
            WebUi_UploadAbort();
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"connection lost during upload\"}");
            return;
        }
        n = (uint32_t)(s->len - s->off);
        if (n > remaining) {
            n = remaining;
        }
        if (!WebUi_UploadWrite((uint8_t *)s->data + s->off, n)) {
            WebUi_UploadAbort();
            send_json(conn, "500 Internal Server Error",
                      "{\"error\":\"flash write error\"}");
            return;
        }
        s->off    += (u16_t)n;
        remaining -= n;
    }

    err = WebUi_UploadFinish();
    if (err != NULL) {
        /* 422: the bytes arrived, they are just not a page.  The area is left
         * without a valid header, so the fallback answers until a good blob
         * lands -- which is the state an operator can act on. */
        snprintf(resp_buf, sizeof(resp_buf), "{\"error\":\"%s\"}", err);
        send_json(conn, "422 Unprocessable Entity", resp_buf);
        return;
    }

    snprintf(resp_buf, sizeof(resp_buf),
             "{\"status\":\"stored\",\"size\":%lu,\"crc32\":\"%08lX\"}",
             (unsigned long)WebUi_PayloadSize(),
             (unsigned long)WebUi_Crc32());
    send_json(conn, "200 OK", resp_buf);
}

static void handle_connection(struct netconn *conn)
{
    sConnStream stream;
    cs_init(&stream, conn);

    if (read_request_header(&stream) < 0) {
        cs_cleanup(&stream);
        return;
    }

    if (route_is("POST /api/image/upload")) {
        handle_image_upload(conn, &stream);
    } else if (route_is("GET /api/image/info")) {
        handle_image_info(conn);
    } else if (route_is("GET /api/image/download")) {
        handle_image_download(conn);
    } else if (route_is("DELETE /api/image ")) {
        handle_image_delete(conn);
    } else if (route_is("POST /api/fwu/install")) {
        handle_fwu_install(conn);
    } else if (route_is("POST /api/fwu/kick")) {
        handle_fwu_kick(conn);
    } else if (route_is("POST /api/fwu/confirm")) {
        handle_fwu_confirm(conn);
    } else if (route_is("GET /api/fwu/verify")) {
        handle_fwu_verify(conn);
    } else if (route_is("GET /api/fwu/status")) {
        handle_fwu_status(conn);
    } else if (route_is("GET /api/crash/latest")) {
        handle_crash_get(conn);
    } else if (route_is("DELETE /api/crash/latest")) {
        handle_crash_delete(conn);
    } else if (route_is("GET /api/system/status")) {
        handle_system_status(conn);
    } else if (route_is("POST /api/system/reset-peaks")) {
        handle_system_reset_peaks(conn);
    } else if (route_is("POST /api/system/reboot")) {
        handle_system_reboot(conn);
    } else if (route_is("POST /api/modbus/dump/on")) {
        handle_modbus_dump(conn, 1);
    } else if (route_is("POST /api/modbus/dump/off")) {
        handle_modbus_dump(conn, 0);
    } else if (route_is("POST /api/modbus/monitor/on")) {
        handle_modbus_monitor(conn, 1);
    } else if (route_is("POST /api/modbus/monitor/off")) {
        handle_modbus_monitor(conn, 0);
    } else if (route_is("GET /api/modbus/gw")) {
        handle_modbus_gw(conn);
    } else if (route_is("GET /api/modbus/bus")) {
        handle_modbus_bus(conn);
    } else if (route_is("POST /api/modbus/bus/reset")) {
        handle_modbus_bus_reset(conn);
    } else if (route_is("POST /api/modbus/write")) {
        handle_modbus_write(conn, &stream);
    } else if (route_is("GET /api/nvdb/layout")) {
        handle_nvdb_layout_get(conn);
    } else if (route_is("POST /api/nvdb/layout")) {
        handle_nvdb_layout_post(conn, &stream);
    } else if (route_is("DELETE /api/nvdb/layout")) {
        handle_nvdb_layout_delete(conn);
    } else if (route_is("GET /api/nvdb/usage")) {
        handle_nvdb_usage(conn);
    } else if (route_is("POST /api/modbus/config/upload")) {
        handle_modbus_cfg_upload(conn, &stream);
    } else if (route_is("POST /api/modbus/config/verify")) {
        handle_modbus_cfg_verify(conn, &stream);
    } else if (route_is("GET /api/modbus/plans")) {
        handle_modbus_plans_list(conn);
    } else if (route_is("POST /api/modbus/plans")) {
        handle_modbus_plan_create(conn, &stream);
    } else if (route_is("PUT /api/modbus/plans/")) {
        handle_modbus_plan_modify(conn, &stream,
                                  (uint8_t)atoi(req_buf + 22));
    } else if (route_is("DELETE /api/modbus/plans/")) {
        handle_modbus_plan_delete(conn, (uint8_t)atoi(req_buf + 25));
    } else if (route_is("POST /api/modbus/config/apply")) {
        handle_modbus_cfg_apply(conn);
    } else if (route_is("GET /api/modbus/config/status")) {
        handle_modbus_cfg_status(conn);
    } else if (route_is("GET /api/modbus/config/download")) {
        handle_modbus_cfg_download(conn);
    } else if (route_is("DELETE /api/modbus/config ")) {
        handle_modbus_cfg_erase(conn);
    } else if (route_is("GET /api/pack/status")) {
        handle_pack_status(conn);
    } else if (route_is("GET /api/pack/stats")) {
        handle_pack_stats(conn, (uint8_t)query_int("idx", 0));
    } else if (route_is("GET /api/pack/balance")) {
        handle_pack_balance(conn, (uint8_t)query_int("idx", 0));
    } else if (route_is("POST /api/pack/balance/reset")) {
        send_json(conn, "200 OK",
                  (Pack_BalanceReset((uint8_t)query_int("idx", 0)) == packErr_ok)
                  ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (route_is("GET /api/pack/cells")) {
        handle_pack_cells(conn, (uint8_t)query_int("idx", 0));
    } else if (route_is("POST /api/pack/config/verify")) {
        handle_pack_cfg_post(conn, &stream, 0);
    } else if (route_is("POST /api/pack/config")) {
        handle_pack_cfg_post(conn, &stream, 1);
    } else if (route_is("GET /api/pack/config")) {
        handle_pack_cfg_get(conn);
    } else if (route_is("DELETE /api/pack/config")) {
        handle_pack_cfg_delete(conn);
    } else if (route_is("GET /api/cluster/status")) {
        handle_cluster_status(conn, (query_int("packs", 1) != 0));
    } else if (route_is("POST /api/cluster/config/verify")) {
        handle_cluster_cfg_post(conn, &stream, 0);
    } else if (route_is("POST /api/cluster/config")) {
        handle_cluster_cfg_post(conn, &stream, 1);
    } else if (route_is("GET /api/cluster/config")) {
        handle_cluster_cfg_get(conn);
    } else if (route_is("DELETE /api/cluster/config")) {
        handle_cluster_cfg_delete(conn);
    } else if (route_is("GET /api/can/status")) {
        handle_can_status(conn);
    } else if (route_is("GET /api/can/traffic")) {
        handle_can_traffic(conn, (query_int("bus", 1) == 2) ? canBus_2
                                                            : canBus_1);
    } else if (route_is("GET /api/can/trace")) {
        handle_can_trace(conn);
    } else if (route_is("POST /api/can/trace/on")) {
        handle_can_trace_enable(conn, 1);
    } else if (route_is("POST /api/can/trace/off")) {
        handle_can_trace_enable(conn, 0);
    } else if (route_is("POST /api/can/mode")) {
        handle_can_mode(conn);
    } else if (route_is("POST /api/can/send")) {
        handle_can_send(conn);
    } else if (route_is("POST /api/can/reset")) {
        handle_can_reset(conn);
    } else if (route_is("GET /api/can/log/status")) {
        handle_can_log_status(conn);
    } else if (route_is("POST /api/can/log/mode")) {
        handle_can_log_mode(conn);
    } else if (route_is("GET /api/can/log/read")) {
        handle_can_log_read(conn);
    } else if (route_is("POST /api/can/log/wipe")) {
        handle_can_log_wipe(conn);
    } else if (route_is("GET /api/wg/status")) {
        handle_wg_status(conn);
    } else if (route_is("POST /api/wg/config")) {
        handle_wg_config(conn, &stream);
    } else if (route_is("DELETE /api/wg/config")) {
        handle_wg_config_reset(conn);
    } else if (route_is("GET /api/trice/status")) {
        handle_trice_status(conn);
    } else if (route_is("POST /api/trice/dest")) {
        handle_trice_dest(conn, &stream, 1);
    } else if (route_is("DELETE /api/trice/dest")) {
        handle_trice_dest(conn, &stream, 0);
    } else if (route_is("POST /api/trice/subscribe")) {
        handle_trice_subscribe(conn, 1);
    } else if (route_is("DELETE /api/trice/subscribe")) {
        handle_trice_subscribe(conn, 0);
    } else if (route_is("POST /api/wg/keygen")) {
        handle_wg_keygen(conn);
    } else if (route_is("POST /api/wg/restart")) {
        handle_wg_restart(conn);
    } else if (route_is("GET /api/ui")) {
        handle_ui_status(conn);
    } else if (route_is("POST /api/ui")) {
        handle_ui_upload(conn, &stream);
    } else if (route_is("DELETE /api/ui")) {
        if (WebUi_Erase()) {
            send_json(conn, "200 OK", "{\"status\":\"erased\"}");
        } else {
            send_json(conn, "500 Internal Server Error",
                      "{\"error\":\"could not erase the stored page\"}");
        }
    } else if (route_is("GET / ")) {
        handle_index(conn);
    } else {
        send_body(conn, "404 Not Found", "text/html",
                  "<html><body><h1>404 Not Found</h1></body></html>");
    }

    cs_cleanup(&stream);
}

/* --------------------------------------------------------------------------
 * Server task
 * -------------------------------------------------------------------------- */

static const osThreadAttr_t s_httpAttr = {
    .name       = "http",
    .stack_size = 1024U * 4U,
    .priority   = osPriorityNormal,
};

static void http_task(void *arg)
{
    (void)arg;

    /* Idle means blocked in netconn_accept() forever, so no deadline — the
     * check-in counts served connections, which is the useful number here. */
    int8_t monId = SysMon_TaskRegister(1024U, 0U);

    struct netconn *listener = netconn_new(NETCONN_TCP);
    if (listener == NULL ||
        netconn_bind(listener, IP_ADDR_ANY, HTTP_SERVER_PORT) != ERR_OK ||
        netconn_listen(listener) != ERR_OK) {
        TRice("HTTP: listener setup FAILED\n");
        if (listener) netconn_delete(listener);
        osThreadExit();
    }

    TRice("HTTP: listening on port %d\n", HTTP_SERVER_PORT);

    for (;;) {
        struct netconn *conn;
        if (netconn_accept(listener, &conn) != ERR_OK) {
            continue;
        }

        netconn_set_recvtimeout(conn, HTTP_IO_TIMEOUT_MS);
        netconn_set_sendtimeout(conn, HTTP_IO_TIMEOUT_MS);

        handle_connection(conn);

        netconn_close(conn);
        netconn_delete(conn);

        SysMon_TaskCheckin(monId);
    }
}

void http_server_init(void)
{
    sFwuConfirmGuard g;

    ImgStore_Init();
    FwuCtl_Init();

    /* Say so ONCE at boot.  An operator who finds a board that rebooted on
     * its own needs this line in the trace to tell the confirmation deadline
     * apart from a crash or an IWDG reset. */
    FwuCtl_GetConfirmGuard(&g);
    if (g.armed) {
        /* Trice needs the whole format in ONE literal -- the ID inserter
         * matches specifiers against arguments and cannot see a split. */
        TRice("FWU: UNCONFIRMED, auto-reboot in %u s unless kicked, %u attempts left\n",
              (unsigned)g.window_sec, (unsigned)g.attemptsLeft);
    }

    osThreadNew(http_task, NULL, &s_httpAttr);
}

/*
 * HTTP server — dedicated FreeRTOS task using the lwIP netconn API.
 *
 * Connections are handled sequentially by one task, so plain synchronous
 * code replaces the old raw-callback state machines: headers are consumed
 * byte-wise from a small stream cursor (immune to TCP segmentation), the
 * upload body is streamed straight into the staging logic, and Trice
 * logging is allowed here (task context, not tcpip_thread).
 *
 * Firmware/FWU domain logic lives in image_transfer.c; this file owns all
 * HTTP parsing and response formatting.
 */

#include "App/Http/http_server.h"
#include "App/Http/image_transfer.h"
#include "App/Log/crash.h"
#include "App/system.h"
#include "bl_app_contract.h"
#include "version.h"
#include "boot_status.h"
#include "lwip/api.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "trice.h"
#include <string.h>
#include <stdio.h>

#define HTTP_IO_TIMEOUT_MS   10000
#define REQ_BUF_SIZE         1024
#define DOWNLOAD_CHUNK_SIZE  512

/* Single-task server: static buffers are safe and cheap */
static char req_buf[REQ_BUF_SIZE];
static char resp_buf[1024];

static const char index_html[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>PeriphNet</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:640px;margin:20px auto;padding:0 12px}"
    "h1{margin-bottom:4px}#ver{color:#666;font-size:.9em}"
    ".card{border:1px solid #ddd;border-radius:6px;padding:12px;margin:10px 0}"
    ".card h3{margin:0 0 8px}#bar{width:100%;height:18px;background:#eee;border-radius:4px;display:none}"
    "#fill{height:100%;background:#4a4;border-radius:4px;transition:width .3s}"
    "button{padding:6px 14px;border:none;border-radius:4px;cursor:pointer;margin:4px 2px}"
    ".btn-up{background:#37c;color:#fff}.btn-dl{background:#666;color:#fff}"
    ".btn-inst{background:#e63;color:#fff}.btn-del{background:#a33;color:#fff}"
    "button:disabled{opacity:.5;cursor:default}"
    "#msg{margin:8px 0;padding:8px;border-radius:4px;display:none}"
    ".ok{background:#dfd;color:#060}.err{background:#fdd;color:#600}"
    "#info{color:#555;font-size:.9em}"
    "pre{background:#f5f5f5;padding:8px;border-radius:4px;font-size:.8em;overflow-x:auto}"
    "</style></head><body>"
    "<h1>PeriphNet</h1><div id=ver></div>"
    "<div class=card><h3>Firmware Update</h3>"
    "<input type=file id=file accept='.pnfw'>"
    "<button class=btn-up onclick=upload()>Upload</button>"
    "<div id=bar><div id=fill></div></div>"
    "<div id=info></div><div id=msg></div>"
    "<div style='margin-top:8px'>"
    "<button class=btn-dl onclick=download() id=bdl disabled>Download Staged</button>"
    "<button class=btn-inst onclick=install() id=binst disabled>Install</button>"
    "<button class=btn-up onclick=confirmFw() id=bconf disabled>Confirm</button>"
    "<button class=btn-del onclick=del() id=bdel disabled>Delete Staged</button>"
    "</div></div>"
    "<div class=card><h3>Last Crash</h3>"
    "<div id=crash>Loading...</div></div>"
    "<script>"
    "var B='http://'+location.host;"
    "function show(t,ok){var m=document.getElementById('msg');m.textContent=t;"
    "m.className=ok?'ok':'err';m.style.display='block'}"
    "function poll(){fetch(B+'/api/firmware/status').then(r=>r.json()).then(j=>{"
    "document.getElementById('ver').textContent='Running: '+j.running_version"
    "+(j.confirmed?' (confirmed)':' UNCONFIRMED, '+j.attempts_remaining+' boots left');"
    "var has=!!j.staged_version;"
    "var t=has?'Staged: '+j.staged_version:'';"
    "if(j.golden_version)t+=(t?' | ':'')+'Golden: '+j.golden_version;"
    "if(j.last_fwu_result!=255)t+=(t?' | ':'')+'Last FWU result: '+j.last_fwu_result;"
    "if(j.promote_pending)t+=' | promoting...';"
    "document.getElementById('info').textContent=t;"
    "document.getElementById('bdl').disabled=!has;"
    "document.getElementById('binst').disabled=!has;"
    "document.getElementById('bdel').disabled=!has;"
    "document.getElementById('bconf').disabled=j.confirmed;"
    "}).catch(()=>{})}"
    "function confirmFw(){fetch(B+'/api/firmware/confirm',{method:'POST'})"
    ".then(r=>r.json()).then(j=>{show('Confirmed'+(j.promote?', promoting to golden':''),1);poll()})"
    ".catch(e=>show(e,0))}"
    "function crashPoll(){fetch(B+'/api/crash/latest').then(r=>r.json()).then(j=>{"
    "var d=document.getElementById('crash');"
    "if(!j.valid){d.innerHTML='No crash recorded.';return}"
    "var h='<b>'+j.type+'</b> at tick '+j.tick+'<br>'"
    "+'PC=0x'+j.pc+' LR=0x'+j.lr+' SP=0x'+j.sp+'<br>';"
    "if(j.task)h+='Task: '+j.task+'<br>';"
    "h+='CFSR=0x'+j.cfsr+' HFSR=0x'+j.hfsr+'<br>';"
    "if(j.backtrace.length)h+='BT: '+j.backtrace.join(' ')+'<br>';"
    "if(j.tasks.length){h+='<pre>';j.tasks.forEach(function(t){"
    "h+=t.name+' ['+t.state+'] PC=0x'+t.pc+' stk='+t.free_stack+'\\n'});"
    "h+='</pre>'}"
    "h+='<button class=btn-del onclick=clearCrash()>Clear</button>';"
    "d.innerHTML=h}).catch(()=>{})}"
    "function clearCrash(){fetch(B+'/api/crash/latest',{method:'DELETE'})"
    ".then(()=>crashPoll()).catch(()=>{})}"
    "function upload(){var f=document.getElementById('file').files[0];"
    "if(!f){show('Select a file first',0);return}"
    "var bar=document.getElementById('bar'),fill=document.getElementById('fill');"
    "bar.style.display='block';fill.style.width='0%';"
    "var x=new XMLHttpRequest();"
    "x.upload.onprogress=function(e){if(e.lengthComputable)"
    "fill.style.width=Math.round(100*e.loaded/e.total)+'%'};"
    "x.onload=function(){bar.style.display='none';"
    "if(x.status==200){var r=JSON.parse(x.responseText);"
    "show('Upload OK: '+r.version+' ('+r.bytes+' B)',1)}else{"
    "show('Upload failed: '+x.responseText,0)}poll()};"
    "x.onerror=function(){bar.style.display='none';show('Network error',0)};"
    "x.open('POST',B+'/api/firmware/upload');"
    "x.setRequestHeader('Content-Type','application/octet-stream');"
    "x.send(f)}"
    "function download(){window.location=B+'/api/firmware/download'}"
    "function install(){if(!confirm('Install staged firmware? Device will reboot.'))return;"
    "fetch(B+'/api/firmware/install',{method:'POST'}).then(r=>r.json()).then(j=>{"
    "if(j.status=='deploying'){show('Installing... device will reboot',1)}else{"
    "show('Install failed: '+(j.error||JSON.stringify(j)),0)}}).catch(e=>show(e,0))}"
    "function del(){fetch(B+'/api/firmware/staged',{method:'DELETE'}).then(r=>r.json())"
    ".then(j=>{show(j.status=='deleted'?'Staged image deleted':'Delete failed: '+(j.error||''),j.status=='deleted');poll()})"
    ".catch(e=>show(e,0))}"
    "poll();setInterval(poll,5000);crashPoll();"
    "</script></body></html>";

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

static void handle_upload(struct netconn *conn, sConnStream *s)
{
    uint32_t content_length = parse_content_length(req_buf);

    const char *err;
    if (!img_upload_begin(content_length, &err)) {
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
            img_upload_abort("Connection lost");
            send_json(conn, "408 Request Timeout",
                      "{\"error\":\"connection lost during upload\"}");
            return;
        }

        uint32_t n = (uint32_t)(s->len - s->off);
        if (n > remaining) n = remaining;

        if (!img_upload_write((uint8_t *)s->data + s->off, n)) {
            send_json(conn, "500 Internal Server Error",
                      "{\"error\":\"flash write error\"}");
            return;
        }
        s->off += (u16_t)n;
        remaining -= n;
    }

    const image_state_t *st = image_transfer_get_status();
    if (img_upload_finish()) {
        TRice("FWU: staged %u B\n", (unsigned)st->staged.blob_size);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\":\"staged\",\"bytes\":%lu,\"version\":\"%s\"}",
                 (unsigned long)st->staged.blob_size, st->staged.version_str);
        send_json(conn, "200 OK", resp_buf);
    } else {
        snprintf(resp_buf, sizeof(resp_buf), "{\"error\":\"%s\"}",
                 st->error_message);
        send_json(conn, "422 Unprocessable Entity", resp_buf);
    }
}

static void handle_download(struct netconn *conn)
{
    const image_state_t *st = image_transfer_get_status();

    if (st->status != IMG_STATUS_STAGED || !st->staged.valid) {
        send_body(conn, "404 Not Found", "text/plain",
                  "No staged firmware available for download\r\n");
        return;
    }

    img_download_begin();

    uint32_t total = st->staged.blob_size;
    char hdr[192];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lu\r\n"
        "Content-Disposition: attachment; filename=\"firmware.pnfw\"\r\n"
        "Connection: close\r\n\r\n",
        (unsigned long)total);
    send_all(conn, hdr, hlen);

    uint8_t buf[DOWNLOAD_CHUNK_SIZE];
    for (uint32_t off = 0; off < total; off += DOWNLOAD_CHUNK_SIZE) {
        uint32_t n = total - off;
        if (n > DOWNLOAD_CHUNK_SIZE) n = DOWNLOAD_CHUNK_SIZE;

        if (!img_read_staged(off, buf, n)) {
            img_download_end(false, "Flash read error");
            return;
        }
        if (!send_all(conn, buf, n)) {
            img_download_end(false, "TCP write error");
            return;
        }
    }

    img_download_end(true, NULL);
}

static void handle_status(struct netconn *conn)
{
    const image_state_t *st = image_transfer_get_status();

    const char *status_str;
    switch (st->status) {
        case IMG_STATUS_IDLE:        status_str = "idle";        break;
        case IMG_STATUS_UPLOADING:   status_str = "uploading";   break;
        case IMG_STATUS_STAGED:      status_str = "staged";      break;
        case IMG_STATUS_DOWNLOADING: status_str = "downloading"; break;
        case IMG_STATUS_ERROR:       status_str = "error";       break;
        default:                     status_str = "unknown";     break;
    }

    uint32_t progress_pct = 0;
    if (st->total_bytes > 0)
        progress_pct = (st->bytes_transferred * 100) / st->total_bytes;

    char running_ver[24] = {0};
    const sAppInfo *app = (const sAppInfo *)APP_INFO_HEADER_ADDR;
    if (app->magic == APP_INFO_MAGIC) {
        ver_toString(&app->fw_version.ver, running_ver, sizeof(running_ver));
    }

    sBootStatus bs;
    bool     bs_ok       = (BootStatus_Read(&bs) == 0);
    bool     unconfirmed = BootStatus_IsUnconfirmed();
    uint8_t  attempts    = BootStatus_AttemptsRemaining();
    uint32_t last_result = bs_ok ? bs.last_fwu_result : (uint32_t)FWU_NO_RESULT;

    char staged_field[32], golden_field[32];
    if (st->staged.valid) {
        snprintf(staged_field, sizeof(staged_field), "\"%s\"",
                 st->staged.version_str);
    } else {
        strcpy(staged_field, "null");
    }
    if (st->golden.valid) {
        snprintf(golden_field, sizeof(golden_field), "\"%s\"",
                 st->golden.version_str);
    } else {
        strcpy(golden_field, "null");
    }

    snprintf(resp_buf, sizeof(resp_buf),
        "{\"status\":\"%s\","
        "\"running_version\":\"%s\","
        "\"staged_version\":%s,"
        "\"golden_version\":%s,"
        "\"confirmed\":%s,"
        "\"attempts_remaining\":%u,"
        "\"last_fwu_result\":%lu,"
        "\"promote_pending\":%s,"
        "\"bytes_transferred\":%lu,"
        "\"total_bytes\":%lu,"
        "\"progress\":%lu,"
        "\"reset_cause\":\"0x%08lX\","
        "\"error\":\"%s\"}",
        status_str,
        running_ver,
        staged_field,
        golden_field,
        unconfirmed ? "false" : "true",
        (unsigned)attempts,
        (unsigned long)last_result,
        image_transfer_promote_pending() ? "true" : "false",
        (unsigned long)st->bytes_transferred,
        (unsigned long)st->total_bytes,
        (unsigned long)progress_pct,
        (unsigned long)System_GetResetCause(),
        st->error_message);

    send_json(conn, "200 OK", resp_buf);
}

static void handle_install(struct netconn *conn)
{
    switch (img_install_request()) {
    case IMG_CTL_OK: {
        const image_state_t *st = image_transfer_get_status();
        TRice("FWU: install requested, rebooting\n");
        snprintf(resp_buf, sizeof(resp_buf),
            "{\"status\":\"deploying\","
            "\"message\":\"Device will reboot in 2 seconds\","
            "\"version\":\"%s\"}", st->staged.version_str);
        send_json(conn, "200 OK", resp_buf);
        break;
    }
    case IMG_CTL_NO_IMAGE:
        send_json(conn, "409 Conflict",
                  "{\"error\":\"no staged firmware available\"}");
        break;
    case IMG_CTL_BUSY:
        send_json(conn, "409 Conflict",
                  "{\"error\":\"golden promotion in progress\"}");
        break;
    default:
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"failed to write boot status\"}");
        break;
    }
}

static void handle_confirm(struct netconn *conn)
{
    bool promote;
    switch (img_confirm(&promote)) {
    case IMG_CTL_OK:
        TRice("FWU: confirmed (promote=%d)\n", (int)promote);
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\":\"confirmed\",\"promote\":%s}",
                 promote ? "true" : "false");
        send_json(conn, "200 OK", resp_buf);
        break;
    case IMG_CTL_ALREADY:
        send_json(conn, "200 OK", "{\"status\":\"already_confirmed\"}");
        break;
    default:
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"failed to write boot status\"}");
        break;
    }
}

static void handle_delete(struct netconn *conn)
{
    switch (img_delete()) {
    case IMG_CTL_OK:
        send_json(conn, "200 OK", "{\"status\":\"deleted\"}");
        break;
    case IMG_CTL_BUSY:
        send_json(conn, "409 Conflict",
                  "{\"error\":\"transfer or promotion in progress\"}");
        break;
    default:
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"flash erase failed\"}");
        break;
    }
}

static void handle_verify(struct netconn *conn)
{
    eFwuRes res = img_verify_running();

    const char *reason = NULL;
    switch (res) {
        case FWU_OK:             break;
        case FWU_ERR_WRONG_MAGIC: reason = "no app header";      break;
        case FWU_ERR_IMAGE_SIZE:  reason = "unsigned image";     break;
        case FWU_ERR_NO_IMAGE:    reason = "BL API unavailable"; break;
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

static const char * const crash_type_names[] = {
    "HardFault", "NMI", "BusFault", "UsageFault", "MemManage", "SwWatchdog"
};
static const char * const task_state_names[] = {
    "Run", "Rdy", "Blk", "Sus", "Del"
};

static void handle_crash_get(struct netconn *conn)
{
    sCrashLog *log = (sCrashLog *)pvPortMalloc(sizeof(sCrashLog));
    if (log == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }

    if (!Crash_ReadFromFlash(log)) {
        vPortFree(log);
        send_json(conn, "200 OK", "{\"valid\":false}");
        return;
    }

    const char *type_str = (log->crash_type < 6)
        ? crash_type_names[log->crash_type] : "Unknown";
    int pos = snprintf(resp_buf, sizeof(resp_buf),
        "{\"valid\":true,\"type\":\"%s\",\"tick\":%lu,"
        "\"pc\":\"%08lX\",\"lr\":\"%08lX\",\"sp\":\"%08lX\","
        "\"r0\":\"%08lX\",\"r12\":\"%08lX\",\"psr\":\"%08lX\","
        "\"cfsr\":\"%08lX\",\"hfsr\":\"%08lX\","
        "\"mmfar\":\"%08lX\",\"bfar\":\"%08lX\","
        "\"task\":\"%s\",\"backtrace\":[",
        type_str, (unsigned long)log->tick,
        (unsigned long)log->pc, (unsigned long)log->lr,
        (unsigned long)log->sp, (unsigned long)log->r0,
        (unsigned long)log->r12, (unsigned long)log->psr,
        (unsigned long)log->cfsr, (unsigned long)log->hfsr,
        (unsigned long)log->mmfar, (unsigned long)log->bfar,
        log->task_name);

    for (int i = 0; i < log->bt_depth && i < CRASH_LOG_MAX_BT_DEPTH; i++) {
        pos += snprintf(resp_buf + pos, sizeof(resp_buf) - pos, "%s\"%08lX\"",
            i > 0 ? "," : "", (unsigned long)log->bt_addr[i]);
    }

    pos += snprintf(resp_buf + pos, sizeof(resp_buf) - pos, "],\"tasks\":[");
    for (int i = 0; i < log->task_count && i < CRASH_LOG_MAX_TASKS; i++) {
        const char *st = (log->tasks[i].state < 5)
            ? task_state_names[log->tasks[i].state] : "???";
        pos += snprintf(resp_buf + pos, sizeof(resp_buf) - pos,
            "%s{\"name\":\"%s\",\"state\":\"%s\","
            "\"pc\":\"%08lX\",\"lr\":\"%08lX\",\"free_stack\":%u}",
            i > 0 ? "," : "",
            log->tasks[i].name, st,
            (unsigned long)log->tasks[i].pc,
            (unsigned long)log->tasks[i].lr,
            log->tasks[i].free_stack);
    }
    snprintf(resp_buf + pos, sizeof(resp_buf) - pos, "]}");

    vPortFree(log);
    send_json(conn, "200 OK", resp_buf);
}

static void handle_crash_delete(struct netconn *conn)
{
    Crash_ClearFlash();
    send_json(conn, "200 OK", "{\"status\":\"cleared\"}");
}

/* --------------------------------------------------------------------------
 * Request dispatch
 * -------------------------------------------------------------------------- */

static bool route_is(const char *method_path)
{
    return strncmp(req_buf, method_path, strlen(method_path)) == 0;
}

static void handle_connection(struct netconn *conn)
{
    sConnStream stream;
    cs_init(&stream, conn);

    if (read_request_header(&stream) < 0) {
        cs_cleanup(&stream);
        return;
    }

    if (route_is("POST /api/firmware/upload")) {
        handle_upload(conn, &stream);
    } else if (route_is("GET /api/firmware/download")) {
        handle_download(conn);
    } else if (route_is("POST /api/firmware/install")) {
        handle_install(conn);
    } else if (route_is("POST /api/firmware/confirm")) {
        handle_confirm(conn);
    } else if (route_is("GET /api/firmware/verify")) {
        handle_verify(conn);
    } else if (route_is("DELETE /api/firmware/staged")) {
        handle_delete(conn);
    } else if (route_is("GET /api/firmware/status")) {
        handle_status(conn);
    } else if (route_is("GET /api/crash/latest")) {
        handle_crash_get(conn);
    } else if (route_is("DELETE /api/crash/latest")) {
        handle_crash_delete(conn);
    } else if (route_is("GET / ")) {
        char hdr[96];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n\r\n",
            (unsigned)(sizeof(index_html) - 1));
        send_all(conn, hdr, hlen);
        send_all(conn, index_html, sizeof(index_html) - 1);
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
    }
}

void http_server_init(void)
{
    image_transfer_init();
    osThreadNew(http_task, NULL, &s_httpAttr);
}

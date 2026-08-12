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
#include "App/Img/image_store.h"
#include "App/Fwu/fwu_control.h"
#include "App/Log/crash.h"
#include "App/Log/trice_udp.h"
#include "App/Log/trice_consumer.h"
#include "usart.h"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include "App/Modbus/modbus_default_config.h"
#include "App/Net/wg_link.h"
#include "App/Net/wg_platform.h"
#include "App/Net/wg_time.h"
#include "App/system.h"
#include "bl_app_contract.h"
#include "version.h"
#include "boot_status.h"
#include "modbus_config_store.h"
#include "modbus_config_compiler.h"
#include "modbus_config_export.h"
#include "lwip/api.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "trice.h"
#include <string.h>
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
    ".msg{margin:8px 0;padding:8px;border-radius:4px;display:none}"
    ".ok{background:#dfd;color:#060}.err{background:#fdd;color:#600}"
    ".info{color:#555;font-size:.9em;margin:6px 0}"
    "pre{background:#f5f5f5;padding:8px;border-radius:4px;font-size:.8em;overflow-x:auto}"
    "</style></head><body>"
    "<h1>PeriphNet</h1><div id=ver></div><div id=uptime class=info></div>"
    "<div class=card><h3>Image Management</h3>"
    "<input type=file id=file accept='.pnfw'>"
    "<button class=btn-up onclick=upload()>Upload</button>"
    "<div id=bar><div id=fill></div></div>"
    "<div id=iinfo class=info></div><div id=imsg class=msg></div>"
    "<div style='margin-top:8px'>"
    "<button class=btn-dl onclick=download() id=bdl disabled>Download</button>"
    "<button class=btn-del onclick=del() id=bdel disabled>Delete</button>"
    "</div></div>"
    "<div class=card><h3>Firmware Update</h3>"
    "<div id=finst class=info></div><div id=finfo class=info></div>"
    "<div id=fmsg class=msg></div>"
    "<div style='margin-top:8px'>"
    "<button class=btn-inst onclick=install() id=binst disabled>Install</button>"
    "<button class=btn-up onclick=confirmFw() id=bconf disabled>Confirm</button>"
    "</div></div>"
    "<div class=card><h3>Last Crash</h3>"
    "<div id=crash>Loading...</div></div>"
    "<script>"
    "var B='http://'+location.host;"
    "function show(id,t,ok){var m=document.getElementById(id);m.textContent=t;"
    "m.className='msg '+(ok?'ok':'err');m.style.display='block'}"
    "function pollImg(){fetch(B+'/api/image/info').then(r=>r.json()).then(j=>{"
    "var t;"
    "if(j.present){t='Name: '+(j.name||'(unnamed)')+' | Version: '+j.version"
    "+' | Size: '+j.size+' B | CRC32: '+j.crc32}"
    "else if(j.status=='uploading'){t='Uploading... '+j.progress+'%'}"
    "else if(j.status=='error'){t='Error: '+j.error}"
    "else{t='No image uploaded.'}"
    "document.getElementById('iinfo').textContent=t;"
    "document.getElementById('bdl').disabled=!j.present;"
    "document.getElementById('bdel').disabled=!j.present;"
    "document.getElementById('binst').disabled=!j.present;"
    "document.getElementById('finst').textContent=j.present?"
    "'Image ready to install: '+j.version+(j.name?' ('+j.name+')':''):"
    "'No image available - upload one in Image Management.';"
    "}).catch(()=>{})}"
    "function pollFwu(){fetch(B+'/api/fwu/status').then(r=>r.json()).then(j=>{"
    "document.getElementById('ver').textContent='Running: '+j.running_version"
    "+(j.confirmed?' (confirmed)':' UNCONFIRMED, '+j.attempts_remaining+' boots left');"
    "var u=j.uptime,s='';"
    "if(u>=86400){s+=Math.floor(u/86400)+'d ';u%=86400}"
    "if(u>=3600){s+=Math.floor(u/3600)+'h ';u%=3600}"
    "if(u>=60){s+=Math.floor(u/60)+'m ';u%=60}"
    "s+=u+'s';"
    "document.getElementById('uptime').textContent='Uptime: '+s;"
    "var t='';"
    "if(j.golden_version)t='Golden: '+j.golden_version;"
    "if(j.last_fwu_result!=255)t+=(t?' | ':'')+'Last FWU result: '+j.last_fwu_result;"
    "if(j.promote_pending)t+=(t?' | ':'')+'promoting to golden...';"
    "document.getElementById('finfo').textContent=t;"
    "document.getElementById('bconf').disabled=j.confirmed;"
    "}).catch(()=>{})}"
    "function poll(){pollImg();pollFwu()}"
    "function confirmFw(){fetch(B+'/api/fwu/confirm',{method:'POST'})"
    ".then(r=>r.json()).then(j=>{show('fmsg','Confirmed'+(j.promote?', promoting to golden':''),1);poll()})"
    ".catch(e=>show('fmsg',e,0))}"
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
    "if(!f){show('imsg','Select a file first',0);return}"
    "var bar=document.getElementById('bar'),fill=document.getElementById('fill');"
    "bar.style.display='block';fill.style.width='0%';"
    "var x=new XMLHttpRequest();"
    "x.upload.onprogress=function(e){if(e.lengthComputable)"
    "fill.style.width=Math.round(100*e.loaded/e.total)+'%'};"
    "x.onload=function(){bar.style.display='none';"
    "if(x.status==200){var r=JSON.parse(x.responseText);"
    "show('imsg','Upload OK: '+r.version+' ('+r.size+' B)',1)}else{"
    "show('imsg','Upload failed: '+x.responseText,0)}poll()};"
    "x.onerror=function(){bar.style.display='none';show('imsg','Network error',0)};"
    "x.open('POST',B+'/api/image/upload');"
    "x.setRequestHeader('Content-Type','application/octet-stream');"
    "x.setRequestHeader('X-Filename',f.name.replace(/[^\\x20-\\x7e]/g,'_'));"
    "x.send(f)}"
    "function download(){window.location=B+'/api/image/download'}"
    "function install(){if(!confirm('Install uploaded image? Device will reboot.'))return;"
    "fetch(B+'/api/fwu/install',{method:'POST'}).then(r=>r.json()).then(j=>{"
    "if(j.status=='deploying'){show('fmsg','Installing... device will reboot',1)}else{"
    "show('fmsg','Install failed: '+(j.error||JSON.stringify(j)),0)}}).catch(e=>show('fmsg',e,0))}"
    "function del(){fetch(B+'/api/image',{method:'DELETE'}).then(r=>r.json())"
    ".then(j=>{show('imsg',j.status=='deleted'?'Image deleted':'Delete failed: '+(j.error||''),j.status=='deleted');poll()})"
    ".catch(e=>show('imsg',e,0))}"
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

    snprintf(resp_buf, sizeof(resp_buf),
        "{\"running_version\":\"%s\","
        "\"confirmed\":%s,"
        "\"attempts_remaining\":%u,"
        "\"last_fwu_result\":%lu,"
        "\"uptime\":%lu,"
        "\"golden_version\":%s,"
        "\"promote_pending\":%s,"
        "\"reset_cause\":\"0x%08lX\"}",
        running_ver,
        unconfirmed ? "false" : "true",
        (unsigned)attempts,
        (unsigned long)last_result,
        (unsigned long)uptime,
        golden_field,
        FwuCtl_PromotePending() ? "true" : "false",
        (unsigned long)System_GetResetCause());

    send_json(conn, "200 OK", resp_buf);
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
 * Modbus config endpoints (/api/modbus/config/*) — upload compiles JSON
 * straight into the inactive LUT region (compile = validation, design §4/§9);
 * apply arms the swap flag and the walker commits at a lap boundary.
 * -------------------------------------------------------------------------- */

/* Last upload compile outcome, for /api/modbus/config/status */
static sMbCompileResult s_lastCompile;
static bool             s_haveCompile;

/* Byte source feeding MbCfgCompile from the connection body */
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

/* Memory byte source (factory-reset compiles the built-in JSON) */
typedef struct {
    const char *data;
    uint32_t    len;
    uint32_t    pos;
} sMemSource;

static int mem_source(void *ctx, uint8_t *buf, uint32_t maxLen)
{
    sMemSource *m = (sMemSource *)ctx;
    uint32_t    n = m->len - m->pos;

    if (n > maxLen) n = maxLen;
    memcpy(buf, &m->data[m->pos], n);
    m->pos += n;
    return (int)n;
}

static void send_compile_result(struct netconn *conn,
                                const sMbCompileResult *res)
{
    if (res->ok) {
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"status\":\"compiled\",\"devices\":%u,"
                 "\"transactions\":%u,\"points\":%u}",
                 res->counts.devices, res->counts.transactions,
                 res->counts.points);
        send_json(conn, "200 OK", resp_buf);
    } else {
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"%s\",\"field\":\"%s\","
                 "\"device\":%d,\"transaction\":%d,\"point\":%d}",
                 res->reason, res->field,
                 res->deviceIdx, res->txnIdx, res->pointIdx);
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
    if (MbCfgStore_IsSwapPending()) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"apply pending; config swap not yet committed\"}");
        return;
    }

    if (header_expects_continue(req_buf)) {
        send_all(conn, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    sBodySource src = { s, content_length };
    MbCfgCompile(body_source, &src, MbCfgStore_InactiveBase(),
                 KickIwdg, &s_lastCompile);
    s_haveCompile = true;

    if (s_lastCompile.ok) {
        TRice("Modbus config: staged %u devices %u txns %u points\n",
              s_lastCompile.counts.devices, s_lastCompile.counts.transactions,
              s_lastCompile.counts.points);
    } else {
        TRiceS("Modbus config: rejected: %s\n", s_lastCompile.reason);
    }
    send_compile_result(conn, &s_lastCompile);
}

static void handle_modbus_cfg_apply(struct netconn *conn)
{
    if (MbCfgStore_SetSwapPending() != 0) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"no valid staged config to apply\"}");
        return;
    }
    TRice("Modbus config: apply armed (walker swaps at lap boundary)\n");
    send_json(conn, "200 OK", "{\"status\":\"pending\"}");
}

static void handle_modbus_cfg_status(struct netconn *conn)
{
    sMbCfgCounts counts = {0};
    uint32_t     active = MbCfgStore_ActiveBase();
    bool         valid  = MbCfgStore_RegionValid(active);

    if (valid) {
        (void)MbCfg_Count(active, &counts);
    }

    int n = snprintf(resp_buf, sizeof(resp_buf),
        "{\"active_region\":%u,\"valid\":%s,"
        "\"devices\":%u,\"transactions\":%u,\"points\":%u,"
        "\"staged_valid\":%s,\"swap_pending\":%s",
        (unsigned)(active == EXT_FLASH_MODBUS_LUT_B_ADDR),
        valid ? "true" : "false",
        counts.devices, counts.transactions, counts.points,
        MbCfgStore_RegionValid(MbCfgStore_InactiveBase()) ? "true" : "false",
        MbCfgStore_IsSwapPending() ? "true" : "false");

    if (s_haveCompile) {
        n += snprintf(resp_buf + n, sizeof(resp_buf) - (size_t)n,
            ",\"last_upload\":{\"ok\":%s,\"error\":\"%s\",\"field\":\"%s\","
            "\"device\":%d,\"transaction\":%d,\"point\":%d}",
            s_lastCompile.ok ? "true" : "false",
            s_lastCompile.reason, s_lastCompile.field,
            s_lastCompile.deviceIdx, s_lastCompile.txnIdx,
            s_lastCompile.pointIdx);
    }
    snprintf(resp_buf + n, sizeof(resp_buf) - (size_t)n, "}");
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
    if (MbCfgExport(MbCfgStore_ActiveBase(), export_count_sink, &total) != 0) {
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
    (void)MbCfgExport(MbCfgStore_ActiveBase(), export_conn_sink, conn);
}

static void handle_modbus_cfg_reset(struct netconn *conn)
{
    if (MbCfgStore_IsSwapPending()) {
        send_json(conn, "409 Conflict",
                  "{\"error\":\"apply pending; config swap not yet committed\"}");
        return;
    }

    sMemSource src = {
        g_modbusDefaultConfigJson,
        (uint32_t)strlen(g_modbusDefaultConfigJson),
        0,
    };
    sMbCompileResult res;
    if (MbCfgCompile(mem_source, &src, MbCfgStore_InactiveBase(),
                     KickIwdg, &res) != 0 ||
        MbCfgStore_SetSwapPending() != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"default config compile failed\"}");
        return;
    }

    TRice("Modbus config: factory default staged + apply armed\n");
    send_json(conn, "200 OK", "{\"status\":\"pending\"}");
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
    int  n = 0;
    uint8_t i;

    /* The public key is what the operator pastes into the hub.  The private
     * key is never reported by any endpoint. */
    (void)WgLink_GetPublicKeyB64(pubKey, sizeof(pubKey));
    (void)WgLink_GetPeerKeyB64(peerKey, sizeof(peerKey));

    for (i = 0u; i < cfg->allowedCount && n >= 0 &&
                 n < (int)sizeof(allowed); i++) {
        n += snprintf(allowed + n, sizeof(allowed) - (size_t)n,
                      "%s\"%u.%u.%u.%u/%u.%u.%u.%u\"", (i > 0u) ? "," : "",
                      cfg->allowed[i].ip[0], cfg->allowed[i].ip[1],
                      cfg->allowed[i].ip[2], cfg->allowed[i].ip[3],
                      cfg->allowed[i].mask[0], cfg->allowed[i].mask[1],
                      cfg->allowed[i].mask[2], cfg->allowed[i].mask[3]);
    }

    sWgPeerStats st;
    char peerStats[160] = "";

    if (WgLink_GetPeerStats(&st) == 0) {
        (void)snprintf(peerStats, sizeof(peerStats),
            ",\"peer_last_rx_ms\":%u,\"peer_last_tx_ms\":%u,"
            "\"tx_packets\":%u,\"rx_counter\":%u,"
            "\"live_endpoint\":\"%u.%u.%u.%u:%u\",\"now_ms\":%u",
            (unsigned)st.lastRx_ms, (unsigned)st.lastTx_ms,
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
static void handle_trice_status(struct netconn *conn)
{
    uint32_t udpSent = 0u, udpFailed = 0u;
    unsigned usbTxState = 255u;

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
    snprintf(resp_buf, sizeof(resp_buf),
        "{\"uart3_gstate\":%u,\"uart3_ready\":%s,"
        "\"aux_fn_registered\":%s,\"consumer_pending\":%u,"
        "\"udp_consumer_registered\":%s,\"udp_pcb_ok\":%s,"
        "\"udp_sent\":%u,\"udp_failed\":%u,\"usb_tx_state\":%u,"
        "\"free_heap\":%u,"
        "\"trice_errors\":%u,\"deferred_overflow\":%u,"
        "\"half_buffer_depth_max\":%u}",
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
    send_json(conn, "200 OK", resp_buf);
}

/* Retarget the Trice UDP stream at runtime.
 *
 * The default destination is a tunnel address, which makes the log stream
 * useless for diagnosing the tunnel itself — the output only arrives over the
 * link being debugged.  This endpoint breaks that circle: point Trice at a
 * host on whatever network currently works. */
static void handle_trice_dest(struct netconn *conn, sConnStream *s)
{
    uint32_t content_length = parse_content_length(req_buf);
    char     body[96];
    uint32_t got = 0u;
    uint8_t  ip[4];

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

    Trice_UdpSetDest(ip[0], ip[1], ip[2], ip[3]);
    TRice("Trice UDP retargeted to %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

    snprintf(resp_buf, sizeof(resp_buf),
             "{\"dest\":\"%u.%u.%u.%u\",\"port\":%u}",
             ip[0], ip[1], ip[2], ip[3], (unsigned)TRICE_UDP_PORT);
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
    WgLink_Stop();
    if (WgLink_Start(NULL) != 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"tunnel restart failed\"}");
        return;
    }
    wg_status_json(resp_buf, sizeof(resp_buf));
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
    } else if (route_is("POST /api/modbus/config/upload")) {
        handle_modbus_cfg_upload(conn, &stream);
    } else if (route_is("POST /api/modbus/config/apply")) {
        handle_modbus_cfg_apply(conn);
    } else if (route_is("GET /api/modbus/config/status")) {
        handle_modbus_cfg_status(conn);
    } else if (route_is("GET /api/modbus/config/download")) {
        handle_modbus_cfg_download(conn);
    } else if (route_is("DELETE /api/modbus/config ")) {
        handle_modbus_cfg_reset(conn);
    } else if (route_is("GET /api/wg/status")) {
        handle_wg_status(conn);
    } else if (route_is("POST /api/wg/config")) {
        handle_wg_config(conn, &stream);
    } else if (route_is("DELETE /api/wg/config")) {
        handle_wg_config_reset(conn);
    } else if (route_is("GET /api/trice/status")) {
        handle_trice_status(conn);
    } else if (route_is("POST /api/trice/dest")) {
        handle_trice_dest(conn, &stream);
    } else if (route_is("POST /api/wg/keygen")) {
        handle_wg_keygen(conn);
    } else if (route_is("POST /api/wg/restart")) {
        handle_wg_restart(conn);
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
    ImgStore_Init();
    FwuCtl_Init();

    /* Provision the built-in Solis Modbus config on a blank device so the
     * walker and the config endpoints always have a valid active region */
    (void)ModbusConfig_EnsureDefault();

    osThreadNew(http_task, NULL, &s_httpAttr);
}

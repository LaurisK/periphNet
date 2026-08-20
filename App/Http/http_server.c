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
#include "nvdb.h"
#include "nvdb_config.h"
#include "nvdb_layout.h"
#include "App/Fwu/fwu_control.h"
#include "App/Log/crash.h"
#include "App/Log/trice_udp.h"
#include "App/Log/trice_consumer.h"
#include "App/Mon/sysmon.h"
#include "usart.h"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include "App/Modbus/modbus.h"
#include "App/Modbus/modbus_trice_sink.h"
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
    "<div class=card><h3>System</h3>"
    "<div id=sysinfo class=info>Loading...</div><div id=systasks></div>"
    "<button class=btn-dl onclick=resetPeaks()>Reset peaks</button></div>"
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
    "function pct(p){return (p/10).toFixed(1)+'%'}"
    "function pollSys(){fetch(B+'/api/system/status').then(r=>r.json()).then(j=>{"
    "var h='CPU '+pct(j.cpu_load_permille)+' (peak '+pct(j.cpu_peak_permille)"
    "+') | idle '+pct(j.idle_permille)"
    "+'<br>Heap '+j.heap.free+' / '+j.heap.size+' B free (min '+j.heap.free_min+')'"
    "+'<br>Watchdog margin: worst gap '+j.iwdg.gap_max_ms+' ms of '+j.iwdg.timeout_ms+' ms';"
    "if(!j.runtime_counter_ok)h+='<br><b>CPU clock not running</b>';"
    "if(j.tasks_stale)h+='<br><b>'+j.tasks_stale+' task(s) missed their check-in deadline</b>';"
    "if(j.stack_warnings)h+='<br><b>'+j.stack_warnings+' task(s) below '+j.stack_warn_words+' free stack words</b>';"
    "document.getElementById('sysinfo').innerHTML=h;"
    "var t='task            pri st    cpu   peak  stack(free/size) checkins\\n';"
    "j.tasks.forEach(function(k){"
    "t+=(k.name+'               ').slice(0,15)+String(k.prio).padStart(4)+' '+k.state"
    "+pct(k.cpu_permille).padStart(7)+pct(k.cpu_peak_permille).padStart(7)+'  '"
    "+(k.stack_free_min_words+'/'+(k.stack_size_words||'?')).padStart(14)+' '"
    "+k.checkins+(k.deadline_ms?' ('+k.since_checkin_ms+'ms)':'')"
    "+(k.stale?' STALE':'')+(k.present?'':' GONE')+'\\n'});"
    "document.getElementById('systasks').innerHTML='<pre>'+t+'</pre>'"
    "}).catch(()=>{})}"
    "function resetPeaks(){fetch(B+'/api/system/reset-peaks',{method:'POST'})"
    ".then(()=>pollSys()).catch(()=>{})}"
    "function poll(){pollImg();pollFwu();pollSys()}"
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
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"%s\",\"field\":\"%s\",\"offset\":%u}",
                 err.reason, err.field, (unsigned)err.offset_bytes);
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

    TRiceS("nvDb: layout '%s' taken aboard, applies at next boot\n", cfg.name);
    snprintf(resp_buf, sizeof(resp_buf),
             "{\"status\":\"onboard\",\"name\":\"%s\",\"version\":%u,"
             "\"operation\":\"%s\","
             "\"note\":\"applied at the next boot; check "
             "lastApplyResult afterwards\"}",
             cfg.name, (unsigned)cfg.version,
             NvDbCfg_ModeName(cfg.operation));
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

/* The crash report outgrew resp_buf once CRASH_LOG_MAX_TASKS went to 16 (~1.6 kB
 * of JSON), so it is built in a transient heap block instead of enlarging a CCM
 * buffer -- the same trade /api/system/status makes, and CCM is the scarce
 * region here.
 *
 * The old builder also accumulated snprintf()'s return, which is the length it
 * WOULD have written.  Past the end that makes `pos` exceed the buffer and
 * `cap - pos` underflow to a huge size_t -- i.e. the first truncated field
 * turned into an overflowing write.  Every append below clamps instead. */
#define CRASH_JSON_CAP  2560u

static size_t json_cat(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    va_list ap;
    int     n;

    if (pos >= cap) {
        return cap;          /* full: swallow, never wrap */
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return pos;
    }
    pos += (size_t)n;
    return (pos > cap) ? cap : pos;   /* truncated is fine; overrunning is not */
}

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

    const char *type_str = (log->crash_type < 6)
        ? crash_type_names[log->crash_type] : "Unknown";

    pos = json_cat(js, CRASH_JSON_CAP, pos,
        "{\"valid\":true,\"type\":\"%s\",\"tick\":%lu,"
        "\"pc\":\"%08lX\",\"lr\":\"%08lX\",\"sp\":\"%08lX\","
        "\"r0\":\"%08lX\",\"r12\":\"%08lX\",\"psr\":\"%08lX\","
        "\"cfsr\":\"%08lX\",\"hfsr\":\"%08lX\","
        "\"mmfar\":\"%08lX\",\"bfar\":\"%08lX\","
        "\"task\":\"%s\",\"task_count\":%u,\"backtrace\":[",
        type_str, (unsigned long)log->tick,
        (unsigned long)log->pc, (unsigned long)log->lr,
        (unsigned long)log->sp, (unsigned long)log->r0,
        (unsigned long)log->r12, (unsigned long)log->psr,
        (unsigned long)log->cfsr, (unsigned long)log->hfsr,
        (unsigned long)log->mmfar, (unsigned long)log->bfar,
        log->task_name, (unsigned)log->task_count);

    for (int i = 0; i < log->bt_depth && i < CRASH_LOG_MAX_BT_DEPTH; i++) {
        pos = json_cat(js, CRASH_JSON_CAP, pos, "%s\"%08lX\"",
                       (i > 0) ? "," : "", (unsigned long)log->bt_addr[i]);
    }

    pos = json_cat(js, CRASH_JSON_CAP, pos, "],\"tasks\":[");
    for (int i = 0; i < log->task_count && i < CRASH_LOG_MAX_TASKS; i++) {
        const char *st = (log->tasks[i].state < 5)
            ? task_state_names[log->tasks[i].state] : "???";
        pos = json_cat(js, CRASH_JSON_CAP, pos,
            "%s{\"name\":\"%s\",\"state\":\"%s\","
            "\"pc\":\"%08lX\",\"lr\":\"%08lX\",\"free_stack\":%u}",
            (i > 0) ? "," : "",
            log->tasks[i].name, st,
            (unsigned long)log->tasks[i].pc,
            (unsigned long)log->tasks[i].lr,
            log->tasks[i].free_stack);
    }
    (void)json_cat(js, CRASH_JSON_CAP, pos, "]}");

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

#define SYS_STATUS_BUF_SIZE 3072u

static void handle_system_status(struct netconn *conn)
{
    sSysMonTaskInfo tasks[SYSMON_MAX_TASKS];
    sSysMonSummary  sum;
    char           *buf;
    size_t          off;
    uint8_t         n;

    buf = (char *)pvPortMalloc(SYS_STATUS_BUF_SIZE);
    if (buf == NULL) {
        send_json(conn, "503 Service Unavailable", "{\"error\":\"oom\"}");
        return;
    }

    SysMon_GetSummary(&sum);
    n = SysMon_GetTasks(tasks, SYSMON_MAX_TASKS);

    off = (size_t)snprintf(buf, SYS_STATUS_BUF_SIZE,
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
        "\"stack_warn_words\":%u,"
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
        (unsigned)sum.stackWarnCnt, (unsigned)SYSMON_STACK_WARN_WORDS);

    if (off >= SYS_STATUS_BUF_SIZE) {
        off = SYS_STATUS_BUF_SIZE - 1u;
    }

    for (uint8_t i = 0u; i < n; i++) {
        const sSysMonTaskInfo *t = &tasks[i];
        int w = snprintf(buf + off, SYS_STATUS_BUF_SIZE - off,
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

        /* Truncating mid-object would emit invalid JSON — stop on the last
         * entry that fits instead. */
        if (w < 0 || (size_t)w >= (SYS_STATUS_BUF_SIZE - off)) {
            break;
        }
        off += (size_t)w;
    }

    snprintf(buf + off, SYS_STATUS_BUF_SIZE - off, "]}");
    send_json(conn, "200 OK", buf);
    vPortFree(buf);
}

static void handle_system_reset_peaks(struct netconn *conn)
{
    SysMon_ResetPeaks();
    TRice("SysMon: peaks cleared over HTTP\n");
    send_json(conn, "200 OK", "{\"status\":\"cleared\"}");
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
            snprintf(resp_buf, sizeof(resp_buf),
                     "{\"error\":\"point is read-only\",\"device\":%u,"
                     "\"id\":%d,\"name\":\"%s\"}",
                     (unsigned)device, (int)id, meta.name);
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

    off = (size_t)snprintf(resp_buf, sizeof(resp_buf),
                           "{\"device\":%u,\"count\":%u,\"items\":[",
                           (unsigned)device, (unsigned)count);
    for (uint16_t i = 0; i < count; i++) {
        sModbusPointMeta meta;
        const char *nm = (Modbus_PointInfo((uint8_t)device, s_wrItems[i].id,
                                           &meta) == 0) ? meta.name : "";

        if (off + 96u >= sizeof(resp_buf)) break;
        off += (size_t)snprintf(resp_buf + off, sizeof(resp_buf) - off,
                                "%s{\"id\":%u,\"name\":\"%s\",\"value\":%ld,"
                                "\"result\":%d}",
                                (i == 0u) ? "" : ",",
                                (unsigned)s_wrItems[i].id, nm,
                                (long)s_wrItems[i].value,
                                (int)s_wrItems[i].result);
    }
    snprintf(resp_buf + off, sizeof(resp_buf) - off, "]}");
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
        snprintf(resp_buf, sizeof(resp_buf),
                 "{\"error\":\"%s\",\"field\":\"%s\","
                 "\"capability\":%d,\"device\":%d,\"plan\":%d,"
                 "\"index\":%d}",
                 res->reason, res->field,
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

    int n = snprintf(resp_buf, sizeof(resp_buf),
        "{\"active_region\":%u,\"valid\":%s,\"state\":\"%s\","
        "\"capabilities\":%u,\"devices\":%u,\"plans\":%u,\"points\":%u,"
        "\"staged_valid\":%s,\"swap_pending\":%s",
        st.activeRegion,
        valid ? "true" : "false",
        valid ? "provisioned" : "unprovisioned",
        counts.capabilities, counts.devices, counts.plans, counts.points,
        st.stagedValid ? "true" : "false",
        st.swapPending ? "true" : "false");

    /* Per device: its port, whether that port has a driver, which plans cover
     * it and whether anything is actually polling it — "why is this device not
     * polled" is a question about SUBSCRIBERS (§4.3). */
    {
        sModbusDeviceInfo devs[MB_MAX_DEVICES];
        int               dn = Modbus_DeviceList(devs, MB_MAX_DEVICES);

        n += snprintf(resp_buf + n, sizeof(resp_buf) - (size_t)n,
                      ",\"devices_state\":[");
        for (int i = 0; i < dn; i++) {
            n += snprintf(resp_buf + n, sizeof(resp_buf) - (size_t)n,
                "%s{\"id\":%u,\"prefix\":\"%s\",\"slave\":%u,"
                "\"capability\":%u,\"port\":\"%s\",\"port_up\":%s,"
                "\"baud\":%lu,\"plans\":%u,\"polled\":%s}",
                i ? "," : "", devs[i].devOrd, devs[i].topicPrefix,
                devs[i].slaveAddr, devs[i].capId,
                (devs[i].portId == mbPort_test) ? "test" : "rs485",
                devs[i].portUp ? "true" : "false",
                (unsigned long)devs[i].baud, devs[i].coveringPlans,
                devs[i].polled ? "true" : "false");
        }
        n += snprintf(resp_buf + n, sizeof(resp_buf) - (size_t)n, "]");
    }

    if (s_haveCompile) {
        n += snprintf(resp_buf + n, sizeof(resp_buf) - (size_t)n,
            ",\"last_upload\":{\"ok\":%s,\"error\":\"%s\",\"field\":\"%s\","
            "\"capability\":%d,\"device\":%d,\"plan\":%d,\"index\":%d}",
            s_lastCompile.ok ? "true" : "false",
            s_lastCompile.reason, s_lastCompile.field,
            s_lastCompile.capIdx, s_lastCompile.devIdx,
            s_lastCompile.planIdx, s_lastCompile.subIdx);
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
    int             at;

    if (n < 0) {
        send_json(conn, "500 Internal Server Error",
                  "{\"error\":\"plan list failed\"}");
        return;
    }

    at = snprintf(resp_buf, sizeof(resp_buf), "{\"plans\":[");
    for (int i = 0; i < n; i++) {
        at += snprintf(resp_buf + at, sizeof(resp_buf) - (size_t)at,
            "%s{\"id\":%u,\"name\":\"%s\",\"capability\":%u,"
            "\"devices\":%u,\"timeTables\":%u,\"subscribers\":%u}",
            i ? "," : "", plans[i].planId, plans[i].name, plans[i].capId,
            plans[i].devices, plans[i].timeTables, plans[i].subscribers);
    }
    snprintf(resp_buf + at, sizeof(resp_buf) - (size_t)at, "]}");
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
static size_t trice_dests_array(char *buf, size_t size);

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

    /* snprintf reports what it *would* have written, so clamp before using it
     * as a cursor — otherwise a future field could walk this past the end. */
    if (off >= sizeof(resp_buf)) {
        off = sizeof(resp_buf) - 1u;
    }
    off += trice_dests_array(resp_buf + off, sizeof(resp_buf) - off);
    snprintf(resp_buf + off, sizeof(resp_buf) - off, "}");
    send_json(conn, "200 OK", resp_buf);
}

/* Render the Trice destination list as a JSON array: ["a.b.c.d",...] */
static size_t trice_dests_array(char *buf, size_t size)
{
    ip_addr_t list[TRICE_UDP_MAX_DEST];
    uint32_t  n   = Trice_UdpGetDests(list, TRICE_UDP_MAX_DEST);
    size_t    off = 0u;

    if (size == 0u) {
        return 0u;
    }
    buf[off++] = '[';

    for (uint32_t i = 0u; i < n && off < (size - 1u); i++) {
        int w = snprintf(buf + off, size - off, "%s\"%u.%u.%u.%u\"",
                         (i == 0u) ? "" : ",",
                         (unsigned)ip4_addr1(&list[i]),
                         (unsigned)ip4_addr2(&list[i]),
                         (unsigned)ip4_addr3(&list[i]),
                         (unsigned)ip4_addr4(&list[i]));
        if (w < 0 || (size_t)w >= (size - off)) {
            off = size - 1u;
            break;
        }
        off += (size_t)w;
    }

    if (off < (size - 1u)) {
        buf[off++] = ']';
    }
    buf[off] = '\0';
    return off;
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
        size_t off = (size_t)snprintf(resp_buf, sizeof(resp_buf), "{\"dests\":");
        off += trice_dests_array(resp_buf + off, sizeof(resp_buf) - off);
        snprintf(resp_buf + off, sizeof(resp_buf) - off,
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
        size_t off = (size_t)snprintf(resp_buf, sizeof(resp_buf), "{\"dests\":");
        off += trice_dests_array(resp_buf + off, sizeof(resp_buf) - off);
        snprintf(resp_buf + off, sizeof(resp_buf) - off,
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

    off = (size_t)snprintf(resp_buf, sizeof(resp_buf),
                           "{\"you\":\"%u.%u.%u.%u\",\"dests\":",
                           (unsigned)ip4_addr1(&peer), (unsigned)ip4_addr2(&peer),
                           (unsigned)ip4_addr3(&peer), (unsigned)ip4_addr4(&peer));
    if (off >= sizeof(resp_buf)) {
        off = sizeof(resp_buf) - 1u;
    }
    off += trice_dests_array(resp_buf + off, sizeof(resp_buf) - off);
    snprintf(resp_buf + off, sizeof(resp_buf) - off,
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
    } else if (route_is("GET /api/system/status")) {
        handle_system_status(conn);
    } else if (route_is("POST /api/system/reset-peaks")) {
        handle_system_reset_peaks(conn);
    } else if (route_is("POST /api/modbus/dump/on")) {
        handle_modbus_dump(conn, 1);
    } else if (route_is("POST /api/modbus/dump/off")) {
        handle_modbus_dump(conn, 0);
    } else if (route_is("POST /api/modbus/monitor/on")) {
        handle_modbus_monitor(conn, 1);
    } else if (route_is("POST /api/modbus/monitor/off")) {
        handle_modbus_monitor(conn, 0);
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
    ImgStore_Init();
    FwuCtl_Init();

    osThreadNew(http_task, NULL, &s_httpAttr);
}

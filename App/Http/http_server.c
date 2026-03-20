#include "App/Http/http_server.h"
#include "App/Http/image_transfer.h"
#include "bl_app_contract.h"
#include "version.h"
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

/* Web UI HTML — served from flash via GET / */
static const char index_html[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n"
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
    "</style></head><body>"
    "<h1>PeriphNet</h1><div id=ver></div>"
    "<div class=card><h3>Firmware Update</h3>"
    "<input type=file id=file accept='.bin'>"
    "<button class=btn-up onclick=upload()>Upload</button>"
    "<div id=bar><div id=fill></div></div>"
    "<div id=info></div><div id=msg></div>"
    "<div style='margin-top:8px'>"
    "<button class=btn-dl onclick=download() id=bdl disabled>Download Staged</button>"
    "<button class=btn-inst onclick=install() id=binst disabled>Install</button>"
    "<button class=btn-del onclick=del() id=bdel disabled>Delete Staged</button>"
    "</div></div>"
    "<script>"
    "var B='http://'+location.host;"
    "function show(t,ok){var m=document.getElementById('msg');m.textContent=t;"
    "m.className=ok?'ok':'err';m.style.display='block'}"
    "function poll(){fetch(B+'/api/firmware/status').then(r=>r.json()).then(j=>{"
    "document.getElementById('ver').textContent='Running: '+j.running_version;"
    "var s=j.staged_version,has=s&&s!='null';"
    "document.getElementById('info').textContent=has?'Staged: '+s+' ('+j.bytes_transferred+' B)':'';"
    "document.getElementById('bdl').disabled=!has;"
    "document.getElementById('binst').disabled=!has;"
    "document.getElementById('bdel').disabled=!has;"
    "}).catch(()=>{})}"
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
    "x.setRequestHeader('Content-Length',f.size);"
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
    "poll();setInterval(poll,5000);"
    "</script></body></html>";

/* Streaming index page state — attached as tcp arg */
typedef struct {
    const char *ptr;
    uint16_t remaining;
} index_send_t;

static err_t index_sent_callback(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)len;
    index_send_t *ctx = (index_send_t *)arg;
    if (ctx == NULL || ctx->remaining == 0) {
        if (ctx) vPortFree(ctx);
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_close(pcb);
        return ERR_OK;
    }

    uint16_t sndbuf = tcp_sndbuf(pcb);
    uint16_t send_len = (ctx->remaining > sndbuf) ? sndbuf : ctx->remaining;
    if (send_len == 0) return ERR_OK;

    err_t werr = tcp_write(pcb, ctx->ptr, send_len, TCP_WRITE_FLAG_COPY);
    if (werr == ERR_OK) {
        ctx->ptr += send_len;
        ctx->remaining -= send_len;
        tcp_output(pcb);
    }
    return ERR_OK;
}

static void index_err_callback(void *arg, err_t err)
{
    (void)err;
    if (arg) vPortFree(arg);
}

static err_t http_serve_index(struct tcp_pcb *pcb)
{
    index_send_t *ctx = (index_send_t *)pvPortMalloc(sizeof(index_send_t));
    if (ctx == NULL) {
        tcp_close(pcb);
        return ERR_MEM;
    }

    ctx->ptr = index_html;
    ctx->remaining = (uint16_t)(sizeof(index_html) - 1);

    /* Detach from the normal http callbacks for this pcb */
    tcp_arg(pcb, ctx);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, index_sent_callback);
    tcp_err(pcb, index_err_callback);

    /* Send first chunk */
    uint16_t sndbuf = tcp_sndbuf(pcb);
    uint16_t send_len = (ctx->remaining > sndbuf) ? sndbuf : ctx->remaining;

    err_t werr = tcp_write(pcb, ctx->ptr, send_len, TCP_WRITE_FLAG_COPY);
    if (werr == ERR_OK) {
        ctx->ptr += send_len;
        ctx->remaining -= send_len;
        tcp_output(pcb);
    } else {
        vPortFree(ctx);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return ERR_OK;
}

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
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return image_install_handler(pcb);
    }

    /* DELETE /api/firmware/staged */
    if (p->len >= 27 && strncmp(request, "DELETE /api/firmware/staged", 27) == 0) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return image_delete_handler(pcb);
    }

    /* GET /api/firmware/status */
    if (p->len >= 24 && strncmp(request, "GET /api/firmware/status", 24) == 0) {
        err_t result = image_status_handler(pcb);
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        tcp_close(pcb);
        return result;
    }

    /* GET / — web UI */
    if (p->len >= 6 && strncmp(request, "GET / ", 6) == 0) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return http_serve_index(pcb);
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

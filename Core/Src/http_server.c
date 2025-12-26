/**
 ******************************************************************************
 * @file    http_server.c
 * @brief   Minimal HTTP server implementation for PeriphNet Milestone 1
 ******************************************************************************
 * @attention
 *
 * Simple HTTP server using lwIP raw TCP API
 * Serves "Hello World v1.0.0" page with system information
 *
 ******************************************************************************
 */

#include "http_server.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "string.h"
#include "stdio.h"
#include "main.h"

/* External network interface (defined in lwip.c) */
extern struct netif gnetif;

/* HTTP response headers */
static const char http_200_header[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char http_404_header[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<html><body><h1>404 Not Found</h1></body></html>";

/**
 * @brief  Generate HTML page with system information
 * @param  buf: Buffer to write HTML to
 * @param  buflen: Size of buffer
 * @retval Number of bytes written
 */
static int http_generate_html(char *buf, size_t buflen)
{
    int len = 0;
    uint32_t uptime_sec = HAL_GetTick() / 1000;
    uint32_t uptime_min = uptime_sec / 60;
    uint32_t uptime_hr = uptime_min / 60;

    /* Get IP address */
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
             ip4_addr1(&gnetif.ip_addr),
             ip4_addr2(&gnetif.ip_addr),
             ip4_addr3(&gnetif.ip_addr),
             ip4_addr4(&gnetif.ip_addr));

    /* Get MAC address */
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
             gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);

    /* Build HTML page */
    len = snprintf(buf, buflen,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <title>PeriphNet - Hello World</title>\n"
        "    <style>\n"
        "        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }\n"
        "        .container { background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); max-width: 600px; }\n"
        "        h1 { color: #2c3e50; border-bottom: 3px solid #3498db; padding-bottom: 10px; }\n"
        "        .info { margin: 20px 0; }\n"
        "        .info-item { margin: 10px 0; padding: 10px; background: #ecf0f1; border-radius: 4px; }\n"
        "        .label { font-weight: bold; color: #34495e; }\n"
        "        .value { color: #16a085; font-family: monospace; }\n"
        "        .footer { margin-top: 30px; padding-top: 20px; border-top: 1px solid #bdc3c7; color: #7f8c8d; font-size: 0.9em; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"container\">\n"
        "        <h1>Hello World v1.0.0</h1>\n"
        "        <p>PeriphNet - STM32F407VET6 Industrial Firmware</p>\n"
        "        \n"
        "        <div class=\"info\">\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">Device:</span> \n"
        "                <span class=\"value\">STM32F407VET6</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">IP Address:</span> \n"
        "                <span class=\"value\">%s</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">MAC Address:</span> \n"
        "                <span class=\"value\">%s</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">Uptime:</span> \n"
        "                <span class=\"value\">%luh %lum %lus</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">Firmware:</span> \n"
        "                <span class=\"value\">v1.0.0 (Milestone 1)</span>\n"
        "            </div>\n"
        "            <div class=\"info-item\">\n"
        "                <span class=\"label\">Stack:</span> \n"
        "                <span class=\"value\">FreeRTOS + lwIP</span>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <div class=\"footer\">\n"
        "            <p>Milestone 1: Ethernet + HTTP Server - COMPLETE</p>\n"
        "            <p>Next: Bootloader implementation</p>\n"
        "        </div>\n"
        "    </div>\n"
        "</body>\n"
        "</html>",
        ip_str, mac_str, uptime_hr, uptime_min % 60, uptime_sec % 60);

    return len;
}

/**
 * @brief  TCP receive callback - handle HTTP request
 * @param  arg: User argument (not used)
 * @param  pcb: TCP protocol control block
 * @param  p: Received packet buffer
 * @param  err: Error status
 * @retval ERR_OK
 */
static err_t http_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char *request;
    static char response_buf[1024];  /* Reduced buffer size to prevent stack issues */
    int html_len;
    err_t ret_err;

    /* Client closed connection */
    if (p == NULL) {
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Get request data and check size */
    request = (char *)p->payload;
    if (p->len > 512) {  /* Reject oversized requests */
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Simple parsing - check if it's a GET request for root */
    if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /index", 10) == 0) {
        /* Generate HTML response */
        strncpy(response_buf, http_200_header, sizeof(response_buf) - 1);
        response_buf[sizeof(response_buf) - 1] = '\0';
        html_len = strlen(response_buf);
        if (html_len < sizeof(response_buf) - 1) {
            html_len += http_generate_html(response_buf + html_len, sizeof(response_buf) - html_len);
        }

        /* Send response */
        ret_err = tcp_write(pcb, response_buf, html_len, TCP_WRITE_FLAG_COPY);
        if (ret_err == ERR_OK) {
            tcp_output(pcb);
        }
    } else {
        /* 404 Not Found */
        tcp_write(pcb, http_404_header, strlen(http_404_header), TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
    }

    /* Acknowledge received data */
    tcp_recved(pcb, p->tot_len);

    /* Free packet buffer */
    pbuf_free(p);

    /* Close connection after sending response */
    tcp_close(pcb);

    return ERR_OK;
}

/**
 * @brief  TCP accept callback - new client connected
 * @param  arg: User argument (not used)
 * @param  newpcb: New connection PCB
 * @param  err: Error status
 * @retval ERR_OK
 */
static err_t http_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    /* Set receive callback */
    tcp_recv(newpcb, http_recv_callback);

    return ERR_OK;
}

/**
 * @brief  Initialize HTTP server
 * @param  None
 * @retval None
 */
void http_server_init(void)
{
    struct tcp_pcb *pcb;

    /* Create new TCP PCB */
    pcb = tcp_new();

    if (pcb != NULL) {
        err_t err;

        /* Bind to port 80 */
        err = tcp_bind(pcb, IP_ADDR_ANY, HTTP_SERVER_PORT);

        if (err == ERR_OK) {
            /* Start listening */
            pcb = tcp_listen(pcb);

            /* Set accept callback */
            tcp_accept(pcb, http_accept_callback);
        } else {
            /* Binding failed, deallocate PCB */
            memp_free(MEMP_TCP_PCB, pcb);
        }
    }
}

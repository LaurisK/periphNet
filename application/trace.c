/**
 ******************************************************************************
 * @file    trace.c
 * @brief   Trice TCP/IP trace server implementation
 ******************************************************************************
 */

#include "trace.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdio.h>

/* Trace client connection structure */
typedef struct {
    struct tcp_pcb *pcb;
    uint8_t active;
} trace_client_t;

/* Trace server state */
static struct {
    struct tcp_pcb *server_pcb;
    trace_client_t clients[TRACE_MAX_CLIENTS];
    uint32_t message_counter;
} trace_server = {0};

/* Forward declarations */
static void trace_task(void *argument);
static err_t trace_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t trace_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static void trace_err_callback(void *arg, err_t err);
static void trace_broadcast_message(const char *msg, uint16_t len);

/**
 * @brief  Initialize trace system
 * @note   Creates trace task, task will initialize TCP server when network is ready
 */
void trace_init(void)
{
    BaseType_t result;

    /* Create trace task */
    result = xTaskCreate(
        trace_task,
        "TraceTask",
        TRACE_TASK_STACK_SIZE,
        NULL,
        TRACE_TASK_PRIORITY,
        NULL
    );

    if (result != pdPASS) {
        /* Task creation failed - handle error */
        /* For now, just return - trace won't be available */
        return;
    }
}

/**
 * @brief  Trace task - manages TCP server and periodic messages
 * @param  argument: Not used
 */
static void trace_task(void *argument)
{
    err_t err;
    TickType_t last_message_time;
    char message_buf[64];
    uint16_t msg_len;

    (void)argument;

    /* Wait for network to be ready */
    /* Simple delay - in production, should wait for network up event */
    vTaskDelay(pdMS_TO_TICKS(5000));

    /* Create TCP server */
    trace_server.server_pcb = tcp_new();

    if (trace_server.server_pcb == NULL) {
        /* Failed to create PCB - delete task */
        vTaskDelete(NULL);
        return;
    }

    /* Bind to trace port */
    err = tcp_bind(trace_server.server_pcb, IP_ADDR_ANY, TRACE_SERVER_PORT);

    if (err != ERR_OK) {
        /* Binding failed */
        tcp_close(trace_server.server_pcb);
        vTaskDelete(NULL);
        return;
    }

    /* Start listening */
    trace_server.server_pcb = tcp_listen(trace_server.server_pcb);
    tcp_accept(trace_server.server_pcb, trace_accept_callback);

    /* Initialize client array */
    for (int i = 0; i < TRACE_MAX_CLIENTS; i++) {
        trace_server.clients[i].pcb = NULL;
        trace_server.clients[i].active = 0;
    }

    last_message_time = xTaskGetTickCount();

    /* Main trace loop */
    while (1) {
        /* Send periodic message every 1 second */
        if ((xTaskGetTickCount() - last_message_time) >= pdMS_TO_TICKS(1000)) {
            trace_server.message_counter++;

            msg_len = snprintf(message_buf, sizeof(message_buf),
                              "Trace message %lu: One second passed\r\n",
                              trace_server.message_counter);

            trace_broadcast_message(message_buf, msg_len);

            last_message_time = xTaskGetTickCount();
        }

        /* Small delay to prevent busy loop */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief  TCP accept callback - new client connected
 */
static err_t trace_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    int slot = -1;

    (void)arg;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    /* Find free client slot */
    for (int i = 0; i < TRACE_MAX_CLIENTS; i++) {
        if (!trace_server.clients[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        /* No free slots - reject connection */
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    /* Accept connection */
    trace_server.clients[slot].pcb = newpcb;
    trace_server.clients[slot].active = 1;

    /* Set callbacks */
    tcp_arg(newpcb, (void *)(intptr_t)slot);  /* Pass slot index as arg */
    tcp_recv(newpcb, trace_recv_callback);
    tcp_err(newpcb, trace_err_callback);

    /* Send welcome message */
    const char *welcome = "Trice trace server connected\r\n";
    tcp_write(newpcb, welcome, strlen(welcome), TCP_WRITE_FLAG_COPY);
    tcp_output(newpcb);

    return ERR_OK;
}

/**
 * @brief  TCP receive callback - handle incoming data
 * @note   We don't expect data from clients, but need to handle connection close
 */
static err_t trace_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    int slot = (int)(intptr_t)arg;

    (void)err;

    /* Client closed connection */
    if (p == NULL) {
        if (slot >= 0 && slot < TRACE_MAX_CLIENTS) {
            trace_server.clients[slot].pcb = NULL;
            trace_server.clients[slot].active = 0;
        }
        tcp_close(pcb);
        return ERR_OK;
    }

    /* Acknowledge received data (but ignore it) */
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

/**
 * @brief  TCP error callback - handle connection errors
 */
static void trace_err_callback(void *arg, err_t err)
{
    int slot = (int)(intptr_t)arg;

    (void)err;

    /* Mark client as inactive */
    if (slot >= 0 && slot < TRACE_MAX_CLIENTS) {
        trace_server.clients[slot].pcb = NULL;
        trace_server.clients[slot].active = 0;
    }
}

/**
 * @brief  Broadcast message to all connected clients
 */
static void trace_broadcast_message(const char *msg, uint16_t len)
{
    err_t err;

    for (int i = 0; i < TRACE_MAX_CLIENTS; i++) {
        if (trace_server.clients[i].active && trace_server.clients[i].pcb != NULL) {
            err = tcp_write(trace_server.clients[i].pcb, msg, len, TCP_WRITE_FLAG_COPY);

            if (err == ERR_OK) {
                tcp_output(trace_server.clients[i].pcb);
            } else {
                /* Write failed - close connection */
                tcp_close(trace_server.clients[i].pcb);
                trace_server.clients[i].pcb = NULL;
                trace_server.clients[i].active = 0;
            }
        }
    }
}

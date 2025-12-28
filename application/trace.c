/**
 ******************************************************************************
 * @file    trace.c
 * @brief   Trice TCP/IP trace server implementation with double buffer
 ******************************************************************************
 * Minimal Trice integration:
 * - TriceTransfer() called periodically (every 50ms) to swap buffers
 * - Hooks to detect when Trice writes data
 * - Buffer fill level monitoring
 * - TCP sending implementation to be added later
 ******************************************************************************
 */

#include "trace.h"
#include "trice.h"
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

/* Trice buffer monitoring state */
static struct {
    volatile uint8_t new_data_available;  /* Flag: new trace data written */
    volatile size_t last_buffer_length;    /* Length of last buffer received */
} trice_monitor = {0};

/* Trace server state */
static struct {
    struct tcp_pcb *server_pcb;
    trace_client_t clients[TRACE_MAX_CLIENTS];
} trace_server = {0};

/* Forward declarations */
static void trace_task(void *argument);
static err_t trace_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t trace_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static void trace_err_callback(void *arg, err_t err);

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
 * @brief  Trace task - manages TCP server and Trice buffer transfers
 * @param  argument: Not used
 */
static void trace_task(void *argument)
{
    err_t err;

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

    /* Main trace loop */
    while (1) {
        /* Call TriceTransfer() every 50ms to swap buffers and transmit */
        /* This checks if transmission is complete and swaps double buffer */
        TriceTransfer();

        /* Delay to prevent busy loop and set transfer rate */
        vTaskDelay(pdMS_TO_TICKS(50));
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
    err_t write_err = tcp_write(newpcb, welcome, strlen(welcome), TCP_WRITE_FLAG_COPY);
    if (write_err == ERR_OK) {
        tcp_output(newpcb);
    }

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
 ******************************************************************************
 * Trice Integration Functions - Minimal Hooks
 * These functions are called by the Trice library
 * TCP sending will be implemented later
 ******************************************************************************
 */

/**
 * @brief  Trice auxiliary output function - hook for monitoring only
 * @param  enc: Pointer to encoded trice data
 * @param  encLen: Length of encoded data
 * @note   Called by Trice library from TriceNonBlockingDeferredWrite8()
 * @note   Currently only sets flag and records length - sending not implemented yet
 */
void TriceNonBlockingDeferredWrite8Auxiliary(const uint8_t* enc, size_t encLen)
{
    (void)enc;  /* Not used yet - sending will be implemented later */

    if (encLen == 0) {
        return;
    }

    /* Set flag to indicate new trace data is available */
    trice_monitor.new_data_available = 1;
    trice_monitor.last_buffer_length = encLen;
}

/**
 * @brief  Trice output depth function - returns transmission state
 * @return 0 (always ready - no actual transmission yet)
 * @note   Called by TriceTransfer() to check if buffer swap is safe
 * @note   Returns 0 to allow immediate buffer swap (sending not implemented yet)
 */
unsigned TriceOutDepth(void)
{
    /* Always return 0 - no transmission happening yet */
    /* This allows Trice to swap buffers immediately */
    return 0;
}

/**
 ******************************************************************************
 * Public API Functions - Buffer Monitoring
 ******************************************************************************
 */

/**
 * @brief  Check if new trace data is available
 * @return 1 if new data available, 0 otherwise
 * @note   Call trace_clear_data_flag() after processing to reset flag
 */
uint8_t trace_has_new_data(void)
{
    return trice_monitor.new_data_available;
}

/**
 * @brief  Get length of last trace buffer received
 * @return Length in bytes of last buffer from Trice
 */
size_t trace_get_last_buffer_length(void)
{
    return trice_monitor.last_buffer_length;
}

/**
 * @brief  Clear the new data available flag
 * @note   Call after processing new trace data
 */
void trace_clear_data_flag(void)
{
    trice_monitor.new_data_available = 0;
}

/**
 * @brief  Get current Trice half-buffer fill level
 * @return Number of bytes used in current half buffer
 * @note   Accesses Trice internal variables - requires Trice library
 */
size_t trace_get_buffer_fill_level(void)
{
    /* Access Trice double buffer internal state */
    /* These are defined in triceDoubleBuffer.c */
    extern uint32_t* TriceBufferWritePosition;
    extern uint32_t* TriceBufferWritePositionStart;

    /* Calculate fill level in 32-bit words, convert to bytes */
    size_t fill_words = TriceBufferWritePosition - TriceBufferWritePositionStart;
    return fill_words * 4;  /* Convert words to bytes */
}

#include "trace.h"
#include "trice.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdio.h>
#include "iwdg.h"

typedef struct {
    struct tcp_pcb *pcb;
    uint8_t active;
} trace_client_t;

static struct {
    volatile uint8_t new_data_available;
    volatile size_t last_buffer_length;
} trice_monitor = {0};

static struct {
    struct tcp_pcb *server_pcb;
    trace_client_t clients[TRACE_MAX_CLIENTS];
} trace_server = {0};

static void trace_task(void *argument);
static err_t trace_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t trace_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
static void trace_err_callback(void *arg, err_t err);

/**
 * @brief Initialize the trace system by creating the trace task.
 * @note The TCP server is started inside the task after a network-ready delay.
 */
void trace_init(void)
{
    BaseType_t result;

    result = xTaskCreate(
        trace_task,
        "TraceTask",
        TRACE_TASK_STACK_SIZE,
        NULL,
        TRACE_TASK_PRIORITY,
        NULL
    );

    if (result != pdPASS) {
        return;
    }
}

/**
 * @brief Trace task body; starts the TCP server and drives periodic TriceTransfer calls.
 * @param argument Unused.
 */
static void trace_task(void *argument)
{
    err_t err;
    uint32_t counter = 0;

    (void)argument;
    for (int i = 0; i < 5; i++) {
        MX_IWDG_Kick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    trace_server.server_pcb = tcp_new();

    if (trace_server.server_pcb == NULL) {
        vTaskDelete(NULL);
        return;
    }

    err = tcp_bind(trace_server.server_pcb, IP_ADDR_ANY, TRACE_SERVER_PORT);

    if (err != ERR_OK) {
        tcp_close(trace_server.server_pcb);
        vTaskDelete(NULL);
        return;
    }

    trace_server.server_pcb = tcp_listen(trace_server.server_pcb);
    tcp_accept(trace_server.server_pcb, trace_accept_callback);

    for (int i = 0; i < TRACE_MAX_CLIENTS; i++) {
        trace_server.clients[i].pcb = NULL;
        trace_server.clients[i].active = 0;
    }

    while (1) {
        TriceTransfer();

        counter++;
        TRice("Counter is - %d\n", counter);

        if (counter % 20 == 0) {
            TaskHandle_t tcpip_h = xTaskGetHandle("tcpip_thread");
            if (tcpip_h) {
                TRice("tcpip HWM=%u words\n",
                      (unsigned)uxTaskGetStackHighWaterMark(tcpip_h));
            }
            TRice("upload HWM=%u words heap=%u\n",
                  (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("ImgUp")),
                  (unsigned)xPortGetFreeHeapSize());

            MX_IWDG_Kick();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief TCP accept callback; registers a new client in the first available slot.
 * @param arg Unused.
 * @param newpcb Newly accepted TCP PCB.
 * @param err lwIP error status.
 * @return ERR_OK on success, ERR_VAL if newpcb is invalid, ERR_ABRT if no slot is available.
 */
static err_t trace_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    int slot = -1;

    (void)arg;

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    for (int i = 0; i < TRACE_MAX_CLIENTS; i++) {
        if (!trace_server.clients[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    trace_server.clients[slot].pcb = newpcb;
    trace_server.clients[slot].active = 1;

    tcp_arg(newpcb, (void *)(intptr_t)slot);
    tcp_recv(newpcb, trace_recv_callback);
    tcp_err(newpcb, trace_err_callback);

    TRice("New client received for trice stream.");

    return ERR_OK;
}

/**
 * @brief TCP receive callback; handles client disconnection and discards any received data.
 * @param arg Slot index cast to pointer.
 * @param pcb TCP PCB for the connection.
 * @param p Received pbuf; NULL indicates the client closed the connection.
 * @param err lwIP error status (unused).
 * @return ERR_OK always.
 */
static err_t trace_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    int slot = (int)(intptr_t)arg;

    (void)err;

    if (p == NULL) {
        if (slot >= 0 && slot < TRACE_MAX_CLIENTS) {
            trace_server.clients[slot].pcb = NULL;
            trace_server.clients[slot].active = 0;
        }
        tcp_close(pcb);
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    return ERR_OK;
}

/**
 * @brief TCP error callback; marks the client slot as inactive.
 * @param arg Slot index cast to pointer.
 * @param err lwIP error code (unused).
 */
static void trace_err_callback(void *arg, err_t err)
{
    int slot = (int)(intptr_t)arg;

    (void)err;

    if (slot >= 0 && slot < TRACE_MAX_CLIENTS) {
        trace_server.clients[slot].pcb = NULL;
        trace_server.clients[slot].active = 0;
    }
}

/**
 * @brief Trice auxiliary output hook; sends encoded trace data to all connected TCP clients.
 * @param enc Pointer to the encoded Trice data buffer.
 * @param encLen Number of bytes in the buffer.
 * @note Called by the Trice library from TriceNonBlockingDeferredWrite8().
 */
void TriceNonBlockingDeferredWrite8Auxiliary(const uint8_t* enc, size_t encLen)
{
    if (encLen == 0) {
        return;
    }

    trice_monitor.new_data_available = 1;
    trice_monitor.last_buffer_length = encLen;

    for (int i = 0; i < TRACE_MAX_CLIENTS; i++) {
        if (trace_server.clients[i].active && trace_server.clients[i].pcb != NULL) {
            uint16_t available = tcp_sndbuf(trace_server.clients[i].pcb);

            if (available >= encLen) {
                err_t err = tcp_write(trace_server.clients[i].pcb, enc, encLen, TCP_WRITE_FLAG_COPY);

                if (err == ERR_OK) {
                    tcp_output(trace_server.clients[i].pcb);
                }
            }
        }
    }
}


/**
 * @brief Check whether new trace data has been written since the last check.
 * @return 1 if new data is available, 0 otherwise.
 */
uint8_t trace_has_new_data(void)
{
    return trice_monitor.new_data_available;
}

/**
 * @brief Return the byte length of the last trace buffer delivered to the output hook.
 * @return Number of bytes in the last buffer.
 */
size_t trace_get_last_buffer_length(void)
{
    return trice_monitor.last_buffer_length;
}

/**
 * @brief Clear the new-data-available flag.
 */
void trace_clear_data_flag(void)
{
    trice_monitor.new_data_available = 0;
}

/**
 * @brief Return the current fill level of the active Trice half-buffer.
 * @return Number of bytes used in the current half-buffer.
 * @note Accesses Trice internal variables defined in triceDoubleBuffer.c.
 */
size_t trace_get_buffer_fill_level(void)
{
    extern uint32_t* TriceBufferWritePosition;
    extern uint32_t* TriceBufferWritePositionStart;

    size_t fill_words = TriceBufferWritePosition - TriceBufferWritePositionStart;
    return fill_words * 4;
}

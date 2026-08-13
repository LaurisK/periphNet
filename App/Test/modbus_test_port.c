/**
 * @file    modbus_test_port.c
 * @brief   Test peripheral — see modbus_test_port.h.
 */

#include "App/Test/modbus_test_port.h"
#include "App/Modbus/modbus_port.h"

#include "trice.h"

#include <string.h>

static uint8_t  s_reply[MB_PORT_BUF_SIZE];
static uint16_t s_replyLen;
static uint8_t  s_silent;          /* answer the next request with silence */
static uint8_t  s_staged;

static uint8_t  s_lastReq[MB_PORT_BUF_SIZE];
static uint16_t s_lastReqLen;
static uint32_t s_requests;

/* The one call down.  Completes inline like the RS485 driver does today; when
 * the engine goes asynchronous at step 10 this can complete from a timer or a
 * socket callback instead and nothing above it changes. */
static int test_submit(void *ctx, const uint8_t *tx, uint16_t txLen,
                       uint8_t *rx, uint16_t rxSize,
                       const sModbusPortParams *p)
{
    (void)ctx;
    (void)p;

    s_requests++;
    s_lastReqLen = (txLen > sizeof(s_lastReq)) ? 0u : txLen;
    if (s_lastReqLen > 0u) {
        memcpy(s_lastReq, tx, s_lastReqLen);
    }

    if (s_silent || !s_staged) {
        /* Silence is a legitimate answer, and the only one a bench board can
         * give for a device that is not there. */
        s_silent = 0u;
        s_staged = 0u;
        Modbus_PortDone(mbPort_test, mbPortDone_timeout, 0);
        return 0;
    }

    uint16_t n = (s_replyLen > rxSize) ? rxSize : s_replyLen;
    memcpy(rx, s_reply, n);
    s_staged = 0u;

    /* A staged frame is delivered EXACTLY as staged, CRC and all: the engine
     * decides what a valid reply is, so a corrupt frame must reach it (§5.1). */
    Modbus_PortDone(mbPort_test, mbPortDone_frame, n);
    return 0;
}

int ModbusTestPort_Register(void)
{
    static const sModbusPortDriver drv = { test_submit, NULL };

    return Modbus_PortRegister(mbPort_test, &drv);
}

int ModbusTestPort_StageReply(const uint8_t *frame, uint16_t len)
{
    if (frame == NULL || len == 0u || len > sizeof(s_reply)) {
        return -1;
    }
    memcpy(s_reply, frame, len);
    s_replyLen = len;
    s_silent   = 0u;
    s_staged   = 1u;
    return 0;
}

void ModbusTestPort_StageSilence(void)
{
    s_silent = 1u;
    s_staged = 1u;
}

uint32_t ModbusTestPort_RequestCount(void)
{
    return s_requests;
}

uint16_t ModbusTestPort_LastRequest(uint8_t *out, uint16_t maxLen)
{
    uint16_t n = (s_lastReqLen > maxLen) ? maxLen : s_lastReqLen;

    if (out != NULL && n > 0u) {
        memcpy(out, s_lastReq, n);
    }
    return n;
}

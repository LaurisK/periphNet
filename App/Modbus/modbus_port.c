/**
 * @file    modbus_port.c
 * @brief   The port table — see modbus_port.h.
 *
 * This is the engine's side of the frame-level contract: it owns the buffers,
 * forms the request, hands it to whichever driver holds the slot, waits for
 * exactly one completion, and parses the reply.  The driver decides how
 * reception ended; THIS file decides what the frame means.
 *
 * STILL SYNCHRONOUS, deliberately (docs/modbus.md §10 step 8): the contract
 * lands first, behind the existing blocking engine, so it is proven before
 * anything depends on its asynchrony.  `port_wait` is the one place that
 * changes at step 10 — from spinning on a flag to blocking on the modbus
 * task's queue.
 */

#include "App/Modbus/modbus_port.h"
#include "App/Modbus/modbus_internal.h"
#include "App/system.h"

#include "modbus_frame.h"

#include "cmsis_os.h"
#include "trice.h"

#include <string.h>

/* Buffers MUST be main SRAM: a DMA driver writes into them and CCM is
 * CPU-only.  Two ports cost about 1 KB. */
typedef struct {
    sModbusPortDriver drv;
    volatile uint8_t  inUse;
    volatile uint8_t  busy;        /* a frame is out                       */
    volatile uint8_t  done;        /* a completion has been posted         */
    volatile uint8_t  how;         /* eModbusPortDone                      */
    volatile uint16_t rxLen;
    uint8_t           tx[MB_PORT_BUF_SIZE];
    uint8_t           rx[MB_PORT_BUF_SIZE];
} sPort;

static sPort s_ports[MB_PORT_COUNT];

/* One completion signal per port.  Modbus_PortDone runs in whatever context
 * the driver completes in — an ISR for a DMA UART — so it only posts, and the
 * engine waits for exactly one thing (§5.1). */
static osSemaphoreId_t s_doneSem[MB_PORT_COUNT];

static volatile int s_monitor;

/* --------------------------------------------------------------------------
 * Registration and completion — the two calls the contract is made of
 * -------------------------------------------------------------------------- */

int Modbus_PortRegister(uint8_t portId, const sModbusPortDriver *drv)
{
    if (portId >= MB_PORT_COUNT || drv == NULL || drv->submit == NULL) {
        return -1;
    }

    if (s_doneSem[portId] == NULL) {
        s_doneSem[portId] = osSemaphoreNew(1, 0, NULL);
        if (s_doneSem[portId] == NULL) {
            return -1;
        }
    }

    s_ports[portId].drv = *drv;
    __DMB();                       /* publish the driver before the flag */
    s_ports[portId].inUse = 1u;
    return 0;
}

void Modbus_PortDone(uint8_t portId, eModbusPortDone how, uint16_t len)
{
    sPort *p;

    if (portId >= MB_PORT_COUNT) {
        return;
    }
    p = &s_ports[portId];

    /* A completion for a port with nothing outstanding is a driver bug or a
     * late reply to an abandoned request; either way it is discarded, never
     * written into a buffer somebody may already be reading. */
    if (!p->busy || p->done) {
        return;
    }

    p->rxLen = (how == mbPortDone_frame && len <= MB_PORT_BUF_SIZE) ? len : 0u;
    p->how   = (uint8_t)how;
    __DMB();
    p->done  = 1u;

    if (s_doneSem[portId] != NULL) {
        (void)osSemaphoreRelease(s_doneSem[portId]);
    }
}

int ModbusPort_IsRegistered(uint8_t portId)
{
    return (portId < MB_PORT_COUNT) && s_ports[portId].inUse;
}

void ModbusPort_SetMonitor(int enable)
{
    s_monitor = enable ? 1 : 0;
}

int ModbusPort_GetMonitor(void)
{
    return s_monitor;
}

/* --------------------------------------------------------------------------
 * Frame monitoring — chunked hex dump; each record stays small and records
 * without '\n' are joined into one output line by the trice tool.
 *
 * It lives here rather than in a driver because the module owns the buffers
 * and sees both directions, so one implementation serves every port.
 * -------------------------------------------------------------------------- */

static void dump_bytes(const uint8_t *buf, uint16_t len)
{
    uint16_t i = 0;

    while ((uint16_t)(len - i) >= 8U) {
        const uint8_t *c = &buf[i];
        uint8_t b0 = c[0], b1 = c[1], b2 = c[2], b3 = c[3];
        uint8_t b4 = c[4], b5 = c[5], b6 = c[6], b7 = c[7];
        TRice8("%02x %02x %02x %02x %02x %02x %02x %02x ",
               b0, b1, b2, b3, b4, b5, b6, b7);
        i += 8U;
    }
    while (i < len) {
        uint8_t b0 = buf[i];
        TRice8("%02x ", b0);
        i++;
    }
    TRice("\n");
}

/* --------------------------------------------------------------------------
 * One transaction
 * -------------------------------------------------------------------------- */

/* Wait for the single completion this transaction is owed.
 *
 * The engine waits for EXACTLY ONE THING, and it blocks rather than spins: a
 * driver that completes from an ISR wakes it, and one that completes inline
 * has already released the semaphore before submit returned. */
static int port_wait(uint8_t portId, sPort *p, uint32_t timeout_ms)
{
    if (p->done) {
        /* Inline completion: consume the token it left behind. */
        (void)osSemaphoreAcquire(s_doneSem[portId], 0);
        return 0;
    }
    if (osSemaphoreAcquire(s_doneSem[portId], timeout_ms) != osOK) {
        return -1;
    }
    return p->done ? 0 : -1;
}

static int port_submit(uint8_t portId, uint16_t txLen,
                       const sModbusPortParams *params,
                       eModbusPortDone *howOut, uint16_t *rxLenOut)
{
    sPort *p = &s_ports[portId];

    if (!p->inUse) {
        return -1;                 /* a port with no driver is disabled */
    }

    p->done  = 0u;
    p->rxLen = 0u;
    p->how   = (uint8_t)mbPortDone_txFailed;
    __DMB();
    p->busy  = 1u;

    if (s_monitor) {
        TRice("Modbus TX[%u]: ", txLen);
        dump_bytes(p->tx, txLen);
    }

    /* The return value is ADVISORY: a driver that cannot send says so through
     * mbPortDone_txFailed, because a transaction is exactly one event. */
    (void)p->drv.submit(p->drv.ctx, p->tx, txLen, p->rx, MB_PORT_BUF_SIZE,
                        params);

    /* The response timeout is the driver's; this is only a backstop against a
     * driver that never completes at all. */
    uint32_t backstop = (params->responseTimeout_ms == 0u)
                            ? 1000u : params->responseTimeout_ms;
    if (port_wait(portId, p, backstop + 500u) != 0) {
        p->busy = 0u;
        *howOut = mbPortDone_timeout;
        *rxLenOut = 0u;
        return 0;
    }

    *howOut   = (eModbusPortDone)p->how;
    *rxLenOut = p->rxLen;
    p->busy   = 0u;

    if (s_monitor && *howOut == mbPortDone_frame) {
        TRice("Modbus RX[%u]: ", *rxLenOut);
        dump_bytes(p->rx, *rxLenOut);
    }
    return 0;
}

/* How reception ended, before the frame means anything. */
static int16_t err_from_done(eModbusPortDone how)
{
    switch (how) {
    case mbPortDone_timeout:   return mbErr_timeout;
    case mbPortDone_lineError: return mbErr_lineError;
    case mbPortDone_txFailed:  return mbErr_txFailed;
    default:                   return mbErr_ok;
    }
}

int16_t ModbusPort_Read(uint8_t portId, const sModbusPortParams *params,
                        uint8_t slave, uint8_t fc, uint16_t addr,
                        uint16_t count, uint16_t *regs)
{
    sPort          *p = &s_ports[portId];
    eModbusPortDone how;
    uint16_t        rxLen;
    int             txLen;
    uint8_t         exc = 0;

    if (portId >= MB_PORT_COUNT || params == NULL || regs == NULL) {
        return mbErr_badArg;
    }

    txLen = MbFrame_BuildRead(slave, fc, addr, count, p->tx, MB_PORT_BUF_SIZE);
    if (txLen < 0) {
        return mbErr_badArg;
    }
    if (port_submit(portId, (uint16_t)txLen, params, &how, &rxLen) != 0) {
        return mbErr_txFailed;
    }
    if (how != mbPortDone_frame) {
        return err_from_done(how);
    }

    return ModbusErr_FromFrame(
        MbFrame_ParseRead(p->rx, rxLen, slave, fc, count, regs, &exc), exc);
}

/* `writeFc` SELECTS THE WRITE FRAME and is a dialect fact, not a code path
 * (§3.3): 6 lands exactly one register, 16 lands as many as the point is
 * wide.  A slave with no FC06 handler — the JK — is served by data alone. */
int16_t ModbusPort_Write(uint8_t portId, const sModbusPortParams *params,
                         uint8_t slave, uint8_t writeFc, uint16_t reg,
                         const uint16_t *values, uint16_t count)
{
    sPort          *p = &s_ports[portId];
    eModbusPortDone how;
    uint16_t        rxLen;
    int             txLen;
    uint8_t         exc = 0;
    uint8_t         fc  = (writeFc == 16u) ? 0x10u : 0x06u;

    if (portId >= MB_PORT_COUNT || params == NULL || values == NULL ||
        count == 0u) {
        return mbErr_badArg;
    }
    if (fc == 0x06u && count != 1u) {
        return mbErr_badArg;       /* FC06 lands exactly one register */
    }

    txLen = (fc == 0x10u)
                ? MbFrame_BuildWriteMulti(slave, reg, values, count, p->tx,
                                          MB_PORT_BUF_SIZE)
                : MbFrame_BuildWrite(slave, reg, values[0], p->tx,
                                     MB_PORT_BUF_SIZE);
    if (txLen < 0) {
        return mbErr_badArg;
    }
    if (port_submit(portId, (uint16_t)txLen, params, &how, &rxLen) != 0) {
        return mbErr_txFailed;
    }
    if (how != mbPortDone_frame) {
        return err_from_done(how);
    }

    return ModbusErr_FromFrame(
        MbFrame_ParseWrite(p->rx, rxLen, slave, fc, &exc), exc);
}

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
 * Line occupancy
 *
 * port_submit() is the ONE place every frame on every port passes through --
 * engine sequences and API requests alike -- so it is the only place that can
 * say how much of a line's time is already spoken for.  What is measured is
 * the whole transaction as the wire sees it: the driver's pre-transmit
 * silence, the frame out, the slave's think time, the frame back, and the
 * end-of-frame idle.  That is what has to fit inside a period, and it is the
 * number to look at before putting another device on the pair.
 *
 * A TIMEOUT IS BUSY TIME, and it is the reading that matters most.  A slave
 * that does not answer costs the line its full response timeout and returns
 * nothing; a mis-addressed device on a wrong baud is therefore not a quiet
 * failure but the most expensive traffic on the bus, and it starves
 * everything scheduled behind it.  Averaging it away would hide exactly the
 * case this exists to catch.
 *
 * The window is a ring of one uint16 per second.  A frame is charged in full
 * to the second it ENDED in, which mis-attributes at most one straddling
 * frame per second -- accepted deliberately: splitting a transaction across
 * slots buys precision nobody needs from an occupancy figure.
 * -------------------------------------------------------------------------- */

#define MB_BUSY_WINDOW_SEC  60u

typedef struct {
    uint32_t txns;
    uint32_t busy_ms;
    uint32_t startTick;
    uint32_t winSec;                    /* second index of the newest slot */
    uint16_t last_ms;
    uint16_t max_ms;
    uint16_t win[MB_BUSY_WINDOW_SEC];
} sPortBusy;

static sPortBusy s_busy[MB_PORT_COUNT];

/* Advanced LAZILY, by whoever touches the ring next.  An idle port posts no
 * completions, so a window that only moved on a frame would keep reporting
 * the traffic that stopped a minute ago -- the one reading a busyness meter
 * must never give. */
static void busy_advance(sPortBusy *b, uint32_t sec)
{
    uint32_t behind = sec - b->winSec;   /* unsigned: tick wrap is fine */

    if (behind == 0u) {
        return;
    }
    if (behind >= MB_BUSY_WINDOW_SEC) {
        memset(b->win, 0, sizeof(b->win));
    } else {
        for (uint32_t i = 1u; i <= behind; i++) {
            b->win[(b->winSec + i) % MB_BUSY_WINDOW_SEC] = 0u;
        }
    }
    b->winSec = sec;
}

/* The accumulator is written on the modbus task and read on the http task, and
 * the ring advance is not one store -- hence the PRIMASK save/restore rather
 * than a mutex: it is a handful of instructions and it must be callable from
 * either. */
static void busy_charge(uint8_t portId, uint32_t t0, uint32_t t1)
{
    sPortBusy *b  = &s_busy[portId];
    uint32_t   ms = t1 - t0;
    uint32_t   pm;

    if (ms > 0xFFFFu) {
        ms = 0xFFFFu;
    }

    pm = __get_PRIMASK();
    __disable_irq();

    busy_advance(b, t1 / 1000u);

    b->txns++;
    b->busy_ms += ms;
    b->last_ms  = (uint16_t)ms;
    if ((uint16_t)ms > b->max_ms) {
        b->max_ms = (uint16_t)ms;
    }
    {
        uint32_t idx = (t1 / 1000u) % MB_BUSY_WINDOW_SEC;
        uint32_t sum = (uint32_t)b->win[idx] + ms;
        b->win[idx] = (sum > 0xFFFFu) ? 0xFFFFu : (uint16_t)sum;
    }

    __set_PRIMASK(pm);
}

int ModbusPort_GetBusy(uint8_t portId, sModbusBusStats *out)
{
    sPortBusy *b;
    uint32_t   now = HAL_GetTick();
    uint32_t   winSum = 0u, winSec, pm;

    if (portId >= MB_PORT_COUNT || out == NULL) {
        return -1;
    }
    b = &s_busy[portId];

    pm = __get_PRIMASK();
    __disable_irq();
    busy_advance(b, now / 1000u);
    for (uint32_t i = 0u; i < MB_BUSY_WINDOW_SEC; i++) {
        winSum += b->win[i];
    }
    out->txns       = b->txns;
    out->busy_ms    = b->busy_ms;
    out->elapsed_ms = now - b->startTick;
    out->last_ms    = b->last_ms;
    out->max_ms     = b->max_ms;
    __set_PRIMASK(pm);

    /* The window cannot report on time that has not passed yet: right after a
     * reset or a boot its denominator is the elapsed time, not 60 s, and the
     * reply says which it used. */
    winSec = out->elapsed_ms / 1000u;
    if (winSec > MB_BUSY_WINDOW_SEC) {
        winSec = MB_BUSY_WINDOW_SEC;
    }
    out->window_sec    = (uint16_t)winSec;
    out->win_permille  = (winSec == 0u) ? 0u : (uint16_t)(winSum / winSec);
    if (out->win_permille > 1000u) {
        out->win_permille = 1000u;      /* a straddling frame, charged whole */
    }
    out->duty_permille = (out->elapsed_ms == 0u) ? 0u
                       : (uint16_t)(((uint64_t)out->busy_ms * 1000u)
                                    / out->elapsed_ms);
    out->registered    = s_ports[portId].inUse;
    return 0;
}

void ModbusPort_ResetBusy(uint8_t portId)
{
    sPortBusy *b;
    uint32_t   pm;

    if (portId >= MB_PORT_COUNT) {
        return;
    }
    b = &s_busy[portId];

    pm = __get_PRIMASK();
    __disable_irq();
    memset(b, 0, sizeof(*b));
    b->startTick = HAL_GetTick();
    b->winSec    = b->startTick / 1000u;
    __set_PRIMASK(pm);
}

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
    sPort   *p = &s_ports[portId];
    uint32_t t0;

    if (!p->inUse) {
        return -1;                 /* a port with no driver is disabled */
    }

    /* The line is occupied from here: the driver's pre-transmit silence is
     * inside submit, and it is real time on the wire like any other. */
    t0 = HAL_GetTick();

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
        busy_charge(portId, t0, HAL_GetTick());
        return 0;
    }

    *howOut   = (eModbusPortDone)p->how;
    *rxLenOut = p->rxLen;
    p->busy   = 0u;
    busy_charge(portId, t0, HAL_GetTick());

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

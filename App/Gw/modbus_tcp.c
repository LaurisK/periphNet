/**
 * @file    modbus_tcp.c
 * @brief   Modbus TCP gateway — see modbus_tcp.h for what it is and the three
 *          rules it is built on.
 *
 * A CONSUMER of App/Modbus, not part of it: it reaches the module only through
 * modbus.h, exactly as App/Can and the Trice sink do.  It lives outside
 * App/Modbus precisely because it includes lwIP, which files in that directory
 * may not (docs/modbus.md §2.2).
 *
 * ONE CONNECTION AT A TIME, deliberately.  `solis_modbus` keeps exactly one
 * client per `host:port` with one lock across every slave, so a second
 * concurrent connection is not a case that arises from the far end this
 * exists for — and serving connections sequentially is what keeps this a
 * single task with static buffers instead of a per-connection allocation on a
 * board with 5.4 KB of main SRAM free.  A second client waits in the listen
 * backlog rather than being refused.
 */

#include "App/Gw/modbus_tcp.h"
#include "App/Modbus/modbus.h"
#include "App/Mon/sysmon.h"
#include "App/Net/wg_link.h"

#include "lwip/api.h"

#include "cmsis_os.h"
#include "trice.h"

#include <string.h>

/* --------------------------------------------------------------------------
 * Wire constants
 * -------------------------------------------------------------------------- */

#define MBAP_HDR_LEN         7u    /* txnId, protoId, length, unitId        */
#define MBAP_MAX_PDU       253u    /* FC16 with 123 registers is the worst  */
#define MBAP_MAX_FRAME     (MBAP_HDR_LEN + MBAP_MAX_PDU)

/* The codes this gateway ORIGINATES.  A slave's own exception is passed
 * through verbatim and never mapped onto these — see modbus_tcp.h. */
#define MBEXC_ILLEGAL_FUNCTION   0x01u
#define MBEXC_ILLEGAL_ADDRESS    0x02u
#define MBEXC_ILLEGAL_VALUE      0x03u
#define MBEXC_SLAVE_BUSY         0x06u
#define MBEXC_GW_PATH            0x0Au
#define MBEXC_GW_TARGET          0x0Bu

/* Accept blocks for this long, then the loop wakes to re-check the tunnel
 * address and check in with sysmon.  Nothing else needs it: an idle listener
 * has nothing to do, and 5 s is far below the monitor's patience. */
#define ACCEPT_TIMEOUT_MS     5000u
#define IO_TIMEOUT_MS        30000u

/* An unprovisioned board retries at this interval rather than exiting, so a
 * tunnel configured over HTTP starts being served without a reboot. */
#define UNPROVISIONED_RETRY_MS 5000u

/* --------------------------------------------------------------------------
 * State
 *
 * Buffers live in .ccmram: they are touched only by this task's CPU — lwIP
 * copies in and out of them — and MAIN SRAM is the tightest region on this
 * board.  Nothing here is ever handed to DMA or to the flash driver, which is
 * the rule that makes CCM legal (CLAUDE.md, Key Constraints).
 * -------------------------------------------------------------------------- */

#define CCMRAM_BSS __attribute__((section(".ccmram")))

static CCMRAM_BSS uint8_t  s_rx[MBAP_MAX_FRAME];
static CCMRAM_BSS uint8_t  s_tx[MBAP_MAX_FRAME];
static CCMRAM_BSS uint16_t s_regs[MB_RAW_MAX_READ_REGS];

static sModbusRawTxn   s_txn;
static osSemaphoreId_t s_done;

/* Set while the module BORROWS s_txn/s_regs and cleared by the completion
 * callback.  It exists for one case: if the completion ever failed to arrive
 * inside our backstop, reusing the buffer would let the engine write into
 * memory we had started refilling.  Answering 0x06 until it clears is the
 * safe read of "we do not know whose bytes those are yet". */
static volatile uint8_t s_txnBorrowed;

static osThreadId_t s_task;
static int8_t       s_monId = -1;

static struct netconn *s_listener;
static volatile uint8_t  s_listening;
static volatile uint8_t  s_connected;
static uint8_t           s_bindIp[4];
static uint32_t          s_connections;
static uint32_t          s_requests;
static uint32_t          s_exceptions;
static uint32_t          s_busyExceptions;
static uint32_t          s_lastRequestTick;
static uint8_t           s_hadRequest;

/* --------------------------------------------------------------------------
 * Byte stream over a connection — hides netbuf/part boundaries
 * -------------------------------------------------------------------------- */

typedef struct {
    struct netconn *conn;
    struct netbuf  *nb;
    void           *data;
    u16_t           len;
    u16_t           off;
} sStream;

static void st_init(sStream *s, struct netconn *conn)
{
    memset(s, 0, sizeof(*s));
    s->conn = conn;
}

static void st_cleanup(sStream *s)
{
    if (s->nb != NULL) {
        netbuf_delete(s->nb);
        s->nb = NULL;
    }
}

static err_t st_fill(sStream *s)
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

/** @return 0 on success, -1 when the connection ended or timed out. */
static int st_read(sStream *s, uint8_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (st_fill(s) != ERR_OK) {
            return -1;
        }
        out[i] = ((uint8_t *)s->data)[s->off++];
    }
    return 0;
}

static int send_all(struct netconn *conn, const void *data, uint32_t len)
{
    const uint8_t *p = data;

    while (len > 0u) {
        size_t written = 0u;
        err_t  err = netconn_write_partly(conn, p, len, NETCONN_COPY, &written);

        if (err != ERR_OK || written == 0u) {
            return -1;
        }
        p   += written;
        len -= (uint32_t)written;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * eModbusErr -> the exception byte a gateway owes its client
 *
 * THE SLAVE'S OWN CODES COME BACK UNCHANGED.  Everything else collapses into
 * the two gateway codes plus busy, because from the client's side that is
 * genuinely all the information there is: either the target did not answer
 * (0x0B) or we could not get to the wire (0x06 / 0x0A).
 * -------------------------------------------------------------------------- */

static uint8_t exc_from_err(int16_t e)
{
    switch (e) {
    case mbErr_excIllegalFunction: return 0x01u;
    case mbErr_excIllegalAddress:  return 0x02u;
    case mbErr_excIllegalValue:    return 0x03u;
    case mbErr_excDeviceFailure:   return 0x04u;
    case mbErr_excOther:           return 0x04u;

    /* We never reached the wire, or gave up before it answered.  "Come back"
     * is the honest reply and the only one that costs the far end nothing. */
    case mbErr_full:
    case mbErr_busy:
    case mbErr_notAttempted:
    case mbErr_timedOut:           return MBEXC_SLAVE_BUSY;

    /* The device record went away underneath us (a config swap), which from
     * the client's side is the same fact as an unknown unit id. */
    case mbErr_idNotFound:
    case mbErr_config:             return MBEXC_GW_PATH;

    default:                       return MBEXC_GW_TARGET;
    }
}

/* --------------------------------------------------------------------------
 * One transfer, synchronously, on this task
 * -------------------------------------------------------------------------- */

static void raw_done(sModbusRawTxn *t, void *ctx)
{
    (void)t;
    (void)ctx;

    /* Runs on the modbus task under the non-blocking rule: release and go. */
    s_txnBorrowed = 0u;
    (void)osSemaphoreRelease(s_done);
}

/**
 * @brief  Carry one PDU to the wire and wait for it.
 * @return 0 with @p txn filled, or a non-zero exception byte to answer with.
 */
static uint8_t do_transfer(uint8_t devOrd, uint8_t fc, uint16_t addr,
                           uint16_t count)
{
    s_txn.regs   = s_regs;
    s_txn.addr   = addr;
    s_txn.count  = count;
    s_txn.fc     = fc;
    s_txn.devOrd = devOrd;
    s_txn.result = mbErr_pending;

    /* Drain any STALE token before submitting.  There is exactly one way to
     * get one: a previous acquire timed out, we answered busy, and the
     * completion arrived afterwards and released anyway.  Without this drain
     * the next acquire would return immediately on that token and read
     * s_txn.result before this transfer had happened. */
    while (osSemaphoreAcquire(s_done, 0u) == osOK) {
        /* nothing */
    }

    s_txnBorrowed = 1u;
    int r = Modbus_RawTransfer(&s_txn, MBTCP_TXN_BUDGET_MS, raw_done, NULL);
    if (r != 0) {
        s_txnBorrowed = 0u;
        return exc_from_err((int16_t)r);
    }

    /* The module's contract is that the callback always fires within the
     * budget.  The extra second is a backstop against that contract being
     * broken, not a second deadline: if it ever expires, s_txnBorrowed stays
     * set and every later request answers busy rather than handing the engine
     * a buffer we have started reusing. */
    if (osSemaphoreAcquire(s_done, MBTCP_TXN_BUDGET_MS + 1000u) != osOK) {
        TRice("MBTCP: transfer completion did not arrive — buffer held\n");
        return MBEXC_SLAVE_BUSY;
    }

    return (s_txn.result == mbErr_ok) ? 0u : exc_from_err(s_txn.result);
}

/* --------------------------------------------------------------------------
 * One request
 * -------------------------------------------------------------------------- */

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/** Build an exception response over the request's own MBAP header.
 *  @return total frame length. */
static uint32_t build_exception(const uint8_t *req, uint8_t code)
{
    s_tx[0] = req[0];                 /* transaction id, echoed */
    s_tx[1] = req[1];
    put16(&s_tx[2], 0u);              /* protocol id */
    put16(&s_tx[4], 3u);              /* unit + fc + code */
    s_tx[6] = req[6];                 /* unit id, echoed */
    s_tx[7] = (uint8_t)(req[7] | 0x80u);
    s_tx[8] = code;

    s_exceptions++;
    if (code == MBEXC_SLAVE_BUSY) {
        s_busyExceptions++;
    }
    return 9u;
}

/**
 * @brief  Answer one framed request.
 * @param  req     the whole frame: MBAP header then PDU
 * @param  pduLen  PDU bytes (function code included)
 * @return response length in s_tx, or 0 if the request is to be ignored.
 */
static uint32_t handle_request(const uint8_t *req, uint16_t pduLen)
{
    const uint8_t *pdu  = &req[MBAP_HDR_LEN];
    uint8_t        unit = req[6];
    uint8_t        fc   = pdu[0];
    uint8_t        devOrd;
    uint16_t       addr;
    uint16_t       count;
    uint8_t        exc;

    s_requests++;
    s_lastRequestTick = osKernelGetTickCount();
    s_hadRequest      = 1u;

    if (fc != 3u && fc != 4u && fc != 6u && fc != 16u) {
        return build_exception(req, MBEXC_ILLEGAL_FUNCTION);
    }
    if (pduLen < 5u) {
        return build_exception(req, MBEXC_ILLEGAL_ADDRESS);
    }

    /* THE UNIT ID IS A SLAVE ADDRESS, resolved against the device records —
     * the config still owns which pair that slave lives on and at what baud
     * (modbus.h, Modbus_DeviceBySlave). */
    if (Modbus_DeviceBySlave(unit, &devOrd) != 0) {
        return build_exception(req, MBEXC_GW_PATH);
    }

    /* CHECKED HERE, NOT INSIDE do_transfer, because the write paths below fill
     * s_regs before they call it — and s_regs is the buffer the engine may
     * still be holding.  Refusing early is what keeps "borrowed until the
     * callback fires" true rather than nearly true. */
    if (s_txnBorrowed) {
        return build_exception(req, MBEXC_SLAVE_BUSY);
    }

    addr = be16(&pdu[1]);

    switch (fc) {
    case 3u:
    case 4u:
        count = be16(&pdu[3]);
        if (count == 0u || count > MB_RAW_MAX_READ_REGS) {
            return build_exception(req, MBEXC_ILLEGAL_VALUE);
        }
        exc = do_transfer(devOrd, fc, addr, count);
        if (exc != 0u) {
            return build_exception(req, exc);
        }
        put16(&s_tx[0], be16(&req[0]));
        put16(&s_tx[2], 0u);
        put16(&s_tx[4], (uint16_t)(3u + count * 2u));   /* unit + fc + bc + data */
        s_tx[6] = unit;
        s_tx[7] = fc;
        s_tx[8] = (uint8_t)(count * 2u);
        for (uint16_t i = 0; i < count; i++) {
            put16(&s_tx[9 + i * 2u], s_regs[i]);
        }
        return 9u + (uint32_t)count * 2u;

    case 6u:
        s_regs[0] = be16(&pdu[3]);
        exc = do_transfer(devOrd, 6u, addr, 1u);
        if (exc != 0u) {
            return build_exception(req, exc);
        }
        /* FC06's response is its request, echoed. */
        memcpy(s_tx, req, MBAP_HDR_LEN + 5u);
        put16(&s_tx[4], 6u);
        return MBAP_HDR_LEN + 5u;

    default:      /* 16 */
        count = be16(&pdu[3]);
        /* The byte count is the request's own statement about itself; if it
         * disagrees with the register count the frame is malformed, and that
         * is a different fact from an address the slave does not have. */
        if (pduLen < 6u || pdu[5] != (uint8_t)(count * 2u) ||
            pduLen < (uint16_t)(6u + count * 2u)) {
            return build_exception(req, MBEXC_ILLEGAL_ADDRESS);
        }
        if (count == 0u || count > MB_RAW_MAX_WRITE_REGS) {
            return build_exception(req, MBEXC_ILLEGAL_VALUE);
        }
        for (uint16_t i = 0; i < count; i++) {
            s_regs[i] = be16(&pdu[6 + i * 2u]);
        }
        /* ONE FRAME, NEVER DECOMPOSED (docs/design_solis_modbus_link.md §3.5):
         * the Remote Dispatch block is silently dropped by the inverter if it
         * arrives as scattered single-register writes. */
        exc = do_transfer(devOrd, 16u, addr, count);
        if (exc != 0u) {
            return build_exception(req, exc);
        }
        put16(&s_tx[0], be16(&req[0]));
        put16(&s_tx[2], 0u);
        put16(&s_tx[4], 6u);
        s_tx[6] = unit;
        s_tx[7] = 16u;
        put16(&s_tx[8], addr);
        put16(&s_tx[10], count);
        return 12u;
    }
}

/* --------------------------------------------------------------------------
 * One connection
 * -------------------------------------------------------------------------- */

static void serve(struct netconn *conn)
{
    sStream st;

    st_init(&st, conn);

    for (;;) {
        uint16_t length;
        uint16_t pduLen;

        if (st_read(&st, s_rx, MBAP_HDR_LEN) != 0) {
            break;                       /* closed, or idle past IO_TIMEOUT */
        }

        /* Protocol id must be zero; anything else is not Modbus TCP and the
         * stream cannot be resynchronised, so the connection ends. */
        if (be16(&s_rx[2]) != 0u) {
            TRice("MBTCP: bad protocol id, closing\n");
            break;
        }

        length = be16(&s_rx[4]);
        if (length < 2u || length > (MBAP_MAX_PDU + 1u)) {
            TRice("MBTCP: bad MBAP length %u, closing\n", length);
            break;
        }
        pduLen = (uint16_t)(length - 1u);        /* unit id is inside length */

        if (st_read(&st, &s_rx[MBAP_HDR_LEN], pduLen) != 0) {
            break;
        }

        {
            uint32_t n = handle_request(s_rx, pduLen);
            if (n > 0u && send_all(conn, s_tx, n) != 0) {
                break;
            }
        }

        SysMon_TaskCheckin(s_monId);
    }

    st_cleanup(&st);
}

/* --------------------------------------------------------------------------
 * The listener
 * -------------------------------------------------------------------------- */

/** The tunnel address, or 0 if this board has none.  RULE 1 lives here: no
 *  tunnel, no listener, and never a fallback to IP_ADDR_ANY. */
static int tunnel_addr(ip_addr_t *out, uint8_t ip[4])
{
    const sWgLinkCfg *cfg = WgLink_ActiveCfg();

    if (cfg == NULL) {
        return -1;
    }
    if ((cfg->tunnelIp[0] | cfg->tunnelIp[1] |
         cfg->tunnelIp[2] | cfg->tunnelIp[3]) == 0u) {
        return -1;
    }
    memcpy(ip, cfg->tunnelIp, 4);
    IP4_ADDR(out, cfg->tunnelIp[0], cfg->tunnelIp[1],
             cfg->tunnelIp[2], cfg->tunnelIp[3]);
    return 0;
}

static void listener_close(void)
{
    if (s_listener != NULL) {
        netconn_close(s_listener);
        netconn_delete(s_listener);
        s_listener = NULL;
    }
    s_listening = 0u;
}

/** @return 0 when bound and listening. */
static int listener_open(void)
{
    ip_addr_t addr;

    if (tunnel_addr(&addr, s_bindIp) != 0) {
        return -1;
    }

    s_listener = netconn_new(NETCONN_TCP);
    if (s_listener == NULL) {
        return -1;
    }
    if (netconn_bind(s_listener, &addr, MBTCP_PORT) != ERR_OK ||
        netconn_listen(s_listener) != ERR_OK) {
        netconn_delete(s_listener);
        s_listener = NULL;
        return -1;
    }
    netconn_set_recvtimeout(s_listener, ACCEPT_TIMEOUT_MS);
    s_listening = 1u;

    TRice("MBTCP: listening on %u.%u.%u.%u:%u (tunnel only)\n",
          s_bindIp[0], s_bindIp[1], s_bindIp[2], s_bindIp[3], MBTCP_PORT);
    return 0;
}

/** Has the tunnel address moved under us?  POST /api/wg/config can change it
 *  at runtime, and a listener bound to the old one would answer nothing while
 *  looking perfectly healthy. */
static int bind_still_valid(void)
{
    ip_addr_t unused;
    uint8_t   now[4];

    if (tunnel_addr(&unused, now) != 0) {
        return 0;
    }
    return memcmp(now, s_bindIp, 4) == 0;
}

static void mbtcp_task(void *arg)
{
    (void)arg;

    /* Accept blocks for ACCEPT_TIMEOUT_MS at a time, so the loop always comes
     * round — but a connection that is open and idle blocks in recv for
     * IO_TIMEOUT_MS, which is longer.  Hence a deadline generous enough to
     * cover the idle case rather than one that would call a healthy, quiet
     * gateway stale. */
    s_monId = SysMon_TaskRegister(MBTCP_TASK_STACK_WORDS,
                                  IO_TIMEOUT_MS + ACCEPT_TIMEOUT_MS * 2u);

    for (;;) {
        struct netconn *conn;

        if (!s_listening) {
            if (listener_open() != 0) {
                /* Unprovisioned, or lwIP not ready.  Both are ordinary
                 * states on a board that has not been given a tunnel yet. */
                SysMon_TaskCheckin(s_monId);
                osDelay(UNPROVISIONED_RETRY_MS);
                continue;
            }
        }

        if (netconn_accept(s_listener, &conn) != ERR_OK) {
            SysMon_TaskCheckin(s_monId);
            if (!bind_still_valid()) {
                TRice("MBTCP: tunnel address changed, rebinding\n");
                listener_close();
            }
            continue;
        }

        netconn_set_recvtimeout(conn, IO_TIMEOUT_MS);
        netconn_set_sendtimeout(conn, IO_TIMEOUT_MS);

        s_connections++;
        s_connected = 1u;
        serve(conn);
        s_connected = 0u;

        netconn_close(conn);
        netconn_delete(conn);
        SysMon_TaskCheckin(s_monId);
    }
}

/* --------------------------------------------------------------------------
 * Public
 * -------------------------------------------------------------------------- */

static osThreadAttr_t s_attr = {
    .name       = "mbtcp",
    .stack_size = MBTCP_TASK_STACK_WORDS * 4U,
    /* BELOW the modbus engine (24), like func: a gateway request arriving
     * from the WAN must never delay the sequence that keeps the battery and
     * the inverter talking.  That is the autonomy rule expressed as a
     * priority rather than as a comment. */
    .priority   = (osPriority_t)(osPriorityNormal - 1),
};

int MbTcp_Init(void)
{
    if (s_task != NULL) {
        return 0;
    }

    s_done = osSemaphoreNew(1u, 0u, NULL);
    if (s_done == NULL) {
        return -1;
    }

    s_task = osThreadNew(mbtcp_task, NULL, &s_attr);
    return (s_task != NULL) ? 0 : -1;
}

int MbTcp_GetStats(sMbTcpStats *out)
{
    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    out->listening      = s_listening;
    out->connected      = s_connected;
    out->connections    = s_connections;
    out->requests       = s_requests;
    out->exceptions     = s_exceptions;
    out->busyExceptions = s_busyExceptions;
    memcpy(out->bindIp, s_bindIp, 4);
    out->lastRequestAge_ms =
        s_hadRequest ? (osKernelGetTickCount() - s_lastRequestTick)
                     : MBTCP_AGE_NEVER;
    return 0;
}

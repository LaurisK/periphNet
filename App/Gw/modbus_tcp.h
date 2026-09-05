/**
 * @file    modbus_tcp.h
 * @brief   Modbus TCP gateway — the board as a slave on :502.
 *
 * WHAT THIS IS FOR.  Home Assistant reaches the Solis inverter through the
 * `solis_modbus` integration, which speaks Modbus TCP and owns a 4482-line
 * curated register map that PeriphNet will never reproduce.  So the board
 * stops trying to be the thing that understands the inverter and becomes the
 * thing that carries frames to it: a Modbus-Ethernet bridge of the kind
 * `solis_modbus` already supports, that additionally carries its own tunnel.
 * The whole finding, the two shapes considered and why this one won:
 * docs/design_solis_modbus_link.md §6.4.
 *
 * WHAT IT IS NOT.  It is not a consumer of readings — it never subscribes,
 * never sees an mbEvt_sample, and knows nothing about points, plans, decode
 * types or scaling.  It resolves a unit id to a device record once and then
 * carries PDUs.  Everything about *meaning* lives at the far end; everything
 * about *the line* stays in the Modbus config.
 *
 * ---------------------------------------------------------------------------
 * THE THREE RULES IT IS BUILT ON
 * ---------------------------------------------------------------------------
 *
 * 1. IT BINDS TO THE TUNNEL ADDRESS, NEVER IP_ADDR_ANY.  This is the whole
 *    authorization model and it is load-bearing, not tidiness: §7.2 was
 *    decided as *transparent both ways*, so anything that reaches this port
 *    can write any holding register on a live inverter with a live battery
 *    behind it.  The WireGuard tunnel is the only thing standing there.  A
 *    board with no tunnel configured SERVES NOTHING and says so — it does not
 *    fall back to the site LAN it happens to have landed on.
 *
 * 2. IT NEVER LETS THE FAR END REACH pymodbus's 5 s TIMEOUT.  `solis_modbus`
 *    holds ONE asyncio lock per link across every slave and every write, so a
 *    single slow answer head-of-line-blocks the whole integration.  This
 *    server answers inside its own short budget or returns exception 0x06
 *    (Slave Device Busy) immediately.  Upstream logs a 0x06 at debug level and
 *    reads the group again next cycle — no sensor disabled, no bisection, no
 *    retry storm, and because an exception PDU is a valid response pymodbus
 *    does not spend a retry on it.
 *
 * 3. IT IS A QUEUED CLIENT OF THE ENGINE, NEVER A SECOND BUS MASTER.  Every
 *    request becomes one Modbus_RawTransfer on the shared FIFO, so it takes
 *    its turn behind scheduled sequences and cannot displace the 1 Hz
 *    Pylontech CAN obligation.  Autonomy is what makes that a rule: the board
 *    must be correct with the WAN and HA both down, so nothing arriving from
 *    the WAN may be able to starve what keeps the battery and inverter
 *    talking (design_remote_access_and_autonomy.md §1).
 *
 * ---------------------------------------------------------------------------
 * EXCEPTION CODES IT ORIGINATES, and why each is the one a gateway owes
 * ---------------------------------------------------------------------------
 *
 *   0x01  the function code is not one of 3, 4, 6, 16
 *   0x02  malformed PDU — the request's own byte count disagrees with itself
 *   0x03  a register count outside what Modbus permits for that function
 *   0x06  busy: the engine FIFO was full, or our budget expired before the
 *         wire.  This is back-pressure, and it is the one code that means
 *         "ask again", so it must never be spent on anything else
 *   0x0A  gateway path unavailable: no device record carries that unit id, or
 *         there is no valid config at all.  The client's map is wrong, not
 *         the slave's
 *   0x0B  gateway target device failed to respond: the slave was addressed
 *         and said nothing, or answered rubbish (timeout, CRC, line error)
 *
 * A SLAVE'S OWN EXCEPTION IS PASSED THROUGH VERBATIM and never rewritten into
 * one of the above.  That is not politeness: `solis_modbus` treats 2 and 3 as
 * *recoverable*, bisects the failing block, isolates the bad register and
 * splits the group — which is how it adapts a generic map to one particular
 * inverter.  Answering "I don't serve that address" with silence turns a
 * one-time adaptation into a permanent retry.
 */

#ifndef MODBUS_TCP_H_
#define MODBUS_TCP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The IANA port for Modbus TCP.  Not configurable: the far end's config
 *  flow defaults to it and a non-standard port buys nothing here. */
#define MBTCP_PORT              502u

/** Stack, in words.  Generous against the frame buffers being STATIC rather
 *  than stack locals — the depth here is lwIP's netconn calls, which mostly
 *  post to tcpip_thread, plus one 259-byte frame's worth of parsing.  It comes
 *  out of the 48 KB .ccmheap FreeRTOS heap, so sysmon's high-water mark is the
 *  number to check on the first hardware run. */
#define MBTCP_TASK_STACK_WORDS  768u

/** How long one request may take before the answer becomes exception 0x06.
 *  Well inside pymodbus's 5 s (rule 2) and above the RS485 response timeout,
 *  so a slave that is merely slow still answers rather than being reported
 *  busy. */
#define MBTCP_TXN_BUDGET_MS    1500u

/**
 * @brief  Start the gateway task.
 *
 *         Idempotent.  Safe to call before the tunnel exists: the task waits
 *         for a configured tunnel address and binds when it appears, so a
 *         board provisioned over HTTP starts serving without a reboot.
 *
 * @return 0 if the task is running, -1 if it could not be created.
 */
int MbTcp_Init(void);

/** Live state, for GET /api/modbus/gw and the CLI. */
typedef struct {
    uint8_t  listening;        /* bound and accepting                       */
    uint8_t  bindIp[4];        /* the tunnel address it is bound to         */
    uint8_t  connected;        /* a client is attached right now            */
    uint32_t connections;      /* accepted since boot                       */
    uint32_t requests;         /* PDUs answered                             */
    uint32_t exceptions;       /* answers that were an exception PDU        */
    uint32_t busyExceptions;   /* ...of those, 0x06 — the back-pressure one */
    uint32_t lastRequestAge_ms;/* since the last PDU, MBTCP_AGE_NEVER = none */
} sMbTcpStats;

#define MBTCP_AGE_NEVER   0xFFFFFFFFu

/** @return 0, or -1 if @p out is NULL. */
int MbTcp_GetStats(sMbTcpStats *out);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_TCP_H_ */

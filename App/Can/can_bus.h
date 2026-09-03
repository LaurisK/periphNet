/*
 * can_bus.h
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * THE CAN PERIPHERAL OWNER.  Both bxCAN cells, one bit-timing calculator, one
 * software TX queue per bus, and THE single RX dispatcher — the one described
 * in docs/design_battery_pack.md §16 item 5 as a prerequisite for
 * `pack_pylontech`.
 *
 * WHY A DISPATCHER AND NOT A CALLBACK PER CONSUMER.  HAL gives exactly one
 * weak RX-FIFO-pending callback for BOTH cells; a second definition
 * is a link error, and "whoever wins the link" is not a design.  So this file
 * defines it once and fans out to subscribers matched by (bus, id, mask).
 * `bms_reader`, the bridge and any future pack type are all ordinary
 * subscribers with no claim on the peripheral.
 *
 * EVERY SUBSCRIBER CALLBACK RUNS IN THE RX ISR at NVIC priority 5.  It must be
 * short, must not block, and must not call anything that blocks.  A subscriber
 * that wants task context posts an event and returns — that is what
 * App/Func exists for.
 *
 * THE ACCEPTANCE FILTERS ARE WIDE OPEN, deliberately.  A bridge that filters
 * is not a bridge: 11-bit, 29-bit, data and remote frames all pass, and
 * whether a frame is interesting is a decision each subscriber makes for
 * itself.  Frames are counted before anyone judges them, which is what makes
 * the monitor able to say "the inverter is talking, nobody listens".
 */

#ifndef CAN_BUS_H_
#define CAN_BUS_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes -----------------------------------------------------------------*/

#include <stdint.h>

/* Exported types -----------------------------------------------------------*/

/** How many subscriptions the dispatcher holds, across both buses. */
#define CAN_SUBSCRIBERS_MAX     8u

/** Software TX queue depth, per bus.  Three hardware mailboxes cover a burst
 *  of three; a bridge forwarding a six-frame Pylontech set that arrived
 *  back-to-back needs more, and the queue is what keeps the RX ISR from
 *  either blocking or dropping. */
#define CAN_TX_QUEUE_DEPTH      16u

typedef enum {
    canBus_1 = 0,           /* CAN1 — PD0/PD1                                */
    canBus_2,               /* CAN2 — PB5/PB6                                */
    canBus_last
} eCanBus;

/** Which way a frame crossed this board.  A bridged frame is BOTH: received
 *  on one bus, transmitted on the other, and counted on each. */
typedef enum {
    canDir_rx = 0,
    canDir_tx,
    canDir_last
} eCanDir;

/**
 * One CAN frame, as it exists on the wire plus the moment we saw it.
 *
 * `id` is the raw identifier, 11 or 29 bit as `ext` says.  A bridge must not
 * normalise either of those: an inverter that answers a remote frame, or a
 * BMS that speaks extended ids, has to cross unchanged or the board is not
 * transparent.
 */
typedef struct {
    uint32_t id;            /* 11- or 29-bit identifier, as on the wire      */
    uint32_t stamp_ms;      /* HAL_GetTick() when received (RX only)         */
    uint8_t  data[8];
    uint8_t  dlc;           /* 0..8                                          */
    uint8_t  bus;           /* eCanBus it arrived on / must go out on        */
    uint8_t  ext;           /* 1 = 29-bit identifier                         */
    uint8_t  rtr;           /* 1 = remote frame; `data` is meaningless       */
} sCanFrame;

/**
 * @brief  A subscriber's frame handler.  ALWAYS ISR CONTEXT.
 * @param  frame - the received frame, valid only for the call
 * @param  ctx - whatever was handed to CanBus_Subscribe
 */
typedef void (*fCanRxHandler)(const sCanFrame *frame, void *ctx);

/**
 * @brief  The tap: one hook that sees EVERY frame, both directions.
 *
 * Separate from a subscription because a monitor is not a consumer — it takes
 * no decision and must not be able to alter one.  TX frames are tapped when
 * they are ACCEPTED for transmission, not when the wire confirms them; the
 * difference between `txAccepted` and `txDone` in sCanBusStats is exactly what
 * an unterminated or unpowered bus looks like.
 */
typedef void (*fCanTap)(const sCanFrame *frame, eCanDir dir, void *ctx);

/** Everything the peripheral will admit to.  Counters are free-running and
 *  reset only by CanBus_ResetStats. */
typedef struct {
    uint32_t rxCnt;             /* frames pulled out of the RX FIFO          */
    uint32_t txAcceptedCnt;     /* frames accepted into a mailbox or queue   */
    uint32_t txDoneCnt;         /* frames the peripheral says went out       */
    uint32_t txDroppedCnt;      /* refused: software queue full              */
    uint32_t rxOverrunCnt;      /* RX FIFO0 overrun — frames LOST in silicon */
    uint32_t errorCnt;          /* error interrupts                          */
    uint32_t busOffCnt;         /* transitions into bus-off                  */
    uint32_t lastError;         /* HAL error word at the last error IRQ      */
    uint32_t bitrate_bps;       /* 0 when the bus is not running             */
    uint8_t  txQueueDepth;      /* frames waiting in software right now      */
    uint8_t  txQueuePeak;       /* deepest the queue has ever been           */
    uint8_t  running;
    uint8_t  busOff;            /* 1 = in bus-off right now                  */
    uint8_t  rxErrorCnt;        /* bxCAN receive error counter (ESR)         */
    uint8_t  txErrorCnt;        /* bxCAN transmit error counter (ESR)        */
} sCanBusStats;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Bring one bus up at a bitrate, filters wide open, interrupts armed.
 *
 * Re-initialises the peripheral CubeMX left at its default timing.  Safe to
 * call on a running bus: it is stopped and rebuilt, which is how a bitrate
 * change happens.
 *
 * @param  bus - which cell
 * @param  bitrate_bps - 10000..1000000; must divide PCLK1/14 exactly
 * @retval 0 on success, negative on a bad argument or a HAL failure
 * @note   Task context.  Never blocks for longer than the HAL start call.
 */
int CanBus_Start(eCanBus bus, uint32_t bitrate_bps);

/**
 * @brief  Stop a bus and release the wire.  Subscriptions survive.
 * @param  bus - which cell
 * @retval 0 on success, negative on a bad argument
 */
int CanBus_Stop(eCanBus bus);

/** @retval 1 when the bus is started. */
int CanBus_IsRunning(eCanBus bus);

/**
 * @brief  Transmit one frame on `frame->bus`.
 *
 * LEGAL FROM ANY CONTEXT — this is what the bridge calls from the other bus's
 * RX ISR.  Never blocks: a free mailbox takes the frame immediately, otherwise
 * it joins the software queue, and a full queue is counted as a drop and
 * refused.  Silence is never the answer to congestion.
 *
 * @param  frame - the frame; `stamp_ms` is ignored
 * @retval 0 on success, negative when the bus is down or the queue is full
 */
int CanBus_Send(const sCanFrame *frame);

/**
 * @brief  Ask to be called for every matching frame on one bus.
 *
 * Matching is `(rxId & mask) == (id & mask)`, so mask 0 means "everything".
 *
 * @param  bus - which cell
 * @param  id - identifier to match after masking
 * @param  mask - 0 for all frames
 * @param  cb - ISR-context handler, must not block
 * @param  ctx - passed back unchanged
 * @retval a non-negative handle, or negative when the table is full
 */
int CanBus_Subscribe(eCanBus bus, uint32_t id, uint32_t mask,
                     fCanRxHandler cb, void *ctx);

/**
 * @brief  Release a subscription.
 * @param  handle - what CanBus_Subscribe returned
 * @retval 0 on success, negative on a bad handle
 */
int CanBus_Unsubscribe(int handle);

/**
 * @brief  Install the single tap (see fCanTap).  NULL removes it.
 * @param  tap - the hook, or NULL
 * @param  ctx - passed back unchanged
 */
void CanBus_SetTap(fCanTap tap, void *ctx);

/**
 * @brief  Copy one bus's counters.
 * @param  bus - which cell
 * @param  out - destination
 * @retval 0 on success, negative on a bad argument
 */
int CanBus_GetStats(eCanBus bus, sCanBusStats *out);

/** @brief  Zero the counters of both buses.  Does not touch the wire. */
void CanBus_ResetStats(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_BUS_H_ */

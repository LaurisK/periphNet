/*
 * can_monitor.h
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * WHAT PASSED OVER THE BRIDGE.  Two views of the same traffic:
 *
 *   - a REGISTER, one row per (bus, identifier), always on: counts, first and
 *     last sighting, the observed period band, the last payload, and how often
 *     that payload actually changed.  This is the view that answers "is the
 *     BMS talking, at what rate, and is anything in the frame moving" without
 *     anyone watching at the time.
 *   - a TRACE ring, off by default: the last N frames in order with
 *     timestamps.  Costs an ISR-context copy per frame, so it is armed
 *     explicitly and never left on.
 *
 * IT DECIDES NOTHING.  The monitor is installed as the bus TAP, not as a
 * subscriber, precisely so it cannot influence forwarding: recording happens
 * before any consumer sees the frame, and a frame the bridge refuses to
 * forward is still recorded on the bus it arrived on.
 *
 * READS ARE A SNAPSHOT, NOT A LOCK ON THE BUS.  A row is copied under a short
 * critical section, so one row is internally consistent; the set of rows is
 * not one instant.  For traffic statistics that is the right trade.
 */

#ifndef CAN_MONITOR_H_
#define CAN_MONITOR_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Includes -----------------------------------------------------------------*/

#include <stdint.h>

#include "App/Can/can_bus.h"

/* Exported types -----------------------------------------------------------*/

/** Rows per bus.  A Pylontech set is six identifiers and an inverter adds one
 *  or two, so 24 leaves room for a surprise and still fits the .bss budget —
 *  MAIN SRAM, not CCM, is what this module is charged against, and it was
 *  already 93 % full before it existed.  A row that does not fit is counted in
 *  `idOverflowCnt` rather than evicting one that does. */
#define CANMON_IDS_MAX          24u

/** Frames held by the trace ring, both buses together.  64 frames is four
 *  seconds of a 1 Hz Pylontech set on both sides, which is the window an
 *  operator actually reads. */
#define CANMON_TRACE_MAX        64u

/** One identifier as seen on one bus. */
typedef struct {
    uint32_t id;
    uint32_t rxCnt;             /* frames received on this bus               */
    uint32_t txCnt;             /* frames this board sent on this bus        */
    uint32_t changeCnt;         /* payload differed from the previous one    */
    uint32_t firstStamp_ms;
    uint32_t lastStamp_ms;
    uint32_t minGap_ms;         /* observed period band; 0 until the second  */
    uint32_t maxGap_ms;         /*   sighting                                */
    uint8_t  data[8];           /* the most recent payload                   */
    uint8_t  dlc;
    uint8_t  ext;               /* 1 = 29-bit identifier                     */
    uint8_t  rtr;               /* 1 = the last frame was a remote frame     */
    uint8_t  bus;
} sCanMonId;

/** One frame in the trace ring. */
typedef struct {
    uint32_t stamp_ms;
    uint32_t id;
    uint8_t  data[8];
    uint8_t  dlc;
    uint8_t  bus;
    uint8_t  dir;               /* eCanDir                                   */
    uint8_t  ext;
} sCanMonTrace;

/**
 * @brief  The register's downstream consumer.  ALWAYS ISR CONTEXT.
 *
 * The flash log needs to know whether a payload CHANGED, and the register has
 * just worked that out to maintain `changeCnt` — computing it twice, from two
 * copies of the last payload, would be both wasteful and a second thing to
 * get wrong.  So the sink is handed that bit, plus the row index, which is a
 * stable small integer the consumer can use to key its own per-identifier
 * state without matching identifiers again.
 *
 * @param  frame - the frame, valid only for the call
 * @param  dir - which way it went
 * @param  rowIdx - its row in this bus's register, stable for the row's life
 * @param  changed - 1 when the payload differed from the previous one
 * @param  ctx - whatever was handed to CanMon_SetSink
 */
typedef void (*fCanMonSink)(const sCanFrame *frame, eCanDir dir,
                            uint8_t rowIdx, uint8_t changed, void *ctx);

/** Whole-monitor counters. */
typedef struct {
    uint32_t recordedCnt;       /* frames offered to the monitor             */
    uint32_t idOverflowCnt;     /* frames whose identifier found no free row */
    uint32_t traceDroppedCnt;   /* trace entries overwritten before a read   */
    uint8_t  tracing;
} sCanMonStats;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Install the monitor as the CAN bus tap.
 * @note   Task context.  Idempotent.
 */
void CanMon_Attach(void);

/** @brief  Remove the tap.  Recorded rows are kept. */
void CanMon_Detach(void);

/** @brief  Forget every row, the trace and the counters. */
void CanMon_Reset(void);

/**
 * @brief  How many identifiers this bus has ever carried.
 * @param  bus - which cell
 * @retval the row count, 0..CANMON_IDS_MAX
 */
uint8_t CanMon_IdCount(eCanBus bus);

/**
 * @brief  Copy ONE row, in first-seen order.
 *
 * One row at a time rather than a bulk copy on purpose: every caller (the CLI
 * and the HTTP handler) formats as it goes, and a 24-row array on either of
 * their stacks — or in their .bss — is memory this board does not have.
 *
 * @param  bus - which cell
 * @param  idx - 0..CanMon_IdCount()-1
 * @param  out - destination
 * @retval 0 on success, negative when there is no such row
 */
int CanMon_GetIdAt(eCanBus bus, uint8_t idx, sCanMonId *out);

/**
 * @brief  Arm or disarm the trace ring.  Arming clears it.
 * @param  on - non-zero to record
 */
void CanMon_TraceEnable(int on);

/**
 * @brief  How many frames the trace ring currently holds.
 * @retval 0..CANMON_TRACE_MAX
 */
uint16_t CanMon_TraceCount(void);

/**
 * @brief  Copy ONE traced frame, oldest first.
 * @param  idx - 0..CanMon_TraceCount()-1, 0 being the oldest still held
 * @param  out - destination
 * @retval 0 on success, negative when there is no such frame
 */
int CanMon_GetTraceAt(uint16_t idx, sCanMonTrace *out);

/**
 * @brief  Copy the monitor's own counters.
 * @param  out - destination
 */
void CanMon_GetStats(sCanMonStats *out);

/**
 * @brief  Install the downstream sink (see fCanMonSink).  NULL removes it.
 * @param  sink - the hook, or NULL
 * @param  ctx - passed back unchanged
 */
void CanMon_SetSink(fCanMonSink sink, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAN_MONITOR_H_ */

/*
 * can_monitor.c
 *
 *  Created on: 2026-09-03
 *      Author: Lauris
 *
 * See can_monitor.h.  Everything here runs in the RX ISR of one of the two
 * cells, so the whole file is written to a budget: a linear scan of at most
 * CANMON_IDS_MAX rows, eight byte compares, and no allocation.  At 500 kbit a
 * frame occupies the wire for ~230 us, which is two orders of magnitude more
 * than the scan costs.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Can/can_monitor.h"

#include "cmsis_os.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* Private types ------------------------------------------------------------*/

typedef struct {
    sCanMonId row[CANMON_IDS_MAX];
    uint8_t   count;
} sCanMonBus;

/* Private variables --------------------------------------------------------*/

/* .bss, not CCM: ~7 KB, cold, and CCM is the constrained region. */
static sCanMonBus   s_bus[canBus_last];
static sCanMonTrace s_trace[CANMON_TRACE_MAX];
static uint16_t     s_traceHead;
static uint16_t     s_traceCount;
static sCanMonStats s_stats;
static uint8_t      s_attached;
static fCanMonSink  s_sink;
static void        *s_sinkCtx;

/* Private function prototypes ----------------------------------------------*/

static uint32_t   EnterCritical(void);
static void       ExitCritical(uint32_t saved);
static sCanMonId *RowFor(sCanMonBus *bus, const sCanFrame *frame);
static void       RecordTrace(const sCanFrame *frame, eCanDir dir);
static void       OnFrame(const sCanFrame *frame, eCanDir dir, void *ctx);

/* Private functions --------------------------------------------------------*/

static uint32_t EnterCritical(void)
{
    if (0u != __get_IPSR()) {
        return taskENTER_CRITICAL_FROM_ISR();
    }
    taskENTER_CRITICAL();
    return 0u;
}

static void ExitCritical(uint32_t saved)
{
    if (0u != __get_IPSR()) {
        taskEXIT_CRITICAL_FROM_ISR(saved);
    } else {
        (void)saved;
        taskEXIT_CRITICAL();
    }
}

/**
 * @brief  The row for this identifier, creating it if there is space.
 * @retval NULL when the table is full — the frame is counted as an overflow
 *         and otherwise ignored.  Evicting a row would lose exactly the
 *         long-lived identifier the operator cares about in favour of noise.
 */
static sCanMonId *RowFor(sCanMonBus *bus, const sCanFrame *frame)
{
    sCanMonId *row;

    for (uint8_t i = 0u; i < bus->count; i++) {
        if ((bus->row[i].id == frame->id) &&
            (bus->row[i].ext == frame->ext)) {
            return &bus->row[i];
        }
    }
    if (bus->count >= CANMON_IDS_MAX) {
        return NULL;
    }

    row = &bus->row[bus->count];
    bus->count++;
    memset(row, 0, sizeof(*row));
    row->id            = frame->id;
    row->ext           = frame->ext;
    row->bus           = frame->bus;
    row->firstStamp_ms = frame->stamp_ms;
    return row;
}

static void RecordTrace(const sCanFrame *frame, eCanDir dir)
{
    sCanMonTrace *slot = &s_trace[s_traceHead];

    slot->stamp_ms = frame->stamp_ms;
    slot->id       = frame->id;
    slot->dlc      = frame->dlc;
    slot->bus      = frame->bus;
    slot->dir      = (uint8_t)dir;
    slot->ext      = frame->ext;
    memcpy(slot->data, frame->data, 8);

    s_traceHead = (uint16_t)((s_traceHead + 1u) % CANMON_TRACE_MAX);
    if (s_traceCount < CANMON_TRACE_MAX) {
        s_traceCount++;
    } else {
        s_stats.traceDroppedCnt++;
    }
}

/** The tap.  ISR context, both buses, both directions. */
static void OnFrame(const sCanFrame *frame, eCanDir dir, void *ctx)
{
    sCanMonBus *bus;
    sCanMonId  *row;
    fCanMonSink sink;
    uint32_t    saved;
    uint32_t    stamp_ms;
    uint8_t     rowIdx  = 0u;
    uint8_t     changed = 0u;

    (void)ctx;
    if ((frame == NULL) || (frame->bus >= (uint8_t)canBus_last)) {
        return;
    }
    bus = &s_bus[frame->bus];

    /* A transmitted frame carries no reception stamp — it is being sent now. */
    stamp_ms = (dir == canDir_rx) ? frame->stamp_ms : HAL_GetTick();

    saved = EnterCritical();

    s_stats.recordedCnt++;
    row = RowFor(bus, frame);
    if (row == NULL) {
        s_stats.idOverflowCnt++;
        ExitCritical(saved);
        return;
    }
    rowIdx = (uint8_t)(row - bus->row);

    if (row->rxCnt + row->txCnt > 0u) {
        uint32_t gap_ms = stamp_ms - row->lastStamp_ms;

        if ((row->minGap_ms == 0u) || (gap_ms < row->minGap_ms)) {
            row->minGap_ms = gap_ms;
        }
        if (gap_ms > row->maxGap_ms) {
            row->maxGap_ms = gap_ms;
        }
    }
    if (dir == canDir_rx) {
        row->rxCnt++;
    } else {
        row->txCnt++;
    }
    if ((row->dlc != frame->dlc) ||
        (memcmp(row->data, frame->data, 8) != 0)) {
        row->changeCnt++;
        memcpy(row->data, frame->data, 8);
        row->dlc = frame->dlc;
        changed  = 1u;
    }
    row->rtr          = frame->rtr;
    row->lastStamp_ms = stamp_ms;

    if (s_stats.tracing != 0u) {
        sCanFrame stamped = *frame;

        stamped.stamp_ms = stamp_ms;
        RecordTrace(&stamped, dir);
    }

    sink = s_sink;
    ExitCritical(saved);

    /* OUTSIDE the lock: the sink stages into its own ring under its own, and
     * nesting two critical sections in an ISR to no purpose is how a short
     * path becomes a long one. */
    if (sink != NULL) {
        sink(frame, dir, rowIdx, changed, s_sinkCtx);
    }
}

/* Exported functions -------------------------------------------------------*/

void CanMon_Attach(void)
{
    if (s_attached != 0u) {
        return;
    }
    s_attached = 1u;
    CanBus_SetTap(OnFrame, NULL);
}

void CanMon_Detach(void)
{
    if (s_attached == 0u) {
        return;
    }
    s_attached = 0u;
    CanBus_SetTap(NULL, NULL);
}

void CanMon_Reset(void)
{
    uint32_t saved = EnterCritical();
    uint8_t  tracing = s_stats.tracing;

    memset(s_bus, 0, sizeof(s_bus));
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.tracing = tracing;
    s_traceHead     = 0u;
    s_traceCount    = 0u;
    ExitCritical(saved);
}

uint8_t CanMon_IdCount(eCanBus bus)
{
    if ((uint32_t)bus >= (uint32_t)canBus_last) {
        return 0u;
    }
    return s_bus[bus].count;
}

int CanMon_GetIdAt(eCanBus bus, uint8_t idx, sCanMonId *out)
{
    uint32_t saved;
    int      res = 0;

    if ((out == NULL) || ((uint32_t)bus >= (uint32_t)canBus_last)) {
        return -1;
    }

    saved = EnterCritical();
    if (idx >= s_bus[bus].count) {
        res = -2;
    } else {
        *out = s_bus[bus].row[idx];
    }
    ExitCritical(saved);
    return res;
}

void CanMon_TraceEnable(int on)
{
    uint32_t saved = EnterCritical();

    s_stats.tracing         = (on != 0) ? 1u : 0u;
    s_stats.traceDroppedCnt = 0u;
    s_traceHead             = 0u;
    s_traceCount            = 0u;
    ExitCritical(saved);
}

uint16_t CanMon_TraceCount(void)
{
    return s_traceCount;
}

int CanMon_GetTraceAt(uint16_t idx, sCanMonTrace *out)
{
    uint32_t saved;
    uint16_t slot;
    int      res = 0;

    if (out == NULL) {
        return -1;
    }

    saved = EnterCritical();
    if (idx >= s_traceCount) {
        res = -2;
    } else {
        /* Walk forward from the oldest still held, which is `count` slots
         * behind the head. */
        slot = (uint16_t)((s_traceHead + CANMON_TRACE_MAX - s_traceCount + idx)
                          % CANMON_TRACE_MAX);
        *out = s_trace[slot];
    }
    ExitCritical(saved);
    return res;
}

void CanMon_SetSink(fCanMonSink sink, void *ctx)
{
    uint32_t saved = EnterCritical();

    s_sinkCtx = ctx;
    s_sink    = sink;
    ExitCritical(saved);
}

void CanMon_GetStats(sCanMonStats *out)
{
    uint32_t saved;

    if (out == NULL) {
        return;
    }
    saved = EnterCritical();
    *out  = s_stats;
    ExitCritical(saved);
}

/**
 * @file    modbus_engine.c
 * @brief   The engine — see modbus_engine.h.
 *
 * Replaces the v1 walker's lap (docs/modbus.md §10 step 10).  What went with
 * it: the 100 ms tick, the traversal, `s_lastPollTick`, and the notion that
 * the engine has a lifecycle anyone outside it steers.
 *
 * THE SHAPE:
 *
 *   FreeRTOS timer  -> due bit + poke -----.
 *   port completion -> semaphore ----------|--> modbus task drains
 *   mutating API    -> flag + poke --------'
 *
 * Timers free-run: nothing rearms on completion, so a period is a period and
 * drift has nowhere to accumulate.  Overrun coalesces — a stacked event is
 * dropped and COUNTED, which is the instrument that says a config is asking
 * for more than the wire can deliver.
 */

#include "App/Modbus/modbus_engine.h"
#include "App/Modbus/modbus.h"
#include "App/Modbus/modbus_internal.h"
#include "App/Modbus/modbus_port.h"
#include "App/Mon/sysmon.h"
#include "App/system.h"

#include "modbus_config_store.h"
#include "modbus_blocks.h"
#include "modbus_decode.h"

#include "FreeRTOS.h"
#include "timers.h"
#include "cmsis_os.h"
#include "trice.h"

#include <stdio.h>
#include <string.h>

/* THE CLOCK IS NOT THE IDENTITY.
 *
 * A sequence is named {device, plan, time table} — the plan is part of it
 * because two plans may cover one device and number their tables
 * independently — and that is what `missed` counts, what a plan edit tears
 * down, and what one run of the wire covers.  None of that needs a clock of
 * its own.  What needs a clock is a PERIOD, and periods repeat: the JK config
 * that exposed this had 28 sequences ticking at four distinct rates.
 *
 * So sequences live in `s_seq[]` and hold no FreeRTOS object; the distinct
 * periods live in `s_ticks[]`, reference-counted, one software timer each.
 * A tick fires once and marks every sequence riding it (docs/modbus.md §5.2).
 *
 * This is not an optimisation.  The old shape armed one timer per sequence
 * and rebuilt the whole set on every subscription or config change, which
 * flooded the 10-deep FreeRTOS timer command queue from a task the timer
 * service cannot preempt: the deletes consumed the queue, every subsequent
 * osTimerStart failed unchecked, and the engine was left holding slots that
 * reported themselves scheduled with nothing armed behind them — a silent,
 * permanent stall (docs/issue_modbus_engine_stall.md §5).  Two properties
 * keep it from coming back: there are far fewer clocks than sequences, and
 * `timers_rebuild()` is a DIFF, so the steady state issues no timer commands
 * at all.
 *
 * The config bounds admit 8 devices x 8 plans x 8 tables of sequences, which
 * the module caps rather than failing an allocation later; distinct periods
 * are bounded by 8 plans x 8 tables, and capped well below that because a
 * config wanting a dozen different rates on one wire is a config problem.
 * Scheduling state is the module's to judge (§1.2). */
#define MB_MAX_TIMERS        32u   /* sequences: {device, plan, time table} */
#define MB_MAX_TICKS         12u   /* distinct periods, one os timer each   */
#define ENGINE_QUEUE_DEPTH   16u
#define ENGINE_RESP_TIMEOUT  1000u

/* How long a timer command may wait for the service queue.  It is allowed to
 * block: this runs on the modbus task, off the wire path, and a bounded wait
 * is the whole difference between "the queue was busy" and a scheduler that
 * silently stops.  CMSIS-RTOS2 cannot express a block time — osTimerStart is
 * xTimerChangePeriod(..., 0) — so the timer calls here go to FreeRTOS
 * directly.  That is the one place in this file that does. */
#define TICK_CMD_WAIT_MS     100u

/* One sequence: what a run of the wire covers.  No FreeRTOS object. */
typedef struct {
    uint8_t          used;
    uint8_t          devOrd;
    uint8_t          planId;
    uint8_t          ttId;
    uint8_t          tickIdx;      /* which clock marks it due */
    uint8_t          keep;         /* rebuild scratch: survives this diff */
    volatile uint8_t due;
    uint32_t         missed;
    uint8_t          missedLogged;
} sSeqSlot;

/* One clock.  Shared by every sequence at this period, and destroyed only
 * when the last of them goes. */
typedef struct {
    TimerHandle_t timer;
    uint32_t      period_sec;
    uint8_t       refs;
    uint8_t       suspect;   /* seen inactive once; see ticks_verify() */
} sTick;

static sSeqSlot          s_seq[MB_MAX_TIMERS];
static sTick             s_ticks[MB_MAX_TICKS];
static osThreadId_t      s_task;
static osMessageQueueId_t s_queue;
static volatile int      s_running;
static volatile int      s_resched;

static uint32_t s_pollCount;
static uint32_t s_errorCount;
static uint32_t s_missedTotal;
static uint32_t s_droppedPokes;
static uint32_t s_armFailures;    /* a clock that would not arm — §6.2 tier 1 */

/* Derivation working set, one sequence at a time.  Scratch, not state. */
#define CCMRAM_BSS __attribute__((section(".ccmram")))

CCMRAM_BSS static uint16_t         s_ttIds[MB_MAX_TT_ENTRIES_PER_TABLE];
CCMRAM_BSS static sModbusPointSpan s_spans[MB_MAX_TT_ENTRIES_PER_TABLE];
CCMRAM_BSS static sModbusReadBlock s_blocks[MB_MAX_READ_BLOCKS_PER_DEV];
CCMRAM_BSS static uint16_t         s_regBuf[MB_MAX_REGS_PER_READ];

/* ==========================================================================
 * Waking the task
 * ========================================================================== */

void ModbusEngine_Poke(void)
{
    uint8_t msg = 0;

    if (s_queue == NULL) {
        return;
    }
    /* Never block: a self-post from the task draining the queue is legal
     * (contract 5), so a blocking post would deadlock.  A full queue means a
     * wake is already pending, which is all a poke conveys. */
    if (osMessageQueuePut(s_queue, &msg, 0, 0) != osOK) {
        s_droppedPokes++;
    }
}

void ModbusEngine_Resched(void)
{
    s_resched = 1;
    ModbusEngine_Poke();
}

/* Runs in the timer service task, so like the ISR case it ONLY posts.
 *
 * One clock, many sequences: the tick marks each one riding it and posts a
 * SINGLE wake for the lot.  Under the old one-timer-per-sequence shape a
 * period coincidence posted one poke per sequence — 28 of them into a 16-deep
 * queue, most of which were dropped and counted as if they meant something. */
static void tick_cb(TimerHandle_t timer)
{
    const sTick *tk  = (const sTick *)pvTimerGetTimerID(timer);
    uint8_t      idx = (uint8_t)(tk - s_ticks);
    int          any = 0;

    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        sSeqSlot *q = &s_seq[i];

        if (!q->used || q->tickIdx != idx) {
            continue;
        }
        if (q->due) {
            /* Stacking is dropped and counted: the previous event for this
             * identity is still unserviced.  That device's other time table
             * is a different identity and is unaffected. */
            q->missed++;
            s_missedTotal++;
            continue;
        }
        q->due = 1u;
        any    = 1;
    }
    if (any) {
        ModbusEngine_Poke();
    }
}

/* ==========================================================================
 * Clocks — reference-counted, one per distinct period
 * ========================================================================== */

/* Every timer command in this module goes through here.  It blocks for a
 * bounded time rather than failing silently, and it reports.  The old code
 * discarded both return values, which is how 28 unarmed timers came to look
 * exactly like 28 armed ones. */
static int tick_arm(sTick *tk)
{
    TickType_t period = pdMS_TO_TICKS(tk->period_sec * 1000u);

    if (period == 0u) {
        period = 1u;               /* a period below one tick is one tick */
    }
    if (xTimerChangePeriod(tk->timer, period,
                           pdMS_TO_TICKS(TICK_CMD_WAIT_MS)) != pdPASS) {
        return -1;                 /* the CALLER counts it, exactly once */
    }
    return 0;
}

/* Acquire the clock for `period_sec`, creating it if this is its first user.
 * Returns the tick index, or -1. */
static int tick_acquire(uint32_t period_sec)
{
    int free = -1;

    for (uint8_t i = 0; i < MB_MAX_TICKS; i++) {
        if (s_ticks[i].refs > 0u && s_ticks[i].period_sec == period_sec) {
            s_ticks[i].refs++;
            return (int)i;         /* shared: NO timer command at all */
        }
        if (s_ticks[i].refs == 0u && free < 0) {
            free = (int)i;
        }
    }
    if (free < 0) {
        return -1;
    }

    sTick *tk = &s_ticks[free];

    /* The handle is kept across acquire/release cycles: creating a FreeRTOS
     * object on a repeating path is the habit that caused the fault this
     * design replaces, so a slot mints its timer once and reuses it. */
    if (tk->timer == NULL) {
        tk->timer = xTimerCreate("mbtick", 1, pdTRUE, tk, tick_cb);
        if (tk->timer == NULL) {
            return -1;
        }
    }
    tk->period_sec = period_sec;
    tk->refs       = 1u;
    if (tick_arm(tk) != 0) {
        s_armFailures++;
        tk->refs = 0u;
        return -1;
    }
    return free;
}

/* Drop one user of a clock; the last one out stops it. */
static void tick_release(uint8_t idx)
{
    sTick *tk;

    if (idx >= MB_MAX_TICKS || s_ticks[idx].refs == 0u) {
        return;
    }
    tk = &s_ticks[idx];
    if (--tk->refs > 0u) {
        return;
    }
    /* Stopped, not deleted — see tick_acquire.  A stop that cannot be queued
     * would leave a clock ticking for sequences that no longer exist, so it
     * waits like every other command here. */
    if (xTimerStop(tk->timer, pdMS_TO_TICKS(TICK_CMD_WAIT_MS)) != pdPASS) {
        /* A clock left running for sequences that no longer exist wakes the
         * task for nothing; harmless, but it is still a command that did not
         * land, so it is counted like the rest. */
        s_armFailures++;
    }
}

/* When the arm commands issued by a rebuild may be checked, and how many
 * consecutive sightings make an unarmed clock a fact rather than a guess.
 *
 * A queued command is not an armed timer.  xTimerChangePeriod only POSTS; the
 * timer service task marks the timer active, and at priority 2 it is the
 * second-lowest task in the system, so it runs only once this task and
 * everything above it are idle.  During start-up that can be a long time:
 * measured on board 2026-09-03, `Pd1.1.41` counted exactly four failures on
 * every boot — one per clock — with a 50 ms delay and two sightings, and
 * every one of them was a clock that armed correctly moments later.
 *
 * So the window is deliberately generous.  Three sightings a second apart
 * means a clock is only reported after THREE SECONDS of still not being
 * armed, by which point the command has had every chance and a starvation
 * that long is itself worth knowing about.  Nothing is lost by waiting: the
 * failure being detected is permanent until reboot, against periods of 5 to
 * 600 seconds. */
#define TICK_VERIFY_DELAY_MS 1000u
#define TICK_VERIFY_SIGHTINGS 3u

static TickType_t s_verifyAt;      /* 0 = nothing to verify */

static void ticks_verify_later(void)
{
    s_verifyAt = xTaskGetTickCount() + pdMS_TO_TICKS(TICK_VERIFY_DELAY_MS);
    if (s_verifyAt == 0u) {
        s_verifyAt = 1u;           /* 0 is the "idle" value */
    }
}

/* Every clock with users must actually be running.  This is the invariant the
 * old code never checked, and checking it is what turns a silent permanent
 * stall into a log line (docs/issue_modbus_engine_stall.md §6.2).
 *
 * A SINGLE SIGHTING OF AN INACTIVE CLOCK MEANS NOTHING, and saying so is what
 * makes the counter worth reading — a monitor that cries wolf on every boot is
 * worse than no monitor.  See TICK_VERIFY_DELAY_MS for why, and for the
 * measurement that set the window. */
static void ticks_verify(void)
{
    int recheck = 0;

    for (uint8_t i = 0; i < MB_MAX_TICKS; i++) {
        sTick *tk = &s_ticks[i];

        if (tk->refs == 0u || tk->timer == NULL) {
            continue;
        }
        if (xTimerIsTimerActive(tk->timer) != pdFALSE) {
            tk->suspect = 0u;
            continue;
        }
        if (++tk->suspect < TICK_VERIFY_SIGHTINGS) {
            recheck = 1;           /* inconclusive: look again, count nothing */
            continue;
        }
        tk->suspect = 0u;          /* the re-arm below starts a fresh count */
        s_armFailures++;           /* counted on CONFIRMATION, once */
        recheck = 1;
        if (tick_arm(tk) == 0) {
            TRice("Modbus: tick %us was not armed, re-armed\n",
                  (unsigned)tk->period_sec);
        } else {
            TRice("Modbus: tick %us WILL NOT ARM, sequences are unscheduled\n",
                  (unsigned)tk->period_sec);
        }
    }
    /* Come back either way: a suspicion has to be settled, a re-arm has to be
     * confirmed, and a clock that would not take the command at all must not
     * be quietly forgotten. */
    if (recheck) {
        ticks_verify_later();
    }
}

/* ==========================================================================
 * Sequences — created and destroyed by SUBSCRIPTION
 * ========================================================================== */

/* Find the live sequence with this identity, or -1. */
static int seq_find(uint8_t devOrd, uint8_t planId, uint8_t ttId)
{
    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        sSeqSlot *q = &s_seq[i];

        if (q->used && q->devOrd == devOrd && q->planId == planId &&
            q->ttId == ttId) {
            return (int)i;
        }
    }
    return -1;
}

/* Mark the sequence for {devOrd, planId, ttId} at `period_sec` as wanted.
 *
 * AN UNCHANGED SEQUENCE IS LEFT ALONE — slot, clock, `due` and `missed` all
 * survive, and no timer command is issued.  That is the whole point: applying
 * a config that renames a point, or a subscriber coming and going, must not
 * disturb a schedule it did not change. */
static int seq_want(uint8_t devOrd, uint8_t planId, uint8_t ttId,
                    uint32_t period_sec)
{
    int idx = seq_find(devOrd, planId, ttId);

    if (idx >= 0) {
        sSeqSlot *q = &s_seq[idx];

        if (s_ticks[q->tickIdx].period_sec == period_sec) {
            q->keep = 1u;          /* identical: nothing to do */
            return 0;
        }
        /* Same identity, new cadence: move it between clocks.  Its due bit
         * and its missed count belong to the identity, so they stay. */
        int tick = tick_acquire(period_sec);
        if (tick < 0) {
            return -1;
        }
        tick_release(q->tickIdx);
        q->tickIdx = (uint8_t)tick;
        q->keep    = 1u;
        return 0;
    }

    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        sSeqSlot *q = &s_seq[i];
        int       tick;

        if (q->used) {
            continue;
        }
        tick = tick_acquire(period_sec);
        if (tick < 0) {
            return -1;
        }
        memset(q, 0, sizeof(*q));
        q->devOrd  = devOrd;
        q->planId  = planId;
        q->ttId    = ttId;
        q->tickIdx = (uint8_t)tick;
        q->keep    = 1u;
        /* Clocks free-run: nothing rearms on completion, so a period is a
         * period.  Everything sharing a period is phase-locked to one clock
         * by construction, which is stronger than the old shape managed with
         * one timer each (§5.2). */
        q->due     = 1u;           /* first read immediately */
        __DMB();                   /* publish the slot before the flag */
        q->used    = 1u;
        return 0;
    }
    return -1;
}

/* Clear the marks before a walk.  It is done here rather than left to
 * `seq_sweep()` because the walk may return early on a bad record, and a
 * `keep` surviving into the NEXT walk would spare a sequence nobody asked
 * for. */
static void seq_unmark(void)
{
    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        s_seq[i].keep = 0u;
    }
}

/* Retire every sequence the walk did not ask for this round. */
static void seq_sweep(void)
{
    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        sSeqSlot *q = &s_seq[i];

        if (!q->used) {
            continue;
        }
        if (q->keep) {
            q->keep = 0u;
            continue;
        }
        /* Retire before releasing: tick_cb runs on the timer service task
         * and skips a slot that is not `used`, so clearing the flag first is
         * what makes the release and the memset unobservable to it. */
        q->used = 0u;
        __DMB();
        tick_release(q->tickIdx);
        memset(q, 0, sizeof(*q));
    }
}

/* Rebuild the sequence set from the config and the live subscriptions.  A plan
 * outside the union of live plan masks creates NO sequences, and a device
 * covered by no live plan is not polled at all (§4.3).
 *
 * THIS IS A DIFF, NOT A REBUILD.  Nothing is torn down before the walk,
 * because a walk that fails partway must not leave the board unscheduled —
 * that failure mode is precisely how the engine came to sit silent with every
 * status field reading healthy.  Sequences are marked as they are asked for
 * and swept afterwards, so an early return leaves the previous schedule
 * running. */
static void timers_rebuild(void)
{
    eNvDbUser         region = MbCfgStore_ActiveRegion();
    sMbCfgCursor      c;
    sModbusPlanRecord plan;
    uint8_t           liveMask = ModbusSub_PlanUnion();
    int               overflow = 0;

    seq_unmark();

    if (liveMask == 0u || MbCfg_SeekPlans(region, &c) != 0) {
        seq_sweep();               /* nothing subscribed: the wire stays quiet */
        return;
    }

    while (MbCfg_NextPlan(&c, &plan) == 1) {
        sModbusTimeTableRecord tt;
        uint8_t                ttId = 0;
        int                    covered = (plan.planId < MB_MAX_PLANS) &&
                                         ((liveMask &
                                           (uint8_t)(1u << plan.planId)) != 0u);
        int rt;

        while ((rt = MbCfg_NextTimeTable(&c, &tt)) == 1) {
            if (MbCfg_ReadPointIds(&c, NULL, tt.entryCount) != 0) {
                return;            /* schedule in force is left untouched */
            }
            if (!covered) {
                ttId++;
                continue;
            }
            for (uint8_t d = 0; d < MB_MAX_DEVICES; d++) {
                if ((plan.devices & (uint8_t)(1u << d)) == 0u) {
                    continue;
                }
                if (seq_want(d, plan.planId, ttId, tt.period_sec) != 0) {
                    overflow = 1;
                }
            }
            ttId++;
        }
        if (rt != 0) {
            return;
        }
    }

    seq_sweep();
    ticks_verify_later();

    if (overflow) {
        /* Named and left alone: the remedy is config, which is where the
         * judgement belongs (§1.2). */
        TRice("Modbus: too many sequences or periods, some are unscheduled\n");
    }
}

/* ==========================================================================
 * One sequence: the derived read blocks of one device for one time table of
 * one plan, run back to back on that device's port
 * ========================================================================== */

static int id_selected(uint16_t id, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        if (s_ttIds[i] == id) {
            return 1;
        }
    }
    return 0;
}

/* Find the plan's time table `ttId` and load its point ids. */
static int load_time_table(eNvDbUser region, uint8_t planId, uint8_t ttId,
                           sModbusPlanRecord *planOut, uint16_t *idCount,
                           uint32_t *period_sec)
{
    sMbCfgCursor      c;
    sModbusPlanRecord plan;

    if (MbCfg_SeekPlans(region, &c) != 0) {
        return -1;
    }
    while (MbCfg_NextPlan(&c, &plan) == 1) {
        sModbusTimeTableRecord tt;
        uint8_t                id = 0;
        int                    rt;

        while ((rt = MbCfg_NextTimeTable(&c, &tt)) == 1) {
            uint16_t take = (tt.entryCount > MB_MAX_TT_ENTRIES_PER_TABLE)
                                ? MB_MAX_TT_ENTRIES_PER_TABLE : tt.entryCount;

            if (plan.planId == planId && id == ttId) {
                if (MbCfg_ReadPointIds(&c, s_ttIds, take) != 0) {
                    return -1;
                }
                *planOut    = plan;
                *idCount    = take;
                *period_sec = tt.period_sec;
                return 0;
            }
            if (MbCfg_ReadPointIds(&c, NULL, tt.entryCount) != 0) {
                return -1;
            }
            id++;
        }
        if (rt != 0) {
            return -1;
        }
    }
    return -1;
}

static void emit_point(const char *devPrefix, uint8_t devOrd,
                       const sModbusPointRecord *pt, uint16_t ptOrd,
                       uint32_t period_sec, const uint16_t *regs)
{
    sModbusPointDesc desc;
    char             text[52];
    int32_t          scaled = 0;
    const char      *textPtr = NULL;

    if (pt->decodeType == mbDecode_ascii) {
        if (MbDecode_Ascii(pt, regs, text, sizeof(text)) < 0) {
            return;
        }
        textPtr = text;
    } else {
        scaled = MbDecode_Scaled(pt, regs);
    }

    ModbusDesc_Build(&desc, devPrefix, devOrd, ptOrd, period_sec, pt);
    ModbusDispatch_Sample(&desc, scaled, textPtr);
}

static void service_block(eNvDbUser region, const sModbusDeviceRecord *dev,
                          uint8_t devOrd, const sModbusCapabilityRecord *cap,
                          const sModbusReadBlock *blk, uint16_t idCount,
                          uint32_t period_sec, uint8_t planId, uint8_t ttId)
{
    sModbusPortParams params;
    uint32_t          t0 = HAL_GetTick();
    int16_t           err;

    if (blk->regs > MB_MAX_REGS_PER_READ) {
        return;
    }

    /* Line parameters travel with the frame, so one wire serves a Solis at
     * 9600 and a JK at 115200 (§3.4). */
    params.baud               = MbRecords_BaudFromCode(dev->baudCode);
    params.format             = dev->format;
    params.responseTimeout_ms = ENGINE_RESP_TIMEOUT;

    err = ModbusPort_Read(dev->portId, &params, dev->slaveAddr, blk->fc,
                          blk->addr, blk->regs, s_regBuf);

    ModbusDispatch_Txn(devOrd, dev->slaveAddr, blk->addr, blk->regs,
                       (uint16_t)(HAL_GetTick() - t0), err, planId, ttId);

    if (err != mbErr_ok) {
        s_errorCount++;
        return;
    }
    s_pollCount++;

    /* Point records are RE-READ FROM FLASH to decode a reply, because the
     * config is never RAM-resident and that property must not leak away. */
    sMbCfgCursor            pc;
    sModbusCapabilityRecord tmp;
    sModbusPointRecord      pt;
    uint16_t                ptOrd = 0;

    if (MbCfg_OpenCapability(region, dev->capId, &pc, &tmp, NULL, 0) != 0) {
        return;
    }
    while (MbCfg_NextPoint(&pc, &pt) == 1) {
        uint8_t width = MbRecords_RegWidth(pt.decodeType, pt.length);
        int     idx;

        if (!id_selected(ptOrd, idCount) || pt.functionCode != blk->fc) {
            ptOrd++;
            continue;
        }
        idx = MbBlocks_RegIndex(cap, blk, pt.addr);
        if (idx >= 0 && (uint32_t)idx + width <= blk->regs) {
            emit_point(dev->topicPrefix, devOrd, &pt, ptOrd, period_sec,
                       &s_regBuf[idx]);
        }
        ptOrd++;
    }
}

static void run_sequence(sSeqSlot *t)
{
    eNvDbUser               region = MbCfgStore_ActiveRegion();
    sModbusPlanRecord       plan;
    sModbusDeviceRecord     dev;
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];
    sMbCfgCursor            c;
    uint16_t                idCount = 0, spanCount = 0, ptOrd = 0;
    uint32_t                period_sec = 0;
    sModbusPointRecord      pt;
    int                     blockCount;

    if (load_time_table(region, t->planId, t->ttId, &plan, &idCount,
                        &period_sec) != 0) {
        return;
    }
    if (MbCfg_FindDevice(region, t->devOrd, &dev) != 0) {
        return;
    }
    /* A port with no driver registered is disabled, structurally (§5.1). */
    if (!ModbusPort_IsRegistered(dev.portId)) {
        return;
    }
    if (MbCfg_OpenCapability(region, plan.capId, &c, &cap, blocks,
                             MB_MAX_BLOCKS_PER_CAP) != 0) {
        return;
    }

    /* Read blocks are DERIVED, never authored and never stored (§3.2). */
    while (MbCfg_NextPoint(&c, &pt) == 1) {
        if (id_selected(ptOrd, idCount) &&
            spanCount < MB_MAX_TT_ENTRIES_PER_TABLE) {
            s_spans[spanCount].addr  = pt.addr;
            s_spans[spanCount].ptOrd = ptOrd;
            s_spans[spanCount].regs  = MbRecords_RegWidth(pt.decodeType,
                                                          pt.length);
            s_spans[spanCount].fc    = pt.functionCode;
            spanCount++;
        }
        ptOrd++;
    }
    if (spanCount == 0u) {
        return;
    }

    blockCount = MbBlocks_Derive(&cap, blocks, cap.blockCount, s_spans,
                                 spanCount, s_blocks,
                                 MB_MAX_READ_BLOCKS_PER_DEV);
    if (blockCount <= 0) {
        return;
    }

    /* Every block goes to one device, so a sequence costs ONE line
     * reconfiguration rather than one per block — and a pending request has an
     * obvious injection point, between blocks (§5.2). */
    for (int b = 0; b < blockCount; b++) {
        ModbusReq_Service();
        service_block(region, &dev, t->devOrd, &cap, &s_blocks[b], idCount,
                      period_sec, t->planId, t->ttId);
    }
}

/* ==========================================================================
 * The task
 * ========================================================================== */

static void service_due(void)
{
    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        sSeqSlot *t = &s_seq[i];

        if (!t->used || !t->due) {
            continue;
        }
        t->due = 0u;
        run_sequence(t);

        if (t->missed > 0u && !t->missedLogged) {
            /* Fires on FIRST occurrence per timer, not every time — the
             * running count belongs in `modbus status`, not in the log. */
            t->missedLogged = 1u;
            /* Fires on FIRST occurrence per timer; the running count belongs
             * in `modbus status`, not in the log.  It prints the period for a
             * human, but the counter keys on {deviceId, planId, timeTableId}
             * (§8.4). */
            char id[40];
            snprintf(id, sizeof(id), "dev%u/plan%u/tt%u %us (n=%lu)",
                     t->devOrd, t->planId, t->ttId,
                     (unsigned)s_ticks[t->tickIdx].period_sec,
                     (unsigned long)t->missed);
            TRiceS("Modbus: missed %s\n", id);
        }
    }
}

static void engine_task(void *arg)
{
    (void)arg;

    TRice("Modbus: started\n");

    /* The 200 ms floor below is what makes a deadline meaningful here: the
     * task must come round even with nothing subscribed.  A sequence that
     * hangs on a port is the failure this catches. */
    int8_t monId = SysMon_TaskRegister(640U, 3000U);

    for (;;) {
        uint8_t msg;

        /* The wake is the queue; the 200 ms floor only keeps a lost poke from
         * stalling pending API work forever. */
        (void)osMessageQueueGet(s_queue, &msg, NULL, pdMS_TO_TICKS(200));
        SysMon_TaskCheckin(monId);

        /* A config swap is committed here, on the task that walks the config,
         * so a reader can never race one. */
        if (MbCfgStore_IsSwapPending() &&
            !MbCfgStore_RegionValid(MbCfgStore_InactiveRegion())) {
            /* Armed with nothing to swap to: unreachable by design (arming
             * checks the region), so getting here means the region was
             * invalidated afterwards.  Left alone the flag is never consumed
             * AND refuses every compile, which locks the config plane out for
             * good — so discard it rather than wait for a commit that cannot
             * come.  Recovers a board already wedged in flash. */
            if (MbCfgStore_ClearSwapPending() == 0) {
                TRice("Modbus: stale config swap discarded\n");
            }
        } else if (MbCfgStore_IsSwapPending()) {
            if (MbCfgStore_CommitSwap() == 0) {
                sModbusConfigCounts counts;

                ModbusReq_CompleteForSwap();
                ModbusPlans_Refresh();
                s_resched = 1;
                TRice("Modbus: config swapped, active region %u\n",
                      (unsigned)(MbCfgStore_ActiveRegion() ==
                                 nvdbUser_modbusLutB));
                if (MbCfg_Count(MbCfgStore_ActiveRegion(), &counts) != 0) {
                    memset(&counts, 0, sizeof(counts));
                }
                ModbusDispatch_Config(
                    (uint8_t)(MbCfgStore_ActiveRegion() ==
                              nvdbUser_modbusLutB), &counts);
                ModbusSub_CatalogueAll();
            }
        }

        ModbusSub_Service();
        ModbusPlans_Service();
        ModbusReq_Service();

        if (s_resched) {
            s_resched = 0;
            timers_rebuild();
        }

        /* Deferred, because a queued timer command is not an armed timer and
         * this task outranks the one that arms it (see TICK_VERIFY_DELAY_MS). */
        if (s_verifyAt != 0u &&
            (int32_t)(xTaskGetTickCount() - s_verifyAt) >= 0) {
            s_verifyAt = 0u;
            ticks_verify();
        }

        service_due();
    }
}

int ModbusEngine_Start(void)
{
    static const osThreadAttr_t attr = {
        .name       = "modbus",
        .stack_size = 640U * 4U,
        .priority   = (osPriority_t)osPriorityNormal,
    };

    if (s_running) {
        return 0;
    }

    s_queue = osMessageQueueNew(ENGINE_QUEUE_DEPTH, sizeof(uint8_t), NULL);
    if (s_queue == NULL) {
        return -1;
    }
    s_task = osThreadNew(engine_task, NULL, &attr);
    if (s_task == NULL) {
        return -1;
    }
    s_running = 1;
    s_resched = 1;
    return 0;
}

int ModbusEngine_IsRunning(void)
{
    return s_running;
}

uint32_t ModbusEngine_ResponseTimeoutMs(void)
{
    return ENGINE_RESP_TIMEOUT;
}

void ModbusEngine_Counters(uint32_t *polls, uint32_t *errors, uint32_t *missed)
{
    if (polls)  *polls  = s_pollCount;
    if (errors) *errors = s_errorCount;
    if (missed) *missed = s_missedTotal;
}

void ModbusEngine_ScheduleStats(sModbusScheduleStats *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->droppedPokes = s_droppedPokes;
    out->armFailures  = s_armFailures;

    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        if (s_seq[i].used) {
            out->sequences++;
        }
    }
    for (uint8_t i = 0; i < MB_MAX_TICKS; i++) {
        if (s_ticks[i].refs == 0u) {
            continue;
        }
        out->ticksLive++;
        if (s_ticks[i].timer != NULL &&
            xTimerIsTimerActive(s_ticks[i].timer) != pdFALSE) {
            out->ticksArmed++;
        }
    }
}

void ModbusEngine_LogStatus(void)
{
    sModbusConfigCounts counts;
    int                 haveCounts =
        (MbCfg_Count(MbCfgStore_ActiveRegion(), &counts) == 0);
    uint8_t             seqs = 0;
    uint8_t             ticks = 0;
    uint8_t             armed = 0;
    char                buf[110];

    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        if (s_seq[i].used) {
            seqs++;
        }
    }
    for (uint8_t i = 0; i < MB_MAX_TICKS; i++) {
        if (s_ticks[i].refs == 0u) {
            continue;
        }
        ticks++;
        if (s_ticks[i].timer != NULL &&
            xTimerIsTimerActive(s_ticks[i].timer) != pdFALSE) {
            armed++;
        }
    }

    /* `dropped` counts wakes that did not fit the queue — the same capacity
     * signal `missed` is, one level down (§4.8).  `ticks` reports armed out of
     * live: anything but n/n means sequences are riding a clock that is not
     * running, which is the failure this scheduler exists to make visible. */
    snprintf(buf, sizeof(buf),
             "%s polls=%u errors=%u missed=%u dropped=%u seqs=%u ticks=%u/%u "
             "armfail=%u subs=%s",
             s_running ? "running" : "stopped", (unsigned)s_pollCount,
             (unsigned)s_errorCount, (unsigned)s_missedTotal,
             (unsigned)s_droppedPokes, seqs, armed, ticks,
             (unsigned)s_armFailures,
             ModbusSub_AnyLive() ? "yes" : "none");
    TRiceS("Modbus engine: %s\n", buf);

    for (uint8_t i = 0; i < MB_MAX_TICKS; i++) {
        if (s_ticks[i].refs == 0u) {
            continue;
        }
        TRice(" tick: %us x%u %s\n", (unsigned)s_ticks[i].period_sec,
              s_ticks[i].refs,
              (s_ticks[i].timer != NULL &&
               xTimerIsTimerActive(s_ticks[i].timer) != pdFALSE)
                  ? "armed" : "NOT ARMED");
    }

    if (!haveCounts) {
        /* Zero devices is a valid, first-class state (§4.2). */
        TRice("Modbus: unprovisioned (no valid config)\n");
        return;
    }
    TRice("Modbus config: region %u, %u caps %u devices %u plans %u points\n",
          (unsigned)(MbCfgStore_ActiveRegion() == nvdbUser_modbusLutB),
          counts.capabilities, counts.devices, counts.plans, counts.points);

    sMbCfgCursor        c;
    sModbusDeviceRecord dev;
    uint8_t             devOrd = 0;

    if (MbCfg_SeekDevices(MbCfgStore_ActiveRegion(), &c) == 0) {
        while (MbCfg_NextDevice(&c, &dev) == 1 && devOrd < MB_MAX_DEVICES) {
            uint8_t polled = 0;

            for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
                if (s_seq[i].used && s_seq[i].devOrd == devOrd) {
                    polled++;
                }
            }
            /* "Why is this device not polled" is a question about SUBSCRIBERS
             * and about the port, so the answer is both (§4.3, §4.2). */
            snprintf(buf, sizeof(buf), "%u %s slave=%u cap=%u %lu %s %s x%u",
                     devOrd, dev.topicPrefix, dev.slaveAddr, dev.capId,
                     (unsigned long)MbRecords_BaudFromCode(dev.baudCode),
                     Modbus_PortName(dev.portId),
                     ModbusPort_IsRegistered(dev.portId) ? "up" : "NO-DRIVER",
                     polled);
            TRiceS(" device: %s\n", buf);
            devOrd++;
        }
    }

    sMbPlanHeader hdr[MB_MAX_PLANS];
    if (MbCfg_ReadPlanHeaders(MbCfgStore_ActiveRegion(), hdr) > 0) {
        for (uint8_t i = 0; i < MB_MAX_PLANS; i++) {
            if (!hdr[i].used) {
                continue;
            }
            snprintf(buf, sizeof(buf), "%u %s cap=%u devices=0x%02x tables=%u",
                     i, hdr[i].rec.name, hdr[i].rec.capId, hdr[i].rec.devices,
                     hdr[i].tableCount);
            TRiceS(" plan: %s\n", buf);
        }
    }
}

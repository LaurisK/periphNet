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
#include "App/system.h"

#include "modbus_config_store.h"
#include "modbus_blocks.h"
#include "modbus_decode.h"

#include "cmsis_os.h"
#include "trice.h"

#include <stdio.h>
#include <string.h>

/* One timer per (device, plan, time table) of every live plan.  The config
 * bounds admit 8 devices x 8 plans x 8 tables, which would be 512 FreeRTOS
 * timers off the 48 KB CCM heap that task stacks also come from — so the
 * module caps it and says so, rather than failing an allocation later.
 * Scheduling state is the module's to judge (§1.2). */
#define MB_MAX_TIMERS        32u
#define ENGINE_QUEUE_DEPTH   16u
#define ENGINE_RESP_TIMEOUT  1000u

typedef struct {
    osTimerId_t      timer;
    uint8_t          used;
    uint8_t          devOrd;
    uint8_t          planId;
    uint8_t          ttId;
    uint32_t         period_sec;
    volatile uint8_t due;
    uint32_t         missed;
    uint8_t          missedLogged;
} sTimerSlot;

static sTimerSlot        s_timers[MB_MAX_TIMERS];
static osThreadId_t      s_task;
static osMessageQueueId_t s_queue;
static volatile int      s_running;
static volatile int      s_resched;

static uint32_t s_pollCount;
static uint32_t s_errorCount;
static uint32_t s_missedTotal;
static uint32_t s_droppedPokes;

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

/* Runs in the timer service task, so like the ISR case it ONLY posts. */
static void timer_cb(void *arg)
{
    sTimerSlot *t = (sTimerSlot *)arg;

    if (t->due) {
        /* Stacking is dropped and counted: the previous event for this
         * identity is still unserviced.  That device's other time table is a
         * different identity and is unaffected. */
        t->missed++;
        s_missedTotal++;
        return;
    }
    t->due = 1u;
    ModbusEngine_Poke();
}

/* ==========================================================================
 * Timers — created and destroyed by SUBSCRIPTION
 * ========================================================================== */

static void timers_destroy(void)
{
    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        if (s_timers[i].used) {
            osTimerDelete(s_timers[i].timer);
            memset(&s_timers[i], 0, sizeof(s_timers[i]));
        }
    }
}

static int timer_create(uint8_t devOrd, uint8_t planId, uint8_t ttId,
                        uint32_t period_sec)
{
    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        sTimerSlot *t = &s_timers[i];

        if (t->used) {
            continue;
        }
        t->devOrd     = devOrd;
        t->planId     = planId;
        t->ttId       = ttId;
        t->period_sec = period_sec;
        t->timer      = osTimerNew(timer_cb, osTimerPeriodic, t, NULL);
        if (t->timer == NULL) {
            return -1;
        }
        t->used = 1u;
        /* Free-running: nothing rearms it on completion, so a period is a
         * period.  A plan's timers all start when it goes live, so everything
         * it covers is due at once and stays phase-locked (§5.2). */
        osTimerStart(t->timer, pdMS_TO_TICKS(period_sec * 1000u));
        t->due = 1u;               /* first read immediately */
        return 0;
    }
    return -1;
}

/* Rebuild the timer set from the config and the live subscriptions.  A plan
 * outside the union of live plan masks creates NO timers, and a device covered
 * by no live plan is not polled at all (§4.3). */
static void timers_rebuild(void)
{
    uint32_t          base = MbCfgStore_ActiveBase();
    sMbCfgCursor      c;
    sModbusPlanRecord plan;
    uint8_t           liveMask = ModbusSub_PlanUnion();
    int               overflow = 0;

    timers_destroy();

    if (liveMask == 0u || MbCfg_SeekPlans(base, &c) != 0) {
        return;                    /* nothing subscribed: the wire stays quiet */
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
                return;
            }
            if (!covered) {
                ttId++;
                continue;
            }
            for (uint8_t d = 0; d < MB_MAX_DEVICES; d++) {
                if ((plan.devices & (uint8_t)(1u << d)) == 0u) {
                    continue;
                }
                if (timer_create(d, plan.planId, ttId, tt.period_sec) != 0) {
                    overflow = 1;
                }
            }
            ttId++;
        }
        if (rt != 0) {
            return;
        }
    }

    if (overflow) {
        /* Named and left alone: the remedy is config, which is where the
         * judgement belongs (§1.2). */
        TRice("Modbus: too many timers, some tables are not scheduled\n");
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
static int load_time_table(uint32_t base, uint8_t planId, uint8_t ttId,
                           sModbusPlanRecord *planOut, uint16_t *idCount,
                           uint32_t *period_sec)
{
    sMbCfgCursor      c;
    sModbusPlanRecord plan;

    if (MbCfg_SeekPlans(base, &c) != 0) {
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

static void service_block(uint32_t base, const sModbusDeviceRecord *dev,
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

    if (MbCfg_OpenCapability(base, dev->capId, &pc, &tmp, NULL, 0) != 0) {
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

static void run_sequence(sTimerSlot *t)
{
    uint32_t                base = MbCfgStore_ActiveBase();
    sModbusPlanRecord       plan;
    sModbusDeviceRecord     dev;
    sModbusCapabilityRecord cap;
    sModbusBlockRecord      blocks[MB_MAX_BLOCKS_PER_CAP];
    sMbCfgCursor            c;
    uint16_t                idCount = 0, spanCount = 0, ptOrd = 0;
    uint32_t                period_sec = 0;
    sModbusPointRecord      pt;
    int                     blockCount;

    if (load_time_table(base, t->planId, t->ttId, &plan, &idCount,
                        &period_sec) != 0) {
        return;
    }
    if (MbCfg_FindDevice(base, t->devOrd, &dev) != 0) {
        return;
    }
    /* A port with no driver registered is disabled, structurally (§5.1). */
    if (!ModbusPort_IsRegistered(dev.portId)) {
        return;
    }
    if (MbCfg_OpenCapability(base, plan.capId, &c, &cap, blocks,
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
        service_block(base, &dev, t->devOrd, &cap, &s_blocks[b], idCount,
                      period_sec, t->planId, t->ttId);
    }
}

/* ==========================================================================
 * The task
 * ========================================================================== */

static void service_due(void)
{
    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        sTimerSlot *t = &s_timers[i];

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
                     t->devOrd, t->planId, t->ttId, (unsigned)t->period_sec,
                     (unsigned long)t->missed);
            TRiceS("Modbus: missed %s\n", id);
        }
    }
}

static void engine_task(void *arg)
{
    (void)arg;

    TRice("Modbus: started\n");

    for (;;) {
        uint8_t msg;

        /* The wake is the queue; the 200 ms floor only keeps a lost poke from
         * stalling pending API work forever. */
        (void)osMessageQueueGet(s_queue, &msg, NULL, pdMS_TO_TICKS(200));

        /* A config swap is committed here, on the task that walks the config,
         * so a reader can never race one. */
        if (MbCfgStore_IsSwapPending() &&
            MbCfgStore_RegionValid(MbCfgStore_InactiveBase())) {
            if (MbCfgStore_CommitSwap() == 0) {
                sModbusConfigCounts counts;

                ModbusReq_CompleteForSwap();
                ModbusPlans_Refresh();
                s_resched = 1;
                TRice("Modbus: config swapped, active region %u\n",
                      (unsigned)(MbCfgStore_ActiveBase() ==
                                 EXT_FLASH_MODBUS_LUT_B_ADDR));
                if (MbCfg_Count(MbCfgStore_ActiveBase(), &counts) != 0) {
                    memset(&counts, 0, sizeof(counts));
                }
                ModbusDispatch_Config(
                    (uint8_t)(MbCfgStore_ActiveBase() ==
                              EXT_FLASH_MODBUS_LUT_B_ADDR), &counts);
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

void ModbusEngine_LogStatus(void)
{
    sModbusConfigCounts counts;
    int                 haveCounts =
        (MbCfg_Count(MbCfgStore_ActiveBase(), &counts) == 0);
    uint8_t             timers = 0;
    char                buf[110];

    for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
        if (s_timers[i].used) {
            timers++;
        }
    }

    snprintf(buf, sizeof(buf),
             "%s polls=%u errors=%u missed=%u timers=%u subs=%s",
             s_running ? "running" : "stopped", (unsigned)s_pollCount,
             (unsigned)s_errorCount, (unsigned)s_missedTotal, timers,
             ModbusSub_AnyLive() ? "yes" : "none");
    TRiceS("Modbus engine: %s\n", buf);

    if (!haveCounts) {
        /* Zero devices is a valid, first-class state (§4.2). */
        TRice("Modbus: unprovisioned (no valid config)\n");
        return;
    }
    TRice("Modbus config: region %u, %u caps %u devices %u plans %u points\n",
          (unsigned)(MbCfgStore_ActiveBase() == EXT_FLASH_MODBUS_LUT_B_ADDR),
          counts.capabilities, counts.devices, counts.plans, counts.points);

    sMbCfgCursor        c;
    sModbusDeviceRecord dev;
    uint8_t             devOrd = 0;

    if (MbCfg_SeekDevices(MbCfgStore_ActiveBase(), &c) == 0) {
        while (MbCfg_NextDevice(&c, &dev) == 1 && devOrd < MB_MAX_DEVICES) {
            uint8_t polled = 0;

            for (uint8_t i = 0; i < MB_MAX_TIMERS; i++) {
                if (s_timers[i].used && s_timers[i].devOrd == devOrd) {
                    polled++;
                }
            }
            /* "Why is this device not polled" is a question about SUBSCRIBERS
             * and about the port, so the answer is both (§4.3, §4.2). */
            snprintf(buf, sizeof(buf), "%u %s slave=%u cap=%u %lu %s %s x%u",
                     devOrd, dev.topicPrefix, dev.slaveAddr, dev.capId,
                     (unsigned long)MbRecords_BaudFromCode(dev.baudCode),
                     (dev.portId == mbPort_test) ? "test" : "rs485",
                     ModbusPort_IsRegistered(dev.portId) ? "up" : "NO-DRIVER",
                     polled);
            TRiceS(" device: %s\n", buf);
            devOrd++;
        }
    }

    sMbPlanHeader hdr[MB_MAX_PLANS];
    if (MbCfg_ReadPlanHeaders(MbCfgStore_ActiveBase(), hdr) > 0) {
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

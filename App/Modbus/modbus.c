/**
 * @file    modbus.c
 * @brief   The module's public surface (modbus.h) over the v1 engine.
 *
 * docs/modbus.md §10 step 1: a thin facade, so consumers stop moving while the
 * insides change.  What is real here is the CONTRACT — bounds, the per-item
 * result set, "the callback always fires" — not the engine underneath it, which
 * is still modbus_walker.c's lap-based poller plus modbus_rtu.c's blocking
 * transactions.  Steps 8-12 replace that engine without touching this file's
 * promises.
 *
 * Two things are deliberately v1-shaped until the record format moves (step 6),
 * and both are noted at their site: a point's access is one WRITABLE flag
 * rather than r/w/rw, and write bounds are always authored so there is no
 * unbounded case to fall back on.
 */

#include "App/Modbus/modbus.h"
#include "App/Modbus/modbus_internal.h"
#include "App/Modbus/modbus_engine.h"
#include "App/system.h"

#include "modbus_config_store.h"
#include "modbus_config_compiler.h"
#include "modbus_config_export.h"
#include "modbus_config_plans.h"
#include "modbus_decode.h"
#include "modbus_frame.h"

#include "cmsis_os.h"
#include "trice.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Widest point a request may touch in one item.  Covers every realistic
 * ASCII field (16 chars = 8 registers) and keeps the read buffer on the
 * modbus task's 2 KB stack rather than in CCM. */
#define REQ_MAX_POINT_REGS   32u

/* --------------------------------------------------------------------------
 * Subscription table (docs/modbus.md §4.3, §4.8)
 *
 * 8 entries in MAIN SRAM, not CCM — nothing about dispatch is latency-critical.
 *
 * Concurrency: consumers live in the mqtt, http and cmd tasks, the dispatcher
 * lives in the modbus task, and there is no mutex.  Subscribe claims a slot in
 * a brief critical section and writes `inUse` LAST, behind a barrier, so the
 * dispatcher sees either a complete entry or no entry.  That is at most 8
 * critical sections in the life of the system.
 * -------------------------------------------------------------------------- */

typedef struct {
    fModbusSubscriber cb;
    void             *ctx;
    uint32_t          eventMask;
    uint8_t           planMask;
    volatile uint8_t  inUse;
} sSubEntry;

static sSubEntry        s_subs[MB_MAX_SUBS];
static volatile uint8_t s_unsubPending;   /* one bit per handle */
static volatile uint8_t s_catPending;     /* one bit per handle */

/* Demand-driven polling (§4.3): a plan nobody subscribes to is not polled.
 * Until plans are records this is the whole of it — no subscribers, no bus
 * traffic. */
int ModbusSub_AnyLive(void)
{
    for (uint8_t i = 0; i < MB_MAX_SUBS; i++) {
        if (s_subs[i].inUse) {
            return 1;
        }
    }
    return 0;
}

/* The union of live plan masks: a plan outside it creates no timers, and a
 * device covered by no live plan is not polled at all (§4.3). */
uint8_t ModbusSub_PlanUnion(void)
{
    uint8_t mask = 0;

    for (uint8_t i = 0; i < MB_MAX_SUBS; i++) {
        if (s_subs[i].inUse) {
            mask |= s_subs[i].planMask;
        }
    }
    return mask;
}

static void sub_release(uint8_t i)
{
    fModbusSubscriber cb  = s_subs[i].cb;
    void             *ctx = s_subs[i].ctx;

    s_subs[i].inUse = 0u;
    __DMB();

    /* The final call IS the release point: after it returns the module never
     * calls again and the slot is reusable. */
    if (cb != NULL) {
        sModbusEvent ev = { mbEvt_released, HAL_GetTick(), { { 0 } } };
        cb(&ev, ctx);
    }
    s_subs[i].cb  = NULL;
    s_subs[i].ctx = NULL;
    ModbusEngine_Resched();
}

/* --------------------------------------------------------------------------
 * The catalogue (docs/modbus.md §4.5)
 *
 * A consumer needs the point LIST before any value arrives — HA discovery is
 * the forcing case: it publishes one message per point at connect time and
 * cannot wait for samples, because a 60 s point would take a minute and a
 * point on a silent device would never appear.
 *
 * So the module replays a burst of mbEvt_pointDesc, one per point in the
 * subscription's scope, `last` on the final entry.  It is delivered after
 * Subscribe, after every config swap, and on demand — and it ALWAYS runs on
 * the modbus task, because dispatching it inline would have mqttTask reading
 * config records out of flash.
 *
 * Note that no record is RAM-resident: a catalogue call is a flash walk.  That
 * property is load-bearing and must not leak away through the API.
 * -------------------------------------------------------------------------- */

void ModbusDesc_Build(sModbusPointDesc *d, const char *topicPrefix,
                      uint8_t devOrd, uint16_t ptOrd, uint32_t period_sec,
                      const sModbusPointRecord *pt)
{
    d->topicPrefix = topicPrefix;
    d->name        = pt->name;
    d->writeMin    = pt->writeMin;
    d->writeMax    = pt->writeMax;
    d->period_sec  = period_sec;
    d->ptOrd       = ptOrd;
    d->devOrd      = devOrd;
    d->decodeType  = pt->decodeType;
    d->unit        = pt->unit;
    d->scalePow10  = pt->scalePow10;

    /* The record carries r/w/rw and MB_PT_BOUNDED as authored (§3.2). */
    d->flags = pt->flags;
}

static void catalogue_emit(const sSubEntry *s, const sModbusPointDesc *pt,
                           uint8_t last)
{
    sModbusEvent ev;

    ev.type          = mbEvt_pointDesc;
    ev.tick          = HAL_GetTick();
    ev.u.desc.pt     = pt;
    ev.u.desc.last   = last;
    s->cb(&ev, s->ctx);
}

/* The shortest period any plan in scope reads {devOrd, ptOrd} at, or 0 if
 * nothing watches it — "capable is not the same as monitored" (§4.5).  A point
 * watched by two plans produces ONE descriptor carrying the fastest cadence
 * anything will actually deliver it at. */
static uint32_t catalogue_period(uint32_t base, uint8_t planMask,
                                 uint8_t devOrd, uint16_t capId,
                                 uint16_t ptOrd)
{
    sMbCfgCursor      c;
    sModbusPlanRecord plan;
    uint32_t          best = 0;

    if (MbCfg_SeekPlans(base, &c) != 0) {
        return 0;
    }

    while (MbCfg_NextPlan(&c, &plan) == 1) {
        sModbusTimeTableRecord tt;
        /* IN SCOPE means in THIS subscription's plan mask: 0 means no plan
         * the subscriber can see watches the point, and a consumer sizing a
         * staleness timeout must not be told about a cadence it will never be
         * delivered at (§4.5). */
        int                    covers = (plan.planId < MB_MAX_PLANS) &&
                                        ((planMask &
                                          (uint8_t)(1u << plan.planId)) != 0u) &&
                                        (plan.capId == capId) &&
                                        ((plan.devices &
                                          (uint8_t)(1u << devOrd)) != 0u);
        int rt;

        while ((rt = MbCfg_NextTimeTable(&c, &tt)) == 1) {
            uint16_t ids[MB_MAX_TT_ENTRIES_PER_TABLE];
            uint16_t take = (tt.entryCount > MB_MAX_TT_ENTRIES_PER_TABLE)
                                ? MB_MAX_TT_ENTRIES_PER_TABLE : tt.entryCount;

            if (MbCfg_ReadPointIds(&c, ids, take) != 0 ||
                MbCfg_ReadPointIds(&c, NULL,
                                   (uint16_t)(tt.entryCount - take)) != 0) {
                return best;
            }
            if (!covers) {
                continue;
            }
            for (uint16_t k = 0; k < take; k++) {
                if (ids[k] == ptOrd &&
                    (best == 0u || tt.period_sec < best)) {
                    best = tt.period_sec;
                }
            }
        }
        if (rt != 0) {
            return best;
        }
    }
    return best;
}

/* How many descriptors a full burst produces: INSTANCES, not records — a
 * capability shared by four devices describes four devices' worth of points. */
static uint16_t catalogue_total(uint32_t base)
{
    sMbCfgCursor        c;
    sModbusDeviceRecord dev;
    uint16_t            total = 0;

    if (MbCfg_SeekDevices(base, &c) != 0) {
        return 0;
    }
    while (MbCfg_NextDevice(&c, &dev) == 1) {
        sMbCfgCursor            pc;
        sModbusCapabilityRecord cap;
        sModbusPointRecord      pt;

        if (MbCfg_OpenCapability(base, dev.capId, &pc, &cap, NULL, 0) != 0) {
            continue;
        }
        while (MbCfg_NextPoint(&pc, &pt) == 1) {
            total++;
        }
    }
    return total;
}

static void catalogue_replay(uint8_t i)
{
    const sSubEntry    *s    = &s_subs[i];
    uint32_t            base = MbCfgStore_ActiveBase();
    sMbCfgCursor        c;
    sModbusDeviceRecord dev;
    uint16_t            total = catalogue_total(base);
    uint16_t            emitted = 0;
    uint8_t             devOrd = 0;

    if (total == 0u || MbCfg_SeekDevices(base, &c) != 0) {
        catalogue_emit(s, NULL, 1u);       /* the empty catalogue */
        return;
    }

    while (MbCfg_NextDevice(&c, &dev) == 1) {
        sMbCfgCursor            pc;
        sModbusCapabilityRecord cap;
        sModbusPointRecord      pt;
        uint16_t                ptOrd = 0;

        if (MbCfg_OpenCapability(base, dev.capId, &pc, &cap, NULL, 0) != 0) {
            devOrd++;
            continue;
        }
        while (MbCfg_NextPoint(&pc, &pt) == 1) {
            sModbusPointDesc desc;

            ModbusDesc_Build(&desc, dev.topicPrefix, devOrd, ptOrd,
                             catalogue_period(base, s->planMask, devOrd,
                                              dev.capId, ptOrd),
                             &pt);
            emitted++;
            catalogue_emit(s, &desc, (uint8_t)(emitted >= total));
            ptOrd++;
        }
        devOrd++;
    }

    /* A malformed stream must still terminate the burst, or a consumer waits
     * for a `last` that never comes. */
    if (emitted < total) {
        catalogue_emit(s, NULL, 1u);
    }
}

void ModbusSub_Service(void)
{
    uint8_t unsubs, cats;

    taskENTER_CRITICAL();
    unsubs         = s_unsubPending;
    cats           = s_catPending;
    s_unsubPending = 0u;
    s_catPending   = 0u;
    taskEXIT_CRITICAL();

    for (uint8_t i = 0; i < MB_MAX_SUBS; i++) {
        uint8_t bit = (uint8_t)(1u << i);

        /* A subscription released this lap does not get a catalogue first. */
        if (unsubs & bit) {
            sub_release(i);
            continue;
        }
        if ((cats & bit) && s_subs[i].inUse && s_subs[i].cb != NULL &&
            (s_subs[i].eventMask & (uint32_t)mbEvt_pointDesc) != 0u) {
            catalogue_replay(i);
        }
    }
}

void ModbusSub_CatalogueAll(void)
{
    taskENTER_CRITICAL();
    for (uint8_t i = 0; i < MB_MAX_SUBS; i++) {
        if (s_subs[i].inUse) {
            s_catPending |= (uint8_t)(1u << i);
        }
    }
    taskEXIT_CRITICAL();
}

int Modbus_RequestCatalogue(int handle)
{
    if (handle < 0 || handle >= (int)MB_MAX_SUBS || !s_subs[handle].inUse) {
        return mbErr_badArg;
    }

    /* Posted, not inline: the replay is a flash walk and it fires callbacks,
     * neither of which belongs on the caller's task. */
    taskENTER_CRITICAL();
    s_catPending |= (uint8_t)(1u << handle);
    taskEXIT_CRITICAL();
    ModbusEngine_Poke();
    return 0;
}

/* --------------------------------------------------------------------------
 * Dispatch — delivery is the call (§1.2)
 *
 * Scoping is a bit test and stores nothing.  UNTIL STEP 6 no event belongs to
 * a plan, so an event's scope is "the whole config" and every planMask matches;
 * the test is written out anyway so step 6 only has to supply the event's
 * planId.
 * -------------------------------------------------------------------------- */

static void dispatch(const sModbusEvent *ev, uint8_t evPlanMask)
{
    for (uint8_t i = 0; i < MB_MAX_SUBS; i++) {
        const sSubEntry *s = &s_subs[i];

        if (!s->inUse || s->cb == NULL) {
            continue;
        }
        if ((s->planMask & evPlanMask) == 0u) {
            continue;
        }
        if ((s->eventMask & (uint32_t)ev->type) == 0u) {
            continue;
        }
        s->cb(ev, s->ctx);
    }
}

void ModbusDispatch_Sample(const sModbusPointDesc *pt, int32_t value,
                           const char *text)
{
    sModbusEvent ev;

    ev.type             = mbEvt_sample;
    ev.tick             = HAL_GetTick();
    ev.u.sample.pt      = pt;
    ev.u.sample.value   = value;
    ev.u.sample.text    = text;
    dispatch(&ev, MB_PLAN_ALL);
}

void ModbusDispatch_Txn(uint8_t devOrd, uint8_t slaveAddr, uint16_t addr,
                        uint16_t regs, uint16_t elapsed_ms, int16_t err,
                        uint8_t planId, uint8_t timeTableId)
{
    sModbusEvent ev;

    ev.type                = mbEvt_txn;
    ev.tick                = HAL_GetTick();
    ev.u.txn.addr          = addr;
    ev.u.txn.regs          = regs;
    ev.u.txn.elapsed_ms    = elapsed_ms;
    ev.u.txn.err           = err;
    ev.u.txn.devOrd        = devOrd;
    ev.u.txn.slaveAddr     = slaveAddr;
    ev.u.txn.planId        = planId;
    ev.u.txn.timeTableId   = timeTableId;
    dispatch(&ev, MB_PLAN_ALL);
}

void ModbusDispatch_Config(uint8_t activeRegion,
                           const sModbusConfigCounts *counts)
{
    sModbusEvent ev;

    ev.type                 = mbEvt_config;
    ev.tick                 = HAL_GetTick();
    ev.u.config.counts      = *counts;
    ev.u.config.activeRegion = activeRegion;
    dispatch(&ev, MB_PLAN_ALL);
}

/* --------------------------------------------------------------------------
 * Public API — subscriptions
 * -------------------------------------------------------------------------- */

int Modbus_Subscribe(uint8_t planMask, uint32_t eventMask,
                     fModbusSubscriber cb, void *ctx)
{
    if (cb == NULL || planMask == 0u) {
        return mbErr_badArg;
    }

    for (uint8_t i = 0; i < MB_MAX_SUBS; i++) {
        int claimed = 0;

        taskENTER_CRITICAL();
        if (!s_subs[i].inUse) {
            s_subs[i].cb        = cb;
            s_subs[i].ctx       = ctx;
            s_subs[i].eventMask = eventMask;
            s_subs[i].planMask  = planMask;
            __DMB();                 /* publish the entry before the flag */
            s_subs[i].inUse     = 1u;
            /* A consumer needs the point list before any value arrives, so a
             * subscription implies a catalogue (§4.5).  It is posted: only the
             * slot claim is synchronous. */
            s_catPending       |= (uint8_t)(1u << i);
            claimed = 1;
        }
        taskEXIT_CRITICAL();

        if (claimed) {
            /* A subscription is what makes a plan run (§4.3). */
            ModbusEngine_Resched();
            return (int)i;
        }
    }
    return mbErr_full;
}

int Modbus_Unsubscribe(int handle)
{
    if (handle < 0 || handle >= (int)MB_MAX_SUBS || !s_subs[handle].inUse) {
        return mbErr_badArg;
    }

    /* With no engine running nothing would ever service the post, and a ctx
     * that is never released is the one failure this contract may not have.
     * No dispatch can be in flight either, because dispatch happens on the
     * task that is not running. */
    if (!ModbusEngine_IsRunning()) {
        sub_release((uint8_t)handle);
        return 0;
    }

    taskENTER_CRITICAL();
    s_unsubPending |= (uint8_t)(1u << handle);
    taskEXIT_CRITICAL();
    ModbusEngine_Poke();
    return 0;
}

/* --------------------------------------------------------------------------
 * Plans (docs/modbus.md §3.5, §4.3)
 *
 * PLAN HEADERS ARE RESIDENT, all 8 slots — the one deliberate exception to
 * "no record is RAM-resident", and stated rather than discovered because the
 * rule it bends is load-bearing everywhere else.  It is what makes PlanList
 * synchronous and what lets a critical section answer "is this plan active"
 * without touching flash.
 *
 * An edit is claimed synchronously (so the call keeps a real error return) and
 * finished on the modbus task (the region rewrite is a flash write).  One edit
 * is in flight at a time; a second gets mbErr_busy.
 * -------------------------------------------------------------------------- */

static sMbPlanHeader s_plans[MB_MAX_PLANS];

static struct {
    volatile uint8_t pending;    /* 0 = none                               */
    uint8_t  slot;
    uint8_t  isDelete;
    char     name[MB_NAME_LEN];
    uint16_t capId;
    uint8_t  devices;
    uint8_t  tableCount;
    uint32_t periods[MB_MAX_TIME_TABLES_PER_PLAN];
    uint16_t counts[MB_MAX_TIME_TABLES_PER_PLAN];
    uint16_t ids[MB_MAX_TT_ENTRIES_PER_PLAN];
} s_edit;

void ModbusPlans_Refresh(void)
{
    (void)MbCfg_ReadPlanHeaders(MbCfgStore_ActiveBase(), s_plans);
}

/* How many live subscriptions NAMED this plan.  Wildcards do not count: a
 * subscriber that asked for every plan expressed no dependency on which plans
 * exist, and without this rule the feature would be dead on arrival — the MQTT
 * bridge and the Trice sink both subscribe to everything (§3.5). */
static uint8_t plan_subscribers(uint8_t planId)
{
    uint8_t bit = (uint8_t)(1u << planId);
    uint8_t n   = 0;

    for (uint8_t i = 0; i < MB_MAX_SUBS; i++) {
        if (s_subs[i].inUse && s_subs[i].planMask != MB_PLAN_ALL &&
            (s_subs[i].planMask & bit) != 0u) {
            n++;
        }
    }
    return n;
}

/* Which plan slots name this device.  Straight off the resident header
 * table, so it needs no flash walk (§3.5). */
uint8_t ModbusPlans_CoveringDevice(uint8_t devOrd)
{
    uint8_t mask = 0;

    for (uint8_t i = 0; i < MB_MAX_PLANS; i++) {
        if (s_plans[i].used &&
            (s_plans[i].rec.devices & (uint8_t)(1u << devOrd)) != 0u) {
            mask |= (uint8_t)(1u << i);
        }
    }
    return mask;
}

static void plan_info_from(uint8_t slot, sModbusPlanInfo *out)
{
    memcpy(out->name, s_plans[slot].rec.name, MB_NAME_LEN);
    out->name[MB_NAME_LEN - 1] = '\0';
    out->capId       = s_plans[slot].rec.capId;
    out->planId      = slot;
    out->devices     = s_plans[slot].rec.devices;
    out->timeTables  = s_plans[slot].tableCount;
    out->subscribers = plan_subscribers(slot);
}

int Modbus_PlanList(sModbusPlanInfo *out, uint8_t max)
{
    int n = 0;

    if (out == NULL) {
        return mbErr_badArg;
    }
    for (uint8_t i = 0; i < MB_MAX_PLANS && n < (int)max; i++) {
        if (s_plans[i].used) {
            plan_info_from(i, &out[n++]);
        }
    }
    return n;
}

int Modbus_PlanGet(uint8_t planId, sModbusPlanInfo *out)
{
    if (out == NULL || planId >= MB_MAX_PLANS) {
        return mbErr_badArg;
    }
    if (!s_plans[planId].used) {
        return mbErr_idNotFound;
    }
    plan_info_from(planId, out);
    return 0;
}

/* Shared/'s refusal reasons -> the API's code set. */
static int err_from_plan(eModbusPlanErr e)
{
    switch (e) {
    case mbPlan_ok:           return 0;
    case mbPlan_errNoCap:
    case mbPlan_errNoDevice:
    case mbPlan_errNoPoint:   return mbErr_idNotFound;
    case mbPlan_errStream:    return mbErr_config;
    case mbPlan_errFull:      return mbErr_full;
    case mbPlan_errFlash:     return mbErr_config;
    default:                  return mbErr_badArg;
    }
}

/* Copy the spec into module memory: the caller's arrays are borrowed only for
 * the duration of the call, and the rewrite happens later on another task. */
static int stage_edit(uint8_t slot, const sModbusPlanSpec *spec)
{
    uint16_t total = 0;

    if (spec != NULL) {
        if (spec->name == NULL ||
            spec->tableCount > MB_MAX_TIME_TABLES_PER_PLAN) {
            return mbErr_badArg;
        }
        for (uint8_t t = 0; t < spec->tableCount; t++) {
            if (spec->tables[t].points == NULL) {
                return mbErr_badArg;
            }
            total = (uint16_t)(total + spec->tables[t].count);
        }
        if (total > MB_MAX_TT_ENTRIES_PER_PLAN) {
            return mbErr_badArg;
        }
    }

    memset(&s_edit, 0, sizeof(s_edit));
    s_edit.slot     = slot;
    s_edit.isDelete = (spec == NULL) ? 1u : 0u;

    if (spec != NULL) {
        snprintf(s_edit.name, sizeof(s_edit.name), "%s", spec->name);
        s_edit.capId      = spec->capId;
        s_edit.devices    = spec->devices;
        s_edit.tableCount = spec->tableCount;

        uint16_t at = 0;
        for (uint8_t t = 0; t < spec->tableCount; t++) {
            s_edit.periods[t] = spec->tables[t].period_sec;
            s_edit.counts[t]  = spec->tables[t].count;
            memcpy(&s_edit.ids[at], spec->tables[t].points,
                   (size_t)spec->tables[t].count * sizeof(uint16_t));
            at = (uint16_t)(at + spec->tables[t].count);
        }
    }
    return 0;
}

/* Claim the slot and post.  The claim borrows Subscribe's critical section,
 * which is the whole reason mbErr_busy can be reported synchronously: a
 * Subscribe arriving afterwards cannot pin a plan whose edit is already
 * committed, and one that arrived before makes the edit fail cleanly (§4.8). */
static int plan_edit(uint8_t slot, const sModbusPlanSpec *spec, int mustExist)
{
    int claimed = 0;

    if (slot >= MB_MAX_PLANS) {
        return mbErr_badArg;
    }
    if (!MbCfgStore_RegionValid(MbCfgStore_ActiveBase())) {
        return mbErr_config;
    }
    /* A plan edit and an upload compete for the same inactive region (§7.3). */
    if (MbCfgStore_IsSwapPending()) {
        return mbErr_busy;
    }
    if (spec != NULL) {
        int v = err_from_plan(MbCfgPlans_Validate(MbCfgStore_ActiveBase(),
                                                  spec));
        if (v != 0) {
            return v;
        }
    }

    taskENTER_CRITICAL();
    if (s_edit.pending) {
        taskEXIT_CRITICAL();
        return mbErr_busy;
    }
    if (mustExist && !s_plans[slot].used) {
        taskEXIT_CRITICAL();
        return mbErr_idNotFound;
    }
    if (plan_subscribers(slot) != 0u) {
        taskEXIT_CRITICAL();
        return mbErr_busy;
    }
    if (stage_edit(slot, spec) == 0) {
        s_edit.pending = 1u;
        claimed = 1;
    }
    taskEXIT_CRITICAL();

    if (claimed) {
        ModbusEngine_Poke();
    }
    return claimed ? 0 : mbErr_badArg;
}

int Modbus_PlanTables(uint8_t planId, sModbusTimeTableSpec *tables,
                      uint8_t maxTables, uint16_t *idBuf, uint16_t maxIds)
{
    sMbCfgCursor      c;
    sModbusPlanRecord plan;

    if (tables == NULL || idBuf == NULL || planId >= MB_MAX_PLANS) {
        return mbErr_badArg;
    }
    if (!s_plans[planId].used) {
        return mbErr_idNotFound;
    }
    if (MbCfg_SeekPlans(MbCfgStore_ActiveBase(), &c) != 0) {
        return mbErr_config;
    }

    while (MbCfg_NextPlan(&c, &plan) == 1) {
        sModbusTimeTableRecord tt;
        uint8_t                n  = 0;
        uint16_t               at = 0;
        int                    rt;

        while ((rt = MbCfg_NextTimeTable(&c, &tt)) == 1) {
            if (plan.planId != planId || n >= maxTables ||
                at + tt.entryCount > maxIds) {
                if (MbCfg_ReadPointIds(&c, NULL, tt.entryCount) != 0) {
                    return mbErr_config;
                }
                continue;
            }
            if (MbCfg_ReadPointIds(&c, &idBuf[at], tt.entryCount) != 0) {
                return mbErr_config;
            }
            tables[n].period_sec = tt.period_sec;
            tables[n].points     = &idBuf[at];
            tables[n].count      = tt.entryCount;
            at = (uint16_t)(at + tt.entryCount);
            n++;
        }
        if (rt != 0) {
            return mbErr_config;
        }
        if (plan.planId == planId) {
            return (int)n;
        }
    }
    return mbErr_idNotFound;
}

int Modbus_PlanParse(fModbusByteSource src, void *srcCtx,
                     sModbusPlanSpec *spec, int *outId,
                     sModbusCompileResult *err)
{
    if (src == NULL || spec == NULL || outId == NULL || err == NULL) {
        return mbErr_badArg;
    }
    return (MbCfgParsePlan(src, srcCtx, spec, outId, err) == 0)
               ? 0 : mbErr_badArg;
}

int Modbus_PlanCreate(const sModbusPlanSpec *spec, uint8_t *outPlanId)
{
    if (spec == NULL) {
        return mbErr_badArg;
    }

    /* The lowest free slot, so slots stay compact without ever moving one. */
    for (uint8_t i = 0; i < MB_MAX_PLANS; i++) {
        if (s_plans[i].used) {
            continue;
        }
        int r = plan_edit(i, spec, 0);
        if (r == 0 && outPlanId != NULL) {
            *outPlanId = i;
        }
        return r;
    }
    return mbErr_full;
}

int Modbus_PlanModify(uint8_t planId, const sModbusPlanSpec *spec)
{
    return (spec == NULL) ? mbErr_badArg : plan_edit(planId, spec, 1);
}

int Modbus_PlanDelete(uint8_t planId)
{
    return plan_edit(planId, NULL, 1);
}

/* Runs on the modbus task: the flash rewrite and the swap arming. */
void ModbusPlans_Service(void)
{
    sModbusTimeTableSpec tables[MB_MAX_TIME_TABLES_PER_PLAN];
    sModbusPlanSpec      spec;
    eModbusPlanErr       r;

    if (!s_edit.pending) {
        return;
    }

    if (s_edit.isDelete) {
        r = MbCfgPlans_Rewrite(MbCfgStore_ActiveBase(),
                               MbCfgStore_InactiveBase(),
                               s_edit.slot, NULL, KickIwdg);
    } else {
        uint16_t at = 0;
        for (uint8_t t = 0; t < s_edit.tableCount; t++) {
            tables[t].period_sec = s_edit.periods[t];
            tables[t].points     = &s_edit.ids[at];
            tables[t].count      = s_edit.counts[t];
            at = (uint16_t)(at + s_edit.counts[t]);
        }
        spec.name       = s_edit.name;
        spec.tables     = tables;
        spec.capId      = s_edit.capId;
        spec.tableCount = s_edit.tableCount;
        spec.devices    = s_edit.devices;

        r = MbCfgPlans_Rewrite(MbCfgStore_ActiveBase(),
                               MbCfgStore_InactiveBase(),
                               s_edit.slot, &spec, KickIwdg);
    }

    if (r == mbPlan_ok && MbCfgStore_SetSwapPending() == 0) {
        TRice("Modbus plan: slot %u %s, swap armed\n", s_edit.slot,
              s_edit.isDelete ? "deleted" : "written");
    } else {
        TRice("Modbus plan: slot %u edit failed (%d)\n", s_edit.slot, (int)r);
    }

    s_edit.pending = 0u;
}

/* --------------------------------------------------------------------------
 * The submission FIFO (docs/modbus.md §4.6)
 *
 * IN FLIGHT: A FIFO OF 8 SUBMISSIONS, drained in arrival order, ~32 B each.
 * Depth 8 is a FAIRNESS bound, not a throughput one: because the deadline runs
 * from submission, the worst wait for the last entry is depth x maximum
 * timeout.
 *
 * The claim is a critical section around head/tail, so a submitter never
 * blocks and the modbus task is the only thing that retires an entry.
 * -------------------------------------------------------------------------- */

#define REQ_FIFO_DEPTH   8u

typedef struct {
    sModbusReqItem  *items;
    uint16_t         count;
    uint8_t          devOrd;
    uint32_t         submitTick;
    uint32_t         timeout_ms;
    uint32_t         gen;            /* config generation at submission */
    fModbusReqDone   cb;
    void            *ctx;
} sReqEntry;

/* A SUBMISSION CARRIES THE CONFIG GENERATION IT WAS MADE AGAINST, which is
 * what makes "a config swap completes outstanding requests" implementable
 * (§4.8): an id resolved against the retiring generation may mean something
 * else now, or nothing at all. */
static volatile uint32_t s_configGen;

static sReqEntry        s_fifo[REQ_FIFO_DEPTH];
static volatile uint8_t s_fifoHead;      /* next to service */
static volatile uint8_t s_fifoCount;
static sReqEntry        s_req;           /* the one being serviced */
static volatile uint8_t s_reqActive;

static uint32_t s_requestsDone;

/* --------------------------------------------------------------------------
 * Transport result -> the API's code set
 *
 * The transport says how one blocking transaction ended; the API says what
 * happened to an item.  A slave that answers an exception is answering, so its
 * code is folded in per item (§4.6) rather than kept as one global "last
 * exception" nobody can attribute.
 * -------------------------------------------------------------------------- */

int16_t ModbusErr_FromFrame(int frameErr, uint8_t exc)
{
    switch (frameErr) {
    case mbFrame_ok:          return mbErr_ok;
    case mbFrame_errShort:
    case mbFrame_errCount:    return mbErr_short;
    case mbFrame_errCrc:      return mbErr_crc;
    case mbFrame_errAddr:
    case mbFrame_errFunction: return mbErr_short;   /* not our reply       */
    case mbFrame_errBadArg:   return mbErr_badArg;
    case mbFrame_exception:
        switch (exc) {
        case 1u:  return mbErr_excIllegalFunction;
        case 2u:  return mbErr_excIllegalAddress;
        case 3u:  return mbErr_excIllegalValue;
        case 4u:  return mbErr_excDeviceFailure;
        default:  return mbErr_excOther;
        }
    default:                  return mbErr_excOther;
    }
}

/* --------------------------------------------------------------------------
 * One item
 * -------------------------------------------------------------------------- */

/* Line parameters travel with the frame, so a device at another rate is just
 * the next transaction's parameters (§3.4).
 *
 * `budget_ms` is what is left of THIS REQUEST'S deadline.  Capping the port
 * timeout by it is what makes "the completion callback always fires, within
 * timeout_ms" true rather than aspirational: without it a 100-item batch at
 * one second per item takes 100 seconds regardless of what the caller asked
 * for.  It is also what makes an item that ran out of budget distinguishable
 * from one that was never started (§4.6). */
static void params_from(const sMbPointLookup *lk, sModbusPortParams *p,
                        uint32_t budget_ms)
{
    uint32_t portTimeout = ModbusEngine_ResponseTimeoutMs();

    p->baud               = MbRecords_BaudFromCode(lk->baudCode);
    p->format             = lk->format;
    p->responseTimeout_ms = (budget_ms < portTimeout) ? budget_ms
                                                      : portTimeout;
}

static int16_t req_read_point(const sMbPointLookup *lk, uint8_t width,
                              int32_t *value, uint32_t budget_ms)
{
    uint16_t          regs[REQ_MAX_POINT_REGS];
    sModbusPortParams params;
    int16_t           e;

    params_from(lk, &params, budget_ms);
    e = ModbusPort_Read(lk->portId, &params, lk->slaveAddr,
                        lk->functionCode, lk->regAddr, width, regs);
    if (e != mbErr_ok) {
        return e;
    }

    /* An ASCII point has no scaled value — nothing in the module computes one
     * (§4.4).  The reading still reaches subscribers as text; the item simply
     * reports that the read succeeded. */
    *value = (lk->point.decodeType == mbDecode_ascii)
                 ? 0 : MbDecode_Scaled(&lk->point, regs);
    return mbErr_ok;
}

static void req_run_item(uint8_t devOrd, sModbusReqItem *it,
                         uint32_t budget_ms)
{
    sMbPointLookup lk;
    uint8_t        width;

    if (MbCfg_ResolvePoint(devOrd, it->id, &lk) != 0) {
        it->result = mbErr_idNotFound;
        return;
    }

    width = MbRecords_RegWidth(lk.point.decodeType, lk.point.length);
    if (width == 0u || width > REQ_MAX_POINT_REGS) {
        it->result = mbErr_badArg;
        return;
    }

    /* THE CONFIG DECIDES WHAT THE ITEM MEANS (§4.6): r reads, w writes, rw
     * writes and then reads the register back. */
    if ((lk.point.flags & MB_PT_WRITE) == 0u) {
        it->result = req_read_point(&lk, width, &it->value, budget_ms);
        return;
    }

    /* The module enforces the point's bounds, not its callers (§4.6): three
     * requesters would otherwise be three copies of one rule.  A point that
     * authors NO bounds is writable across its decode type's full range —
     * absent bounds mean unconstrained, not forbidden. */
    if ((lk.point.flags & MB_PT_BOUNDED) != 0u &&
        (it->value < lk.point.writeMin || it->value > lk.point.writeMax)) {
        it->result = mbErr_outOfRange;
        return;
    }

    /* Encoding is decode's inverse, and a value that does not fit the point's
     * register width is refused before a frame is formed — never truncated
     * (§5.3).  The ADDRESS IS THE AUTHORED ONE: a write needs no stride
     * arithmetic at all, so a byte-addressed slave is written where it is
     * read (§3.3). */
    uint16_t regs[REQ_MAX_POINT_REGS];
    int      n = MbEncode_Scaled(&lk.point, it->value, regs);

    if (n <= 0 || (uint8_t)n != width) {
        it->result = mbErr_badArg;
        return;
    }

    sModbusPortParams params;
    params_from(&lk, &params, budget_ms);

    int16_t e = ModbusPort_Write(lk.portId, &params, lk.slaveAddr,
                                 lk.writeFc, lk.regAddr, regs, (uint16_t)n);
    if (e != mbErr_ok) {
        it->result = e;
        return;
    }

    /* Read-back is automatic on rw, not requested: the cost is a second round
     * trip and a per-request opt-in would have defaulted to "on" anyway
     * (§4.6).  A w point cannot be read back at all, so it says so. */
    if ((lk.point.flags & MB_PT_READ) == 0u) {
        it->result = mbErr_ok;
        return;
    }

    int32_t readBack = it->value;
    int16_t r        = req_read_point(&lk, width, &readBack, budget_ms);
    if (r == mbErr_ok) {
        it->value = readBack;
    }
    it->result = r;
}

/* --------------------------------------------------------------------------
 * Completion — the one moment the caller may reclaim its array
 * -------------------------------------------------------------------------- */

static void req_complete(void)
{
    sModbusReqReply rep = { s_req.items, s_req.count, s_req.devOrd };
    fModbusReqDone  cb  = s_req.cb;
    void           *ctx = s_req.ctx;

    s_req.items = NULL;
    s_req.cb    = NULL;
    s_requestsDone++;
    s_reqActive = 0u;           /* released BEFORE the callback, so a
                                   re-submission from inside it is accepted */
    if (cb != NULL) {
        cb(&rep, ctx);
    }
}

/* Take the oldest submission, if any. */
static int req_pop(void)
{
    int taken = 0;

    taskENTER_CRITICAL();
    if (s_fifoCount > 0u) {
        s_req = s_fifo[s_fifoHead];
        s_fifoHead = (uint8_t)((s_fifoHead + 1u) % REQ_FIFO_DEPTH);
        s_fifoCount--;
        s_reqActive = 1u;
        taken = 1;
    }
    taskEXIT_CRITICAL();
    return taken;
}

void ModbusReq_Service(void)
{
    if (!req_pop()) {
        return;
    }

    /* A request submitted against the RETIRING generation is finished
     * immediately: ids that no longer resolve say so, the rest were never
     * attempted, and the callback fires either way (§4.6). */
    if (s_req.gen != s_configGen) {
        sMbPointLookup lk;

        for (uint16_t i = 0; i < s_req.count; i++) {
            s_req.items[i].result =
                (MbCfg_ResolvePoint(s_req.devOrd, s_req.items[i].id, &lk) == 0)
                    ? mbErr_notAttempted : mbErr_idNotFound;
        }
        req_complete();
        return;
    }

    for (uint16_t i = 0; i < s_req.count; i++) {
        uint32_t spent = HAL_GetTick() - s_req.submitTick;
        uint32_t left;

        /* The deadline runs from SUBMISSION, so a batch that waited out its
         * own timeout completes with every item NEVER ATTEMPTED — which is
         * what actually bounds the caller's memory (§4.6). */
        if (spent >= s_req.timeout_ms) {
            for (uint16_t j = i; j < s_req.count; j++) {
                s_req.items[j].result = mbErr_notAttempted;
            }
            break;
        }
        left = s_req.timeout_ms - spent;

        req_run_item(s_req.devOrd, &s_req.items[i], left);

        /* The item that was ON THE WIRE when the deadline arrived is a
         * different fact from one that never started: an in-flight WRITE may
         * have landed on the slave, a never-started one certainly did not.  It
         * only claims the wire timed it out — a reply that did arrive keeps
         * its own verdict, because we know what happened to it. */
        if (s_req.items[i].result == mbErr_timeout &&
            HAL_GetTick() - s_req.submitTick >= s_req.timeout_ms) {
            s_req.items[i].result = mbErr_timedOut;
        }
    }

    req_complete();
}

void ModbusReq_CompleteForSwap(void)
{
    /* Bump FIRST: anything already queued now belongs to the retiring
     * generation, and anything submitted after this belongs to the new one. */
    s_configGen++;

    /* A CONFIG SWAP COMPLETES OUTSTANDING REQUESTS; IT NEVER DROPS THEM.  It
     * has to: "the callback always fires" is what lets a caller reclaim its
     * array (§4.6).  ModbusReq_Service does the per-item work — an id that no
     * longer resolves says mbErr_idNotFound, the rest were not attempted — so
     * draining the FIFO through it is the whole of this. */
    while (s_fifoCount > 0u) {
        ModbusReq_Service();
    }
}

/* --------------------------------------------------------------------------
 * Public API — requests
 * -------------------------------------------------------------------------- */

int Modbus_Request(uint8_t devOrd, sModbusReqItem *items, uint16_t count,
                   uint32_t timeout_ms, fModbusReqDone cb, void *ctx)
{
    /* Submit-side validation is the part the caller's task can answer for:
     * shape and slot.  Ids and values are checked at service time against the
     * config that is live then (§4.6). */
    if (items == NULL || cb == NULL ||
        count == 0u || count > MB_REQ_MAX_ITEMS ||
        timeout_ms < MB_REQ_TIMEOUT_MIN_MS ||
        timeout_ms > MB_REQ_TIMEOUT_MAX_MS) {
        return mbErr_badArg;
    }

    /* Nothing would service the batch, and a callback that never fires is the
     * one failure this API may not have. */
    if (!ModbusEngine_IsRunning()) {
        return mbErr_config;
    }

    taskENTER_CRITICAL();
    if (s_fifoCount >= REQ_FIFO_DEPTH) {
        taskEXIT_CRITICAL();
        return mbErr_full;
    }
    {
        uint8_t tail = (uint8_t)((s_fifoHead + s_fifoCount) % REQ_FIFO_DEPTH);

        s_fifo[tail].items      = items;
        s_fifo[tail].count      = count;
        s_fifo[tail].devOrd     = devOrd;
        s_fifo[tail].submitTick = HAL_GetTick();
        s_fifo[tail].timeout_ms = timeout_ms;
        s_fifo[tail].gen        = s_configGen;
        s_fifo[tail].cb         = cb;
        s_fifo[tail].ctx        = ctx;
        s_fifoCount++;
    }
    taskEXIT_CRITICAL();

    for (uint16_t i = 0; i < count; i++) {
        items[i].result = mbErr_pending;
    }
    ModbusEngine_Poke();
    return 0;
}

/* --------------------------------------------------------------------------
 * Public API — lifecycle
 * -------------------------------------------------------------------------- */

int Modbus_Init(void)
{
    if (MbCfgStore_Init() != 0) {
        return mbErr_config;
    }

    /* INVALID IS ERASED, NOT REPAIRED (§4.2): a region failing magic, version
     * or CRC is erased at init, so "invalid" collapses to one observable state
     * instead of a spectrum of partially-readable ones — and the region is
     * immediately reusable.  This is also what makes the v1 -> v2 move a wipe:
     * the version test is strict equality, so both regions fail it once and
     * the board comes up unprovisioned (§11.2). */
    if (!MbCfgStore_RegionValid(MbCfgStore_ActiveBase())) {
        (void)MbCfgStore_EraseRegion(MbCfgStore_ActiveBase());
    }
    if (!MbCfgStore_RegionValid(MbCfgStore_InactiveBase())) {
        (void)MbCfgStore_EraseRegion(MbCfgStore_InactiveBase());
    }

    ModbusPlans_Refresh();

    /* Set it and forget it: there is no Start/Stop pair, and timers are NOT
     * started here — they come and go with subscriptions, so a board that
     * boots with a valid config and no subscribers puts nothing on the wire
     * (§4.2). */
    if (ModbusEngine_Start() != 0) {
        return mbErr_config;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Public API — configuration
 * -------------------------------------------------------------------------- */

int Modbus_ConfigCompile(fModbusByteSource src, void *srcCtx,
                         sModbusCompileResult *err)
{
    if (src == NULL || err == NULL) {
        return mbErr_badArg;
    }
    if (MbCfgStore_IsSwapPending()) {
        return mbErr_busy;       /* the inactive region is about to go live */
    }

    if (MbCfgCompile(src, srcCtx, MbCfgStore_InactiveBase(),
                     KickIwdg, err) != 0) {
        return mbErr_config;     /* *err carries what actually went wrong */
    }
    return 0;
}

int Modbus_ConfigErase(void)
{
    /* Both regions: "erase" means the board is for nothing until it is told
     * what it is for, and a surviving staged config would contradict that. */
    int a = MbCfgStore_EraseRegion(MbCfgStore_ActiveBase());
    int b = MbCfgStore_EraseRegion(MbCfgStore_InactiveBase());

    /* Disarming is part of erasing, not a courtesy.  A swap armed against the
     * region just erased can never be committed (the engine requires a valid
     * inactive region) and can never be cleared, while the flag itself refuses
     * every subsequent compile — an erase would otherwise leave the config
     * plane permanently locked out, in flash, across reboots. */
    int c = MbCfgStore_IsSwapPending() ? MbCfgStore_ClearSwapPending() : 0;

    return (a == 0 && b == 0 && c == 0) ? 0 : mbErr_config;
}

int Modbus_ConfigVerify(fModbusByteSource src, void *srcCtx,
                        sModbusCompileResult *err)
{
    if (src == NULL || err == NULL) {
        return mbErr_badArg;
    }
    /* Never refused: it touches no flash and no live state. */
    return (MbCfgVerify(src, srcCtx, err) == 0) ? 0 : mbErr_config;
}

int Modbus_ConfigApply(void)
{
    if (MbCfgStore_SetSwapPending() != 0) {
        return mbErr_config;
    }
    ModbusEngine_Poke();           /* the engine commits it at a safe point */
    return 0;
}

int Modbus_ConfigExport(fModbusByteSink sink, void *ctx)
{
    if (sink == NULL) {
        return mbErr_badArg;
    }
    return (MbCfgExport(MbCfgStore_ActiveBase(), sink, ctx) == 0)
               ? 0 : mbErr_config;
}

int Modbus_DeviceList(sModbusDeviceInfo *out, uint8_t max)
{
    sMbCfgCursor        c;
    sModbusDeviceRecord dev;
    uint8_t             devOrd = 0;
    int                 n = 0;

    if (out == NULL) {
        return mbErr_badArg;
    }
    if (MbCfg_SeekDevices(MbCfgStore_ActiveBase(), &c) != 0) {
        return 0;                  /* unprovisioned: zero devices is valid */
    }

    while (n < (int)max && MbCfg_NextDevice(&c, &dev) == 1) {
        snprintf(out[n].topicPrefix, sizeof(out[n].topicPrefix), "%s",
                 dev.topicPrefix);
        out[n].devOrd    = devOrd;
        out[n].slaveAddr = dev.slaveAddr;
        out[n].capId     = (uint8_t)dev.capId;
        out[n].portId    = dev.portId;
        out[n].baud      = MbRecords_BaudFromCode(dev.baudCode);
        out[n].format    = dev.format;
        out[n].portUp    = ModbusPort_IsRegistered(dev.portId) ? 1u : 0u;
        /* "Why is this device not polled" is a question about SUBSCRIBERS
         * (§4.3), so the answer is the plans that cover it AND are live. */
        out[n].coveringPlans = ModbusPlans_CoveringDevice(devOrd);
        out[n].polled        = (uint8_t)((out[n].coveringPlans &
                                          ModbusSub_PlanUnion()) != 0u &&
                                         out[n].portUp);
        n++;
        devOrd++;
    }
    return n;
}

int Modbus_ConfigStatus(sModbusConfigStatus *out)
{
    if (out == NULL) {
        return mbErr_badArg;
    }

    uint32_t active = MbCfgStore_ActiveBase();

    memset(out, 0, sizeof(*out));
    out->activeRegion = (uint8_t)(active == EXT_FLASH_MODBUS_LUT_B_ADDR);
    out->valid        = MbCfgStore_RegionValid(active) ? 1u : 0u;
    out->stagedValid  = MbCfgStore_RegionValid(MbCfgStore_InactiveBase())
                            ? 1u : 0u;
    out->swapPending  = MbCfgStore_IsSwapPending() ? 1u : 0u;

    if (out->valid) {
        (void)MbCfg_Count(active, &out->counts);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Public API — diagnostics
 * -------------------------------------------------------------------------- */

int Modbus_Stats(sModbusStats *out)
{
    if (out == NULL) {
        return mbErr_badArg;
    }

    memset(out, 0, sizeof(*out));
    ModbusEngine_Counters(&out->polls, &out->errors, &out->missed);
    out->requests = s_requestsDone;
    out->monitor  = (uint8_t)(ModbusPort_GetMonitor() ? 1 : 0);
    return 0;
}

void Modbus_LogStatus(void)
{
    ModbusEngine_LogStatus();
}

void Modbus_SetMonitor(int enable)
{
    ModbusPort_SetMonitor(enable);
    TRice("Modbus monitor: %s\n", enable ? "on" : "off");
}

int Modbus_GetMonitor(void)
{
    return ModbusPort_GetMonitor();
}

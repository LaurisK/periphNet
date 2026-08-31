/*
 * pack.c
 *
 * The battery pack module's core: instance table, subscriptions, command
 * claim, publish/commit, dispatch, persistence and statistics
 * (docs/design_battery_pack.md §10, §11, §13, §14).
 *
 * It never learns what a holding register or a CAN id is.  Everything
 * protocol-shaped lives behind sPackType; everything pure lives in
 * pack_fsm.c; everything JSON-shaped lives in pack_cfg.c.
 *
 * THE ONE LOCK.  CoreLock/CoreUnlock is the only place in the module that
 * knows there are two calling contexts.  A TYPE never locks — CMake fails the
 * build if one tries — because a lock is a context decision and a type does
 * not know its context statically: the same pack_pylontech.c code is reached
 * from an ISR and from a task.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack.h"
#include "App/Pack/pack_type.h"
#include "App/Pack/pack_fsm.h"
#include "App/Pack/pack_soc.h"
#include "App/Pack/pack_cfg.h"
#include "App/nv_record.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#include "trice.h"

#include <stdio.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

/** The module's INTERNAL event ids (§13).  Nothing to do with
 *  ePackEventType, which is the consumer-facing mask: a consumer never sees
 *  one of these and func.c never learns what one means.  Each carries its
 *  instance index in the event's payload word. */
typedef enum {
    packInt_stateCommitted = 0,
    packInt_cellsCommitted,
    packInt_commandDone,
    packInt_liveness,
    packInt_commandSubmit,
    packInt_release,
    packInt_configChanged,
    packInt_rebind,
    packInt_last
} ePackIntEvt;

_Static_assert((int)packInt_last == (int)PACK_EVT_COUNT,
               "PACK_EVT_COUNT must be packInt_last, or func.c's range drifts");

/* Private types ------------------------------------------------------------*/

/** One command's completion, detached from the instance state it came from. */
typedef struct {
    sPackCommand cmd;
    fPackCmdDone done;
    void        *ctx;
    ePackErr     result;
    char         origin[PACK_NAME_LEN];
} sPackCmdRecord;

typedef struct {
    sPackFsm             fsm;
    sPackCmdSlot         slot;
    sPackRaw             live;
    sPackRaw             staging;
    sPackCells           cellsLive;
    sPackCells           cellsStaging;

    const sPackType     *type;
    const sPackCmdBound *bounds;        /* -> boundsOwn, below               */

    /* THE ENFORCED DOMAIN, core-owned.  It used to point straight at the
     * TYPE's storage, which silently discarded the operator's `commands`
     * bounds: they were parsed, ceiling-checked, persisted and exported, and
     * then a 500 A charge limit passed validation anyway because the JK's
     * own writeMin/writeMax was what got checked.  §12 calls that block
     * "where the answer to WHO MAY DISCONNECT THIS BATTERY belongs", so the
     * intersection is computed here, where a type cannot forget it and
     * cannot widen it. */
    sPackCmdBound        boundsOwn[packCmd_last];
    uint32_t             caps;          /* confirmed at bind                 */
    uint32_t             cmds;

    /* the in-flight command's completion, copied so no caller lifetime rule
     * survives the call (§10.9) */
    fPackCmdDone         done;
    void                *doneCtx;
    sPackCommand         pending;
    uint32_t             pendingSeq;
    char                 pendingOrigin[PACK_NAME_LEN];

    /* THE COMPLETION MAILBOX.  A completion is decided in one context (the
     * modbus task, a CAN ISR, or this task's own expiry sweep) and DELIVERED
     * on the func task, and between those two moments the slot is already
     * free -- so another task may legally claim it and overwrite `done`,
     * `doneCtx` and `pending`.  Reading them at dispatch time therefore
     * delivered the OLD result to the NEW command's callback and then NULLed
     * `done`, losing the new command's real outcome for good.
     *
     * So the record is SNAPSHOTTED inside the same critical section that
     * frees the slot, and dispatch reads only the snapshot.
     *
     * One slot is provably enough: a second command cannot complete before
     * this one is delivered, because completing requires submitting, and
     * submission happens on the func task -- the same task that drains this
     * mailbox, from a FIFO queue in which the completion was posted first. */
    sPackCmdRecord       doneRec;
    uint8_t              doneRecValid;

    /* WHAT THE NEXT DISPATCH SHOULD ANNOUNCE.  The queue entry is one word
     * and it is spent on `idx`, so a commit's `groups` used to be dropped at
     * the queue and rebuilt as 0 -- every packEvt_state claimed it refreshed
     * nothing, which is precisely the "partial delivery is visible instead of
     * silent" property §11.1 exists for.  Accumulated under the lock so two
     * commits before one dispatch coalesce instead of racing. */
    /* Statistics (§23.1) and balance-transfer accounting (§23.3). */
    sPackStat            stats[PACK_STATS_MAX];
    uint8_t              statCount;

    /* The SOC estimator (§24).  It lives in the CORE, not in a type: it
     * consumes only published, vendor-neutral values -- current, the mAh
     * counter, cell voltages -- so every type that supplies those gets it
     * without knowing it exists, which is the "estimator lands without a
     * consumer noticing" promise of §2. */
    sPackSoc             soc;                        /* pack unit + counter */
    sPackSocUnit         socCell[PACK_CELLS_MAX];    /* SAME type, per cell */
    int32_t              socBalSnap_mAs[PACK_CELLS_MAX]; /* last step's net */
    uint8_t              haveBalSnap;
    uint32_t             socRestSamples;
    uint32_t             socDisturbed_ms;   /* last |I| > C/20              */
    uint8_t              socHaveDisturb;

    sPackBalanceStats    bal;
    uint32_t             balLast_ms;    /* last NoteBalance, for the interval */
    uint32_t             balStart_ms;   /* window start, for window_sec       */
    uint8_t              balSeen;       /* an interval has been established   */

    uint32_t             pendingGroups;
    uint32_t             alarmAdded;
    uint32_t             alarmCleared;

    char                 name[PACK_NAME_LEN];
    uint32_t             nameplate_mAh;
    uint8_t              boundCount;
    uint8_t              typeId;
    uint8_t              cellCount;
    uint8_t              used;
    uint8_t              configChanged;
} sPackInst;

typedef struct {
    fPackSubscriber cb;
    void           *ctx;
    uint32_t        evtMask;
    uint8_t         inUse;
    uint8_t         releasing;
} sPackSub;

/** What survives a reboot, per instance (§23.4).
 *
 * BALANCE TOTALS ARE THE POINT.  They accumulate over weeks and are the one
 * measurement that separates a low-capacity cell from a merely discharged one
 * (§23.3) -- and this site power-cycles, so holding them only in RAM threw
 * the measurement away roughly daily.
 *
 * The SOC anchor rides along because re-anchoring costs 120 near-rest samples
 * (~2 h of favourable conditions) and losing it means publishing the vendor's
 * number again until it rebuilds.
 *
 * A STATISTIC MAY BE LOSSY.  This is written on a slow cadence, not per
 * sample, exactly as nvDb's C13 allows -- a crash costs minutes of
 * accumulation, never correctness. */
typedef struct {
    int32_t  in_mAs[PACK_CELLS_MAX];
    int32_t  out_mAs[PACK_CELLS_MAX];
    uint32_t window_sec;
    uint32_t activeSamples;
    int32_t  soc_pm;            /* -1 when never anchored                    */
    int32_t  driftResidual_pm;
    uint16_t socConf_pm;
    uint8_t  cellCount;
    uint8_t  socSeeded;

    /* THE LEARNED PER-CELL CAPACITIES.  These take days of favourable
     * conditions to measure -- two anchors at least 15 %% SOC apart -- so
     * losing them to a power cut would mean starting the measurement over
     * roughly as often as the site blinks. */
    int32_t  cellCap_mAh[PACK_CELLS_MAX];
    uint16_t cellCapConf_pm[PACK_CELLS_MAX];
    int32_t  packCap_mAh;
    uint16_t packCapConf_pm;
    uint8_t  packCapLearned;
    uint8_t  pad;
} sPackStateEntry;

typedef struct {
    sNvRecordHdr    hdr;
    sPackStateEntry pack[PACK_MAX];
} sPackStateRecord;

#define PACK_STATE_MAGIC    0x50414B53u  /* "PAKS" */
/* 2: version 1 records persisted an UNMEASURED capacity (the nameplate a
 * unit is born holding) and restored it as if measured, so a board that had
 * saved once came back claiming 16 measurements it had never made.  Bumping
 * the version makes NvRecord_Load discard those records rather than have
 * every deployed board carry the fabrication forward. */
#define PACK_STATE_VERSION  2u

/* How often the accumulation is written back.  Slow on purpose: this is a
 * statistic, and an erase-per-sample would wear the medium for nothing. */
#define PACK_STATE_SAVE_SEC 900u

/** The persisted record.  Header FIRST, as App/nv_record.h requires. */
typedef struct {
    sNvRecordHdr hdr;
    sPackCfg     cfg;
} sPackCfgRecord;

/* Private variables --------------------------------------------------------*/

static sPackInst        s_inst[PACK_MAX];
static sPackSub         s_subs[PACK_MAX_SUBS];
static const sPackType *s_types[packType_last];
static sPackCfg         s_cfg;

/* ONE SHARED CONFIGURATION SCRATCH, ~1 KB.  Pack_Init (func task, once at
 * boot), Pack_ConfigVerify and Pack_ConfigApply (both on the single http
 * task) each used to keep their own, costing four copies of a buffer that is
 * never live in two of them at the same time.  It cannot be a stack local:
 * sPackCfgRecord is ~1 KB against a 2 KB task stack, and a FreeRTOS stack is
 * pvPortMalloc'd from .ccmheap -- CCM, which DMA cannot see -- so the buffer
 * handed to NvDb_Read would silently lose the DMA path as well. */
static sPackCfgRecord   s_cfgScratch;
/* Sharing it means claiming it.  Verify and Apply both run on the single http
 * task and cannot overlap each other, but Pack_Init runs on the func task at
 * boot and an upload arriving in that window would share the buffer. */
static uint8_t          s_cfgScratchBusy;
static uint32_t         s_lastStateSave_ms;
static sPackStats       s_stats;
static fFuncPost        s_post;
static uint16_t         s_evtBase;
static uint8_t          s_provisioned;
static uint8_t          s_inited;

/* Private function prototypes ----------------------------------------------*/

static void SocOnCommit(uint8_t idx, uint32_t groups);
static void StateSave(void);
static void StateRestore(void);

static uint32_t CoreLock(void);
static void     CoreUnlock(uint32_t saved);
static void     PostInt(ePackIntEvt evt, uint8_t idx);
static void     Publish(const sPackEvent *ev);
static void     BuildState(const sPackInst *in, uint8_t idx,
                           sPackState *out, uint32_t now);
static void     BindAll(void);
static void     UnbindAll(void);

/* Private functions --------------------------------------------------------*/

/**
 * The ONE place that knows there are two contexts.  taskENTER_CRITICAL()
 * misbehaves from an ISR on this port and taskENTER_CRITICAL_FROM_ISR() is
 * the ISR form, so the choice is made here and nowhere else.
 */
static uint32_t CoreLock(void)
{
    if (0u != __get_IPSR()) {
        return taskENTER_CRITICAL_FROM_ISR();
    }
    taskENTER_CRITICAL();
    return 0u;
}

/**
 * RE-CHECKS IPSR; it does NOT infer the context from @p saved.  Zero is a
 * legitimate interrupt mask, so inferring would unlock the wrong way exactly
 * when interrupts were already unmasked — the hardest case to reproduce.
 */
static void CoreUnlock(uint32_t saved)
{
    if (0u != __get_IPSR()) {
        taskEXIT_CRITICAL_FROM_ISR(saved);
    } else {
        (void)saved;
        taskEXIT_CRITICAL();
    }
}

/* A bounded name copy.  NOT snprintf: two of these run inside the critical
 * section, and formatted output is unbounded library work that may take a
 * newlib reentrancy lock -- neither is acceptable with interrupts masked. */
static void copy_name(char *dst, const char *src, uint32_t cap)
{
    uint32_t i;

    for (i = 0u; (i + 1u) < cap; i++) {
        if (src[i] == '\0') {
            break;
        }
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void PostInt(ePackIntEvt evt, uint8_t idx)
{
    if (s_post != NULL) {
        s_post((uint16_t)(s_evtBase + (uint16_t)evt), (void *)(uintptr_t)idx);
    }
}

/** Deliver to every subscriber that asked for this type.  Runs on the shared
 *  task only; a subscriber must not block, exactly as docs/modbus.md §4.7. */
static void Publish(const sPackEvent *ev)
{
    uint8_t i;

    for (i = 0u; i < PACK_MAX_SUBS; i++) {
        const sPackSub *s = &s_subs[i];

        if ((s->inUse == 0u) || (s->cb == NULL)) {
            continue;
        }
        if ((s->evtMask & (uint32_t)ev->type) == 0u) {
            continue;
        }
        s->cb(ev, s->ctx);
    }
}

static void RaiseState(uint8_t idx, uint32_t groups)
{
    sPackEvent ev;

    (void)memset(&ev, 0, sizeof(ev));
    ev.type          = packEvt_state;
    ev.tick_ms       = (uint32_t)osKernelGetTickCount();
    ev.name          = s_inst[idx].name;
    ev.idx           = idx;
    ev.u.state.groups = groups;
    Publish(&ev);
}

/** A protection opened or closed — §4's middle case, the only kind of
 *  disconnection a pack can detect about itself. */
static void RaiseAlarm(uint8_t idx, uint32_t added, uint32_t cleared)
{
    sPackEvent ev;

    (void)memset(&ev, 0, sizeof(ev));
    ev.type            = packEvt_alarm;
    ev.tick_ms         = (uint32_t)osKernelGetTickCount();
    ev.name            = s_inst[idx].name;
    ev.idx             = idx;
    ev.u.alarm.added   = added;
    ev.u.alarm.cleared = cleared;
    Publish(&ev);
}

static void RaiseCondition(uint8_t idx, ePackCondition from, ePackCondition to)
{
    sPackEvent ev;

    (void)memset(&ev, 0, sizeof(ev));
    ev.type    = packEvt_condition;
    ev.tick_ms = (uint32_t)osKernelGetTickCount();
    ev.name    = s_inst[idx].name;
    ev.idx     = idx;
    ev.u.condition.from = (uint8_t)from;
    ev.u.condition.to   = (uint8_t)to;
    Publish(&ev);

    if (to == packCond_stale) {
        s_stats.staleEvents++;
    }
}

static void RaiseCommand(uint8_t idx, const sPackCommand *cmd, int16_t result)
{
    sPackEvent ev;

    (void)memset(&ev, 0, sizeof(ev));
    ev.type    = packEvt_command;
    ev.tick_ms = (uint32_t)osKernelGetTickCount();
    ev.name    = s_inst[idx].name;
    ev.idx     = idx;
    ev.u.command.cmd    = (uint8_t)cmd->cmd;
    ev.u.command.value  = cmd->value;
    ev.u.command.result = result;
    Publish(&ev);
}

/** Finish an in-flight command: tell the caller, tell subscribers, count it.
 *  Runs on the shared task; the slot is already cleared by the FSM. */
/**
 * @brief  Snapshot the completion of the command that just ended.
 * @note   MUST BE CALLED WITH THE CORE LOCK HELD, in the same critical
 *         section that freed the slot.  See sPackInst.doneRec.
 */
static void TakeCommandRecord(uint8_t idx, ePackErr result)
{
    sPackInst *in = &s_inst[idx];

    if (in->doneRecValid != 0u) {
        /* Cannot happen: see the ordering argument on the mailbox.  Drop
         * rather than overwrite, so the earlier completion is still
         * delivered, and count it as the contract violation it would be. */
        s_stats.lateCompletes++;
        return;
    }

    in->doneRec.cmd    = in->pending;
    in->doneRec.done   = in->done;
    in->doneRec.ctx    = in->doneCtx;
    in->doneRec.result = result;
    copy_name(in->doneRec.origin, in->pendingOrigin,
              sizeof(in->doneRec.origin));

    /* Cleared HERE, so a command claimed before the dispatch runs installs
     * its own callback without disturbing this record. */
    in->done         = NULL;
    in->doneCtx      = NULL;
    in->doneRecValid = 1u;
}

/**
 * @brief  Deliver a snapshotted completion.
 * @note   FUNC TASK, OUTSIDE THE LOCK -- it calls a consumer callback and a
 *         subscriber, and neither may run with interrupts masked.
 */
static void FinishCommand(uint8_t idx)
{
    sPackCmdRecord rec;
    uint32_t       saved;

    saved = CoreLock();
    if (s_inst[idx].doneRecValid == 0u) {
        CoreUnlock(saved);
        return;
    }
    rec = s_inst[idx].doneRec;
    s_inst[idx].doneRecValid = 0u;
    CoreUnlock(saved);

    {
    const ePackErr result = rec.result;
    const sPackCommand cmd = rec.cmd;
    const fPackCmdDone done = rec.done;
    void *const ctx = rec.ctx;

    if (result == packErr_ok) {
        s_stats.cmdOk++;
    } else if (result == packErr_unknownOutcome) {
        s_stats.cmdUnknown++;
    } else {
        s_stats.cmdFailed++;
    }

    /* Trice encodes a non-`S` argument as a 32-bit value, so a %s among
     * numbers renders the POINTER.  The string needs its own TRiceS. */
    TRice("[Pack] cmd %u=%d on %u -> %d\n",
          (unsigned)cmd.cmd, (int)cmd.value, (unsigned)idx, (int)result);
    TRiceS("[Pack] cmd origin %s\n", rec.origin);

    if (done != NULL) {
        done(idx, &cmd, (int16_t)result, ctx);
    }
    RaiseCommand(idx, &cmd, (int16_t)result);
    }
}

/** Assemble the consumer-facing view.  The core owns everything the type does
 *  not: name, idx, condition, ages, staleness, capability confirmation, and
 *  the confidence CEILING — which is a minimum operation, never an
 *  assignment, so a type reporting low confidence keeps it (§10.6). */
static void BuildState(const sPackInst *in, uint8_t idx, sPackState *out,
                       uint32_t now)
{
    const uint16_t cap = PackFsm_ConfidenceCap_pm(&in->fsm, now);
    uint32_t       g;

    (void)memset(out, 0, sizeof(*out));

    out->caps          = in->caps;
    out->cmds          = in->cmds;
    out->nameplate_mAh = in->nameplate_mAh;
    out->idx           = idx;
    out->typeId        = in->typeId;
    out->cond          = in->fsm.cond;
    out->cellCount     = in->cellCount;
    copy_name(out->name, in->name, sizeof(out->name));

    for (g = 0u; g < (uint32_t)packGrp_last; g++) {
        out->age_ms[g] = PackFsm_AgeMs(&in->fsm, (ePackGroup)g, now);
    }

    out->voltage_mV       = in->live.voltage_mV;
    out->current_mA       = in->live.current_mA;
    out->remaining_mAh    = in->live.remaining_mAh;
    /* Substituting the nameplate silently made a learned capacity and a
     * plate number indistinguishable -- the very distinction the amp-hour
     * API exists to carry (§2 point 3).  Say which it is. */
    if (in->live.capacity_mAh != 0u) {
        out->capacity_mAh = in->live.capacity_mAh;
        out->flags       |= (uint32_t)packFlag_capacityLearnt;
    } else {
        out->capacity_mAh = in->nameplate_mAh;
    }
    /* REPORTED AS READ, whatever their age.  A limit is a SETTING: it changes
     * when somebody writes it, not with time, so blanking it because this
     * module's invented budget elapsed destroyed a value that was correct.
     * The safety property that motivated the blanking -- a cluster must not
     * sum headroom from a battery that left the bus -- belongs to `cond`,
     * which the consumer filters on before aggregating anything. */
    out->chargeLimit_mA        = in->live.chargeLimit_mA;
    out->dischargeLimit_mA     = in->live.dischargeLimit_mA;
    out->chargeVoltLimit_mV    = in->live.chargeVoltLimit_mV;
    out->dischargeVoltLimit_mV = in->live.dischargeVoltLimit_mV;
    out->alarms           = in->live.alarms;
    out->vendorAlarms[0]  = in->live.vendorAlarms[0];
    out->vendorAlarms[1]  = in->live.vendorAlarms[1];
    out->flags           |= in->live.flags;
    out->soc_pm           = in->live.soc_pm;
    out->soh_pm           = in->live.soh_pm;
    out->socConf_pm       = (in->live.socConf_pm < cap) ? in->live.socConf_pm
                                                        : cap;

    /* THE ESTIMATE REPLACES THE VENDOR'S ONLY ONCE IT HAS ANCHORED, and says
     * so through packFlag_socEstimated -- a flag that has existed since the
     * module was written and been produced by nothing until now.  Before the
     * first anchor the JK's number stands, because an unanchored coulomb
     * count is not an opinion, it is an accumulator. */
    {
        uint16_t      estConf = 0u;
        const int32_t est     = PackSoc_UnitGet_pm(&in->soc.unit, &estConf);

        out->socDrift_pm = (int16_t)in->soc.unit.driftResidual_pm;
        if (est >= 0) {
            out->soc_pm     = (uint16_t)est;
            out->socConf_pm = (estConf < cap) ? estConf : cap;
            out->flags     |= (uint32_t)packFlag_socEstimated;
        }
    }
    out->sohConf_pm       = (in->live.sohConf_pm < cap) ? in->live.sohConf_pm
                                                        : cap;
    out->cellMax_mV       = in->live.cellMax_mV;
    out->cellMin_mV       = in->live.cellMin_mV;
    out->cellMaxIdx       = in->live.cellMaxIdx;
    out->cellMinIdx       = in->live.cellMinIdx;
    out->tempMax_dC       = in->live.tempMax_dC;
    out->tempMin_dC       = in->live.tempMin_dC;
    out->chargeSwitch     = in->live.chargeSwitch;
    out->dischargeSwitch  = in->live.dischargeSwitch;
    out->why              = in->fsm.why;
}

/** Bind every configured instance to its registered type. */
/** Claim the shared configuration scratch.  Non-zero if it was free. */
static int ScratchClaim(void)
{
    uint32_t saved = CoreLock();
    int      got   = (s_cfgScratchBusy == 0u) ? 1 : 0;

    if (got != 0) {
        s_cfgScratchBusy = 1u;
    }
    CoreUnlock(saved);
    return got;
}

static void ScratchRelease(void)
{
    uint32_t saved = CoreLock();

    s_cfgScratchBusy = 0u;
    CoreUnlock(saved);
}

/** The operator's domain for one command, or NULL if the document gave none. */
static const sPackCmdBound *FindCfgBound(const sPackCfgEntry *e, ePackCmdId cmd)
{
    uint8_t i;

    for (i = 0u; i < e->boundCount; i++) {
        if (e->bounds[i].cmd == cmd) {
            return &e->bounds[i];
        }
    }
    return NULL;
}

static void BindAll(void)
{
    uint8_t  i;
    uint8_t  b;
    uint32_t saved;
    uint8_t  perType[packType_last];

    (void)memset(perType, 0, sizeof(perType));

    for (i = 0u; i < s_cfg.count; i++) {
        const sPackCfgEntry *e   = &s_cfg.pack[i];
        sPackInst           *in  = &s_inst[i];
        const sPackType     *ty;
        sPackBindInfo        info;
        sPackBindResult      res;
        ePackAbsentReason    why = packWhy_none;

        /* THE INSTANCE GOES OFF LINE FIRST, under the lock.  Every
         * concurrent accessor -- PackType_Publish from the modbus task or a
         * CAN ISR, Pack_Command and Pack_GetState from http/cli at priority
         * 24, all ABOVE this task -- gates on `used`, so clearing it is what
         * makes the rest of this iteration safe to do unlocked.  `used` is
         * then set LAST, once the binding is complete.
         *
         * ty->bind() may block (it walks a Modbus point catalogue, which is
         * flash I/O), so it must stay outside the lock -- which is precisely
         * why the flag, not the whole loop, is the thing being protected. */
        saved = CoreLock();
        (void)memset(in, 0, sizeof(*in));
        CoreUnlock(saved);

        in->typeId        = e->typeId;
        in->nameplate_mAh = e->nameplate_mAh;
        in->cellCount     = e->cellCount;
        copy_name(in->name, e->name, sizeof(in->name));

        PackFsm_Init(&in->fsm, e->staleAfter_ms, 0u);

        ty = (e->typeId < (uint8_t)packType_last) ? s_types[e->typeId] : NULL;
        if (ty == NULL) {
            /* Configured for a type this firmware does not carry. */
            PackFsm_SetBindReason(&in->fsm, packWhy_noType);
            s_stats.bindFailures++;
            saved = CoreLock();
            in->used = 1u;
            CoreUnlock(saved);
            continue;
        }

        /* A transport that cannot tell two of its packs apart says so here,
         * rather than letting a config declare three and having them
         * overwrite each other. */
        perType[e->typeId]++;
        if ((ty->maxInstances != 0u) && (perType[e->typeId] > ty->maxInstances)) {
            PackFsm_SetBindReason(&in->fsm, packWhy_typeUnavailable);
            s_stats.bindFailures++;
            saved = CoreLock();
            in->used = 1u;
            CoreUnlock(saved);
            continue;
        }

        (void)memset(&info, 0, sizeof(info));
        info.idx               = i;
        info.name              = in->name;
        info.bindKey           = e->bind;
        info.nameplate_mAh     = e->nameplate_mAh;
        info.staleAfter_ms     = e->staleAfter_ms;
        info.cmdAllow          = e->cmdAllow;
        info.cellCount         = e->cellCount;
        info.chemistry         = e->chemistry;

        (void)memset(&res, 0, sizeof(res));
        if (ty->bind(&info, &res) != packErr_ok) {
            /* A failed bind is a REPORTABLE STATE, never a boot failure. */
            why = (res.why != (uint8_t)packWhy_none)
                ? (ePackAbsentReason)res.why : packWhy_noBinding;
            PackFsm_SetBindReason(&in->fsm, why);
            s_stats.bindFailures++;
            saved = CoreLock();
            in->used = 1u;
            CoreUnlock(saved);
            continue;
        }

        saved = CoreLock();
        in->type       = ty;
        in->caps       = res.caps;
        /* "The type may narrow, never widen" is ENFORCED here rather than
         * merely stated — the same move the lock ban makes (§11). */
        in->cmds       = res.cmds & e->cmdAllow;

        /* NARROWEST OF THE TWO WINS, per command.  A command the operator
         * gave no domain for keeps the type's; a domain the operator gave
         * for a command the type does not bound is dropped with the command
         * itself, since cmds was already masked above. */
        in->boundCount = 0u;
        for (b = 0u; b < res.boundCount; b++) {
            const sPackCmdBound *cfgB;
            sPackCmdBound       *dst;

            if (in->boundCount >= (uint8_t)packCmd_last) {
                break;
            }
            if ((in->cmds & PACK_CMD_BIT(res.bounds[b].cmd)) == 0u) {
                continue;
            }
            dst  = &in->boundsOwn[in->boundCount];
            *dst = res.bounds[b];

            cfgB = FindCfgBound(e, res.bounds[b].cmd);
            if (cfgB != NULL) {
                if (cfgB->min_scaled > dst->min_scaled) {
                    dst->min_scaled = cfgB->min_scaled;
                }
                if (cfgB->max_scaled < dst->max_scaled) {
                    dst->max_scaled = cfgB->max_scaled;
                }
            }
            in->boundCount++;
        }
        in->bounds     = in->boundsOwn;
        in->bal.cellCount = e->cellCount;
        PackSoc_Init(&in->soc, e->nameplate_mAh);
        {
            uint8_t c;

            /* A cell's share of the nameplate is the starting guess; it is
             * replaced by a measurement the moment two anchors allow one. */
            const uint32_t perCell = (e->cellCount > 0u)
                                   ? (e->nameplate_mAh / e->cellCount)
                                   : e->nameplate_mAh;

            for (c = 0u; c < PACK_CELLS_MAX; c++) {
                PackSoc_UnitInit(&in->socCell[c], e->nameplate_mAh);
                (void)perCell;
            }
        }
        in->haveBalSnap = 0u;
        in->socRestSamples = 0u;
        in->fsm.caps   = res.caps;
        /* A type may qualify a SUCCESSFUL bind -- "resolved, but no plan
         * reads it".  Carrying that through is what makes the difference
         * between a silent battery and a silent plan visible. */
        PackFsm_SetBindReason(&in->fsm, (ePackAbsentReason)res.why);
        in->used       = 1u;          /* LAST: the instance is now live */
        CoreUnlock(saved);
    }
}

static void UnbindAll(void)
{
    uint8_t i;

    for (i = 0u; i < PACK_MAX; i++) {
        const sPackType *ty;
        uint32_t         saved;
        int              live;

        /* OFF LINE BEFORE TEARDOWN, for the same reason as BindAll: unbind()
         * may block, so what the lock protects is the flag that stops a
         * higher-priority publisher writing into an instance being dropped. */
        saved = CoreLock();
        live  = (s_inst[i].used != 0u) ? 1 : 0;
        ty    = s_inst[i].type;
        s_inst[i].used = 0u;
        CoreUnlock(saved);

        if ((live != 0) && (ty != NULL) && (ty->unbind != NULL)) {
            (void)ty->unbind(i);
        }

        saved = CoreLock();
        (void)memset(&s_inst[i], 0, sizeof(s_inst[i]));
        CoreUnlock(saved);
    }
}

/* Exported functions -------------------------------------------------------*/

/* --- lifecycle ---------------------------------------------------------- */

int Pack_Init(uint16_t evtIdBase, fFuncPost post)
{
    /* STATIC, not automatic: sPackCfgRecord is ~1072 B and the func task's
     * stack is 2048, and this frame is live across BindAll -> bind ->
     * resolve_dev (a further 224 B) -> walk_points.  Pack_Init runs once, on
     * one task, so a static costs nothing and buys back half the stack.
     * Pack_ConfigVerify/Apply use static scratch for the same reason. */
    sPackCfgRecord *const rec = &s_cfgScratch;

    (void)ScratchClaim();       /* boot: nothing else can hold it yet */

    s_evtBase = evtIdBase;
    s_post    = post;

    if (s_inited != 0u) {
        return packErr_ok;              /* idempotent                       */
    }
    s_inited = 1u;

    (void)memset(&s_stats, 0, sizeof(s_stats));
    (void)memset(&s_cfg, 0, sizeof(s_cfg));

    if (NvRecord_Load(nvdbUser_packCfg, PACK_CFG_MAGIC, PACK_CFG_VERSION,
                      rec, sizeof(*rec)) == 0) {
        s_cfg         = rec->cfg;
        s_provisioned = (uint8_t)((s_cfg.count > 0u) ? 1u : 0u);
    } else {
        /* Absent, truncated, wrong-version and corrupt all collapse to
         * UNPROVISIONED and say so, rather than misreading a binding (§14).
         * There is no built-in default: a pack configuration describes
         * hardware the board may not have. */
        s_provisioned = 0u;
    }
    s_stats.provisioned = s_provisioned;

    ScratchRelease();

    BindAll();
    StateRestore();

    TRice("[Pack] init: %u pack(s), provisioned=%u\n",
          (unsigned)s_cfg.count, (unsigned)s_provisioned);
    return packErr_ok;
}

void Pack_HandleEvent(uint16_t localEvt, void *arg)
{
    const uint8_t idx = (uint8_t)(uintptr_t)arg;
    uint32_t      now = (uint32_t)osKernelGetTickCount();

    if ((idx >= PACK_MAX) && (localEvt != (uint16_t)packInt_configChanged)) {
        return;
    }

    switch ((ePackIntEvt)localEvt) {
    case packInt_stateCommitted:
    case packInt_liveness: {
        ePackCondition from;
        ePackCondition to;
        uint32_t       groups;
        uint32_t       added;
        uint32_t       cleared;
        uint32_t       saved;
        int            chg;

        /* The commit already happened under the lock; this is the dispatch
         * half.  Evaluate here as well as on the tick so the two paths meet
         * at exactly one decision point — under the lock, because a push
         * type's ISR may be inside NotePublish on this same instance. */
        saved   = CoreLock();
        chg     = PackFsm_Evaluate(&s_inst[idx].fsm, now, &from, &to);
        groups  = s_inst[idx].pendingGroups;
        added   = s_inst[idx].alarmAdded;
        cleared = s_inst[idx].alarmCleared;
        s_inst[idx].pendingGroups = 0u;
        s_inst[idx].alarmAdded    = 0u;
        s_inst[idx].alarmCleared  = 0u;
        CoreUnlock(saved);

        /* A BARE LIVENESS REPORT IS NOT A STATE UPDATE.  Both paths landed
         * here and both raised packEvt_state, so "the pack answered" was
         * indistinguishable from "values refreshed" and a subscriber saw an
         * update event when nothing had changed. */
        if (groups != 0u) {
            RaiseState(idx, groups);
        }
        if ((added != 0u) || (cleared != 0u)) {
            RaiseAlarm(idx, added, cleared);
        }
        if (chg != 0) {
            RaiseCondition(idx, from, to);
        }
        break;
    }

    case packInt_cellsCommitted: {
        uint32_t saved  = CoreLock();
        uint32_t groups = s_inst[idx].pendingGroups;

        s_inst[idx].pendingGroups = 0u;
        CoreUnlock(saved);

        if (groups != 0u) {
            RaiseState(idx, groups);
        }
        break;
    }

    case packInt_commandDone:
        FinishCommand(idx);
        break;

    case packInt_commandSubmit: {
        sPackInst   *in = &s_inst[idx];
        sPackCommand cmd;
        uint32_t     timeout;
        uint32_t     seq;
        uint32_t     saved;
        int          go;

        /* RE-CHECK UNDER THE LOCK.  Between accepting this command and
         * dispatching it, a configuration change or an expiry may have ended
         * it -- and putting a write on a live battery after the core has
         * already told the consumer the outcome is exactly what §6 forbids.
         * Snapshot the operands too: reconstructing the timeout from the
         * CURRENT slot would use a re-claimed command's deadline. */
        saved   = CoreLock();
        go      = (in->slot.inFlight != 0u) ? 1 : 0;
        cmd     = in->pending;
        seq     = in->pendingSeq;
        timeout = in->slot.deadline_ms - in->slot.issued_ms;
        CoreUnlock(saved);

        if ((go != 0) && (in->type != NULL) && (in->type->submit != NULL)) {
            const ePackErr r = (ePackErr)in->type->submit(idx, &cmd, timeout);

            if (r != packErr_ok) {
                /* A synchronous refusal from the type: no completion will
                 * follow, so finish it here. */
                ePackErr out;
                int      done;

                saved = CoreLock();
                done  = PackFsm_CmdComplete(&in->slot, seq, r, &out);
                if (done != 0) {
                    TakeCommandRecord(idx, out);
                }
                CoreUnlock(saved);

                if (done != 0) {
                    FinishCommand(idx);
                }
            }
        }
        break;
    }

    case packInt_release: {
        /* A BORROW ENDS WHEN THE MODULE SAYS IT ENDED: one final call with
         * packEvt_released, regardless of evtMask. */
        uint8_t h;

        for (h = 0u; h < PACK_MAX_SUBS; h++) {
            if (s_subs[h].releasing != 0u) {
                sPackEvent ev;

                (void)memset(&ev, 0, sizeof(ev));
                ev.type    = packEvt_released;
                ev.tick_ms = now;
                if (s_subs[h].cb != NULL) {
                    s_subs[h].cb(&ev, s_subs[h].ctx);
                }
                (void)memset(&s_subs[h], 0, sizeof(s_subs[h]));
            }
        }
        break;
    }

    case packInt_configChanged:
    case packInt_rebind: {
        sPackEvent ev;
        uint8_t    i;

        /* COMPLETE OUTSTANDING COMMANDS FIRST.  UnbindAll() memsets each
         * instance, which erases `done`, `doneCtx` and `slot.inFlight` -- so
         * relying on the tick to expire them afterwards left the consumer's
         * fPackCmdDone never firing at all.  §10.9 rule 6 says `done` ALWAYS
         * fires for an accepted command, and rule 8 says a configuration
         * change completes it packErr_unknownOutcome; both were unimplemented
         * on the ordinary path because a queued event always drains before
         * the queue-timeout tick that would have caught it. */
        for (i = 0u; i < PACK_MAX; i++) {
            ePackErr result;
            uint32_t saved;
            int      expired;

            if (s_inst[i].used == 0u) {
                continue;
            }
            saved   = CoreLock();
            expired = PackFsm_CmdExpire(&s_inst[i].slot,
                                        (ePackCondition)s_inst[i].fsm.cond,
                                        1 /* configChanged */, now, &result);
            if (expired != 0) {
                TakeCommandRecord(i, result);
            }
            CoreUnlock(saved);

            if (expired != 0) {
                FinishCommand(i);
            }
        }

        UnbindAll();
        BindAll();
        StateRestore();

        (void)memset(&ev, 0, sizeof(ev));
        ev.type    = packEvt_config;
        ev.tick_ms = now;
        ev.name    = "";
        Publish(&ev);

        for (i = 0u; i < PACK_MAX; i++) {
            s_inst[i].configChanged = 0u;
        }
        break;
    }

    default:
        break;
    }
}

void Pack_Tick(uint32_t now_ms)
{
    uint8_t i;

    for (i = 0u; i < PACK_MAX; i++) {
        sPackInst     *in = &s_inst[i];
        ePackCondition from;
        ePackCondition to;
        ePackErr       result;

        if (in->used == 0u) {
            continue;
        }

        /* THE SLOT AND THE FSM ARE SHARED WITH PackType_CommandDone AND
         * PackType_Publish, which run on the modbus task (priority 24, ABOVE
         * this one) and from a CAN ISR.  Deciding here without the lock would
         * let a type's completion and this expiry both observe inFlight == 1
         * and both finish the same command — the consumer's fPackCmdDone
         * would fire twice for one write.
         *
         * Decide under the lock; DISPATCH outside it, because FinishCommand
         * and RaiseCondition call subscribers and a subscriber must never run
         * with interrupts masked. */
        uint32_t saved   = CoreLock();
        int      condChg = PackFsm_Evaluate(&in->fsm, now_ms, &from, &to);
        int      expired = PackFsm_CmdExpire(&in->slot,
                                             (ePackCondition)in->fsm.cond,
                                             (int)in->configChanged, now_ms,
                                             &result);
        if (expired != 0) {
            TakeCommandRecord(i, result);
        }
        CoreUnlock(saved);

        if (condChg != 0) {
            RaiseCondition(i, from, to);
        }
        if (expired != 0) {
            FinishCommand(i);
        }

        /* THE THIRD THING §13 SPECIFIES PER WAKE, and it was missing: a push
         * type has no other way to close a frame set that stopped arriving,
         * so a Pylontech pack would never have published a partial set at
         * all.  Outside the lock -- a tick may publish, and publishing takes
         * the lock itself. */
        if ((in->type != NULL) && (in->type->tick != NULL)) {
            in->type->tick(i, now_ms);
        }
    }

    /* OUTSIDE THE PER-INSTANCE LOOP: this is one module-wide record, and
     * inside the loop it was skipped entirely whenever no pack was bound
     * (the `used` check continues before reaching it).
     *
     * Flash I/O is legal here -- this is the func task -- and the quarter
     * hour cadence is what makes a lossy statistic cheap (§23.4). */
    if ((s_provisioned != 0u) &&
        ((uint32_t)(now_ms - s_lastStateSave_ms) >=
         (PACK_STATE_SAVE_SEC * 1000u))) {
        s_lastStateSave_ms = now_ms;
        StateSave();
    }
}

/* --- reading ------------------------------------------------------------ */

int Pack_Count(void)
{
    uint8_t i;
    int     n = 0;

    /* Instances, not configuration entries -- see Pack_FindByName.  These
     * two and Pack_GetState must agree about what exists, and only the
     * instance table is what the rest of the module operates on. */
    for (i = 0u; i < PACK_MAX; i++) {
        if (s_inst[i].used != 0u) {
            n++;
        }
    }
    return n;
}

int Pack_FindByName(const char *name)
{
    uint8_t i;

    if (name == NULL) {
        return packErr_badArg;
    }
    /* KEYED OFF THE INSTANCE TABLE, not s_cfg.  Pack_ConfigApply assigns
     * s_cfg from the http task while BindAll has not yet run on the func
     * task, so between the two this walked the NEW count over the OLD
     * instances and handed back an index Pack_GetState then rejected. */
    for (i = 0u; i < PACK_MAX; i++) {
        if ((s_inst[i].used != 0u) && (strcmp(s_inst[i].name, name) == 0)) {
            return (int)i;
        }
    }
    return packErr_notFound;
}

int Pack_GetState(uint8_t idx, sPackState *out)
{
    const uint32_t now = (uint32_t)osKernelGetTickCount();
    uint32_t       saved;

    if ((out == NULL) || (idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }

    /* COPIES, so there are no borrowed pointers and no lifetime rules.  The
     * ages are computed inside the same critical section as the values, so an
     * age can never disagree with what it describes. */
    saved = CoreLock();
    BuildState(&s_inst[idx], idx, out, now);
    CoreUnlock(saved);

    return packErr_ok;
}

int Pack_GetCells(uint8_t idx, sPackCells *out)
{
    const uint32_t wanted = (uint32_t)packCap_cellDetail |
                            (uint32_t)packCap_leadResistance |
                            (uint32_t)packCap_balancer;
    uint32_t saved;

    if ((out == NULL) || (idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }
    if ((s_inst[idx].caps & wanted) == 0u) {
        /* No capability here without an accessor behind it, and no accessor
         * answering for a capability the instance does not advertise. */
        return packErr_notSupported;
    }

    /* INSIDE the lock, like Pack_GetState -- pack.h promises an age can
     * never disagree with the value it describes, and computing it afterwards
     * with a second clock read let a PublishCells from an ISR land in
     * between, so the age described a different snapshot. */
    saved = CoreLock();
    *out = s_inst[idx].cellsLive;
    out->age_ms = PackFsm_AgeMs(&s_inst[idx].fsm, packGrp_cells,
                                (uint32_t)osKernelGetTickCount());
    CoreUnlock(saved);

    return packErr_ok;
}

/* --- subscribing -------------------------------------------------------- */

int Pack_Subscribe(uint32_t evtMask, fPackSubscriber cb, void *ctx)
{
    uint8_t  i;
    uint32_t saved;

    if (cb == NULL) {
        return packErr_badArg;
    }

    /* Does NOT post, deliberately: a posted subscribe cannot return "table
     * full", which is a real init-time error. */
    saved = CoreLock();
    for (i = 0u; i < PACK_MAX_SUBS; i++) {
        if (s_subs[i].inUse == 0u) {
            s_subs[i].cb        = cb;
            s_subs[i].ctx       = ctx;
            s_subs[i].evtMask   = evtMask;
            s_subs[i].releasing = 0u;
            s_subs[i].inUse     = 1u;   /* written LAST                     */
            CoreUnlock(saved);
            return (int)i;
        }
    }
    CoreUnlock(saved);
    return packErr_full;
}

int Pack_Unsubscribe(int handle)
{
    if ((handle < 0) || (handle >= (int)PACK_MAX_SUBS)) {
        return packErr_badArg;
    }
    if (s_subs[handle].inUse == 0u) {
        return packErr_notFound;
    }

    /* Posts and returns; legal from inside a callback.  The release point is
     * the final packEvt_released call, made on the task. */
    s_subs[handle].releasing = 1u;
    PostInt(packInt_release, 0u);
    return packErr_ok;
}

/* --- commanding --------------------------------------------------------- */

int Pack_Command(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms,
                 fPackCmdDone done, void *ctx)
{
    sPackInst  *in;
    sPackCmdCtx cctx;
    ePackErr    err;
    uint32_t    saved;
    uint32_t    seq = 0u;

    if ((idx >= PACK_MAX) || (cmd == NULL) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }
    in = &s_inst[idx];

    /* VALIDATE AND CLAIM IN ONE CRITICAL SECTION.  Reading the binding
     * outside it and claiming inside left a window in which a rebind on the
     * func task replaced `cmds`/`bounds` between the check and the claim, so
     * the command was validated against one binding and issued against
     * another; the same window let a pack go stale after `cond` was read.
     * PackFsm_ValidateCommand is pure and walks at most packCmd_last bounds,
     * so it is cheap enough to hold the lock across. */
    saved = CoreLock();

    (void)memset(&cctx, 0, sizeof(cctx));
    cctx.cmds        = in->cmds;
    cctx.bounds      = in->bounds;
    cctx.boundCount  = in->boundCount;
    cctx.cond        = in->fsm.cond;
    cctx.inFlight    = in->slot.inFlight;
    cctx.provisioned = s_provisioned;

    err = PackFsm_ValidateCommand(&cctx, cmd, timeout_ms);
    if (err == packErr_ok) {
        err = PackFsm_CmdClaim(&in->slot, cmd, timeout_ms,
                               (uint32_t)osKernelGetTickCount(), &seq);
    }
    if (err == packErr_ok) {
        /* The command is COPIED: `origin` is borrowed for this call only, so
         * nothing the caller owns survives past the return. */
        in->pending    = *cmd;
        in->pending.origin = NULL;
        in->pendingSeq = seq;
        in->done       = done;
        in->doneCtx    = ctx;
        copy_name(in->pendingOrigin,
                  (cmd->origin != NULL) ? cmd->origin : "?",
                  sizeof(in->pendingOrigin));
    }
    CoreUnlock(saved);

    /* Counted INSIDE the same lock as the decision: http and cli both reach
     * here at priority 24 and a plain ++ across them loses increments. */
    saved = CoreLock();
    if (err != packErr_ok) {
        s_stats.cmdRefused++;
    } else {
        s_stats.cmdAccepted++;
    }
    CoreUnlock(saved);

    if (err != packErr_ok) {
        return (int)err;
    }

    PostInt(packInt_commandSubmit, idx);
    return packErr_ok;
}

int Pack_CommandBounds(uint8_t idx, ePackCmdId cmd, int32_t *min, int32_t *max)
{
    const sPackCmdBound *b;

    if ((idx >= PACK_MAX) || (min == NULL) || (max == NULL) ||
        (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }
    /* PACK_CMD_BIT is `1u << cmd`, and this is reached straight from the HTTP
     * and CLI surfaces with an operator-supplied id -- a shift by >= 32 is
     * undefined.  PackFsm_ValidateCommand already range-checks; this one did
     * not. */
    if ((cmd == packCmd_undefined) || ((uint32_t)cmd >= (uint32_t)packCmd_last)) {
        return packErr_badArg;
    }
    if ((s_inst[idx].cmds & PACK_CMD_BIT(cmd)) == 0u) {
        return packErr_notSupported;
    }

    b = PackFsm_FindBound(s_inst[idx].bounds, s_inst[idx].boundCount, cmd);
    if (b == NULL) {
        return packErr_notSupported;
    }
    *min = b->min_scaled;
    *max = b->max_scaled;
    return packErr_ok;
}

/* --- configuration ------------------------------------------------------ */

int Pack_ConfigVerify(fPackByteSource src, void *srcCtx, sPackCfgResult *res)
{
    int r;

    if (ScratchClaim() == 0) {
        return packErr_busy;
    }
    /* SAME parser, SAME pass, SAME result struct — there is never a second
     * validator that can disagree with the first (§10.10). */
    r = PackCfg_Parse(src, srcCtx, &s_cfgScratch.cfg, res);
    ScratchRelease();
    return r;
}

int Pack_ConfigApply(fPackByteSource src, void *srcCtx, sPackCfgResult *res)
{
    sPackCfgRecord *const rec    = &s_cfgScratch;
    sPackCfg       *const parsed = &s_cfgScratch.cfg;
    int                   r;
    uint8_t               i;

    if (ScratchClaim() == 0) {
        return packErr_busy;
    }

    r = PackCfg_Parse(src, srcCtx, parsed, res);
    if (r != packErr_ok) {
        ScratchRelease();
        return r;
    }

    /* `parsed` IS &rec->cfg -- the document was parsed straight into the
     * record's payload, so only the header is cleared here.  Zeroing the
     * whole record would erase what was just parsed. */
    (void)memset(&rec->hdr, 0, sizeof(rec->hdr));
    if (NvRecord_Save(nvdbUser_packCfg, PACK_CFG_MAGIC, PACK_CFG_VERSION,
                      rec, sizeof(*rec)) != 0) {
        ScratchRelease();
        return packErr_transport;
    }

    s_cfg         = *parsed;
    ScratchRelease();

    /* THE TOTALS ARE KEYED BY INDEX, so a new configuration must discard
     * them: reordering the pack list would otherwise hand pack 0 a different
     * battery's weeks of accumulated balance transfer, which is worse than
     * starting over because it looks plausible. */
    (void)NvRecord_Forget(nvdbUser_packState);
    s_provisioned = (uint8_t)((s_cfg.count > 0u) ? 1u : 0u);
    s_stats.provisioned = s_provisioned;

    /* Outstanding commands are neither dropped nor assumed to have landed:
     * the binding they were issued against no longer exists (§10.9 rule 8). */
    for (i = 0u; i < PACK_MAX; i++) {
        s_inst[i].configChanged = 1u;
    }
    PostInt(packInt_configChanged, 0u);
    return packErr_ok;
}

int Pack_ConfigExport(fPackByteSink sink, void *ctx)
{
    if (s_provisioned == 0u) {
        return packErr_unprovisioned;
    }
    return PackCfg_Serialize(&s_cfg, sink, ctx);
}

int Pack_ConfigErase(void)
{
    uint8_t i;

    /* Erase, not Reset: with no built-in default there is nothing to reset
     * TO.  NvRecord_Forget clears the magic synchronously, so "forgotten" is
     * true when this returns. */
    (void)NvRecord_Forget(nvdbUser_packCfg);

    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    s_provisioned       = 0u;
    s_stats.provisioned = 0u;

    for (i = 0u; i < PACK_MAX; i++) {
        s_inst[i].configChanged = 1u;
    }
    PostInt(packInt_configChanged, 0u);
    return packErr_ok;
}

/* --- diagnostics -------------------------------------------------------- */

const char *Pack_TypeName(uint8_t typeId)
{
    return PackCfg_TypeName(typeId);
}

int Pack_CmdIdFromName(const char *name, ePackCmdId *out)
{
    return PackCfg_CmdIdFromName(name, out);
}

const char *Pack_CmdName(ePackCmdId cmd)
{
    return PackCfg_CmdName(cmd);
}

int Pack_StatCount(uint8_t idx)
{
    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }
    return (int)s_inst[idx].statCount;
}

int Pack_StatGet(uint8_t idx, uint8_t n, sPackStat *out)
{
    uint32_t saved;
    int      r;

    if ((idx >= PACK_MAX) || (out == NULL) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }

    saved = CoreLock();
    if (n < s_inst[idx].statCount) {
        *out = s_inst[idx].stats[n];
        r    = packErr_ok;
    } else {
        r = packErr_notFound;
    }
    CoreUnlock(saved);
    return r;
}

int Pack_BalanceStats(uint8_t idx, sPackBalanceStats *out)
{
    uint32_t saved;

    if ((idx >= PACK_MAX) || (out == NULL) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }
    if ((s_inst[idx].caps & (uint32_t)packCap_balancer) == 0u) {
        /* No accessor without a capability behind it, and no capability
         * answered for that the instance does not advertise (§10.2). */
        return packErr_notSupported;
    }

    saved = CoreLock();
    *out = s_inst[idx].bal;

    /* THE DERIVED FIGURE, computed here rather than stored, so it can never
     * disagree with the totals it comes from.
     *
     * A cell the balancer keeps having to CHARGE is below the pack in
     * capacity -- hence the sign flip.  Relative to the pack, not absolute:
     * the reference is the pack's own median transfer, so a pack whose
     * balancer favours one end uniformly does not read as every cell being
     * bad. */
    PackFsm_BalanceDerive(out, out->capacityDelta_mAh);
    CoreUnlock(saved);
    return packErr_ok;
}

int Pack_GetCellEstimate(uint8_t idx, sPackCellEstimate *out)
{
    uint32_t saved;
    uint8_t  c;

    if ((idx >= PACK_MAX) || (out == NULL) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }
    if ((s_inst[idx].caps & (uint32_t)packCap_cellDetail) == 0u) {
        return packErr_notSupported;
    }

    (void)memset(out, 0, sizeof(*out));

    saved = CoreLock();
    {
        const sPackInst *in = &s_inst[idx];
        int32_t          cap = 0;

        for (c = 0u; c < PACK_CELLS_MAX; c++) {
            out->soc_pm[c]       = (int16_t)in->socCell[c].soc_pm;
            out->capacity_mAh[c] = in->socCell[c].capacity_mAh;
            out->capConf_pm[c]   = in->socCell[c].capConf_pm;
            if (in->socCell[c].capacityLearned != 0u) {
                out->measuredCount++;
            }
        }
        out->cellCount  = in->cellCount;
        out->weakestIdx = (int8_t)PackSoc_WeakestUnit(in->socCell,
                                                      in->cellCount, &cap);
    }
    CoreUnlock(saved);
    return packErr_ok;
}

int Pack_BalanceReset(uint8_t idx)
{
    uint32_t saved;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return packErr_badArg;
    }

    saved = CoreLock();
    {
        const uint8_t keep = s_inst[idx].bal.cellCount;

        (void)memset(&s_inst[idx].bal, 0, sizeof(s_inst[idx].bal));
        s_inst[idx].bal.cellCount = keep;
        s_inst[idx].balSeen       = 0u;   /* re-establish the interval */
    }
    CoreUnlock(saved);
    TRice("[Pack] balance totals reset on %u\n", (unsigned)idx);
    return packErr_ok;
}

/** Write the accumulation back.  FUNC TASK ONLY -- it does flash I/O. */
static void StateSave(void)
{
    static sPackStateRecord rec;     /* static: ~1 KB, and the func task
                                        stack is 2 KB */
    uint8_t  i;
    uint32_t saved;

    (void)memset(&rec.pack, 0, sizeof(rec.pack));

    saved = CoreLock();
    for (i = 0u; i < PACK_MAX; i++) {
        const sPackInst *in = &s_inst[i];

        if (in->used == 0u) {
            continue;
        }
        (void)memcpy(rec.pack[i].in_mAs,  in->bal.in_mAs,
                     sizeof(rec.pack[i].in_mAs));
        (void)memcpy(rec.pack[i].out_mAs, in->bal.out_mAs,
                     sizeof(rec.pack[i].out_mAs));
        rec.pack[i].window_sec       = in->bal.window_sec;
        rec.pack[i].activeSamples    = in->bal.activeSamples;
        rec.pack[i].cellCount        = in->bal.cellCount;
        rec.pack[i].soc_pm           = in->soc.unit.soc_pm;
        rec.pack[i].driftResidual_pm = in->soc.unit.driftResidual_pm;
        rec.pack[i].socConf_pm       = in->soc.unit.conf_pm;
        rec.pack[i].socSeeded        = in->soc.unit.haveAnchor;
        rec.pack[i].packCap_mAh      = in->soc.unit.capacity_mAh;
        rec.pack[i].packCapConf_pm   = in->soc.unit.capConf_pm;
        rec.pack[i].packCapLearned   = in->soc.unit.capacityLearned;
        {
            uint8_t c;

            for (c = 0u; c < PACK_CELLS_MAX; c++) {
                /* ONLY A MEASURED capacity is persisted.  A unit is born
                 * holding the nameplate, so saving capacity_mAh
                 * unconditionally and restoring "> 0 means learned" turned
                 * the starting guess into 16 fabricated measurements on the
                 * next boot -- the board reported measuredCells=16 with every
                 * value exactly the nameplate. */
                rec.pack[i].cellCap_mAh[c]    =
                    (in->socCell[c].capacityLearned != 0u)
                        ? in->socCell[c].capacity_mAh : 0;
                rec.pack[i].cellCapConf_pm[c] = in->socCell[c].capConf_pm;
            }
        }
    }
    CoreUnlock(saved);

    (void)NvRecord_Save(nvdbUser_packState, PACK_STATE_MAGIC,
                        PACK_STATE_VERSION, &rec, sizeof(rec));
}

/** Restore one instance's accumulation after a bind.  FUNC TASK ONLY.
 *
 *  KEYED BY INDEX, which is only sound because a configuration change wipes
 *  this record -- see Pack_ConfigApply.  Without that, pack 0 would inherit a
 *  different battery's totals the moment the operator reordered the list. */
static void StateRestore(void)
{
    static sPackStateRecord rec;
    uint8_t  i;
    uint32_t saved;

    if (NvRecord_Load(nvdbUser_packState, PACK_STATE_MAGIC,
                      PACK_STATE_VERSION, &rec, sizeof(rec)) != 0) {
        return;                     /* absent or stale: start clean */
    }

    saved = CoreLock();
    for (i = 0u; i < PACK_MAX; i++) {
        sPackInst *in = &s_inst[i];

        if ((in->used == 0u) ||
            (rec.pack[i].cellCount != in->bal.cellCount)) {
            continue;               /* different shape: do not adopt it */
        }
        (void)memcpy(in->bal.in_mAs,  rec.pack[i].in_mAs,
                     sizeof(in->bal.in_mAs));
        (void)memcpy(in->bal.out_mAs, rec.pack[i].out_mAs,
                     sizeof(in->bal.out_mAs));
        in->bal.window_sec    = rec.pack[i].window_sec;
        in->bal.activeSamples = rec.pack[i].activeSamples;

        if (rec.pack[i].socSeeded != 0u) {
            in->soc.unit.soc_pm           = rec.pack[i].soc_pm;
            in->soc.unit.anchorSoc_pm     = rec.pack[i].soc_pm;
            in->soc.unit.driftResidual_pm = rec.pack[i].driftResidual_pm;
            in->soc.unit.conf_pm          = rec.pack[i].socConf_pm;
            in->soc.unit.haveAnchor       = 1u;
        }
        if (rec.pack[i].packCapLearned != 0u) {
            in->soc.unit.capacity_mAh    = rec.pack[i].packCap_mAh;
            in->soc.unit.capConf_pm      = rec.pack[i].packCapConf_pm;
            in->soc.unit.capacityLearned = 1u;
        }
        {
            uint8_t c;

            for (c = 0u; c < PACK_CELLS_MAX; c++) {
                if (rec.pack[i].cellCap_mAh[c] > 0) {
                    in->socCell[c].capacity_mAh    = rec.pack[i].cellCap_mAh[c];
                    in->socCell[c].capConf_pm      =
                        rec.pack[i].cellCapConf_pm[c];
                    in->socCell[c].capacityLearned = 1u;
                }
            }
        }
    }
    CoreUnlock(saved);
    TRice("[Pack] state restored from flash\n");
}

int Pack_Stats(sPackStats *out)
{
    if (out == NULL) {
        return packErr_badArg;
    }
    *out = s_stats;
    return packErr_ok;
}

void Pack_LogStatus(void)
{
    uint8_t i;

    TRice("[Pack] %u pack(s), prov=%u, upd=%u cmd=%u/%u unk=%u stale=%u late=%u\n",
          (unsigned)s_cfg.count, (unsigned)s_provisioned,
          (unsigned)s_stats.updates, (unsigned)s_stats.cmdOk,
          (unsigned)s_stats.cmdAccepted, (unsigned)s_stats.cmdUnknown,
          (unsigned)s_stats.staleEvents, (unsigned)s_stats.lateCompletes);

    for (i = 0u; i < s_cfg.count; i++) {
        char line[96];

        (void)snprintf(line, sizeof(line),
                       "%s type=%s cond=%u why=%u caps=%08lx cmds=%02lx",
                       s_inst[i].name, PackCfg_TypeName(s_inst[i].typeId),
                       (unsigned)s_inst[i].fsm.cond,
                       (unsigned)s_inst[i].fsm.why,
                       (unsigned long)s_inst[i].caps,
                       (unsigned long)s_inst[i].cmds);
        TRiceS("[Pack]   %s\n", line);
    }
}

/* --- the TYPE side (pack_type.h) ---------------------------------------- */

int PackType_Register(const sPackType *type)
{
    if ((type == NULL) || (type->id >= (uint8_t)packType_last) ||
        (type->bind == NULL)) {
        return packErr_badArg;
    }
    s_types[type->id] = type;
    return packErr_ok;
}

sPackRaw *PackType_Staging(uint8_t idx)
{
    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return NULL;
    }
    return &s_inst[idx].staging;
}

sPackCells *PackType_CellStaging(uint8_t idx)
{
    const uint32_t wanted = (uint32_t)packCap_cellDetail |
                            (uint32_t)packCap_leadResistance |
                            (uint32_t)packCap_balancer |
                            (uint32_t)packCap_cellSummary;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u) ||
        ((s_inst[idx].caps & wanted) == 0u)) {
        return NULL;
    }
    return &s_inst[idx].cellsStaging;
}


void PackType_Publish(uint8_t idx, uint32_t groups)
{
    uint32_t saved;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u) || (groups == 0u)) {
        return;
    }

    /* Commit under the lock, THEN announce — so a subscriber calling
     * Pack_GetState from inside its callback is guaranteed to see at least
     * the update the event announced. */
    saved = CoreLock();
    {
        /* ALARMS ARE DIFFED AT THE COMMIT, the one moment both the old and
         * the new set are in hand.  Nothing did this, so packEvt_alarm was
         * declared, decoded and never raised -- a subscriber watching for a
         * protection to open (§4's middle case, the ONE kind of disconnection
         * a pack can detect itself) heard nothing at all. */
        const uint32_t oldAl = s_inst[idx].live.alarms;
        const uint32_t newAl = s_inst[idx].staging.alarms;

        s_inst[idx].alarmAdded   |= (newAl & ~oldAl);
        s_inst[idx].alarmCleared |= (oldAl & ~newAl);
    }
    s_inst[idx].live = s_inst[idx].staging;
    s_inst[idx].pendingGroups |= groups;
    SocOnCommit(idx, groups);
    (void)PackFsm_NotePublish(&s_inst[idx].fsm, groups,
                              (uint32_t)osKernelGetTickCount());
    s_stats.updates++;
    CoreUnlock(saved);

    PostInt(packInt_stateCommitted, idx);
}

void PackType_PublishCells(uint8_t idx)
{
    uint32_t saved;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return;
    }

    saved = CoreLock();
    s_inst[idx].cellsLive = s_inst[idx].cellsStaging;
    s_inst[idx].pendingGroups |= PACK_GRP_BIT(packGrp_cells);
    (void)PackFsm_NotePublish(&s_inst[idx].fsm, PACK_GRP_BIT(packGrp_cells),
                              (uint32_t)osKernelGetTickCount());
    CoreUnlock(saved);

    PostInt(packInt_cellsCommitted, idx);
}

void PackType_CommandDone(uint8_t idx, ePackErr result)
{
    ePackErr deliver;
    uint32_t saved;
    int      live;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return;
    }

    saved = CoreLock();
    live = PackFsm_CmdComplete(&s_inst[idx].slot, s_inst[idx].pendingSeq,
                               result, &deliver);
    if (live != 0) {
        TakeCommandRecord(idx, deliver);
    } else {
        s_stats.lateCompletes++;
    }
    CoreUnlock(saved);

    if (live != 0) {
        PostInt(packInt_commandDone, idx);
    }
}

/**
 * @brief  Feed the SOC estimator from a fresh commit.  CALLED UNDER THE LOCK.
 *
 * Two independent inputs, on their own cadences:
 *   - the vendor's mAh counter advances the coulomb count (§23.2);
 *   - a NEAR-REST cell voltage folds into the accumulating anchor, weighted
 *     by the square of the local OCV slope.
 *
 * THERE IS NO HARD GATE on the second.  A plateau sample is admitted at
 * weight ~1 against a knee sample's ~400 and moves nothing; a hard 4 mV/%
 * cut, run against 690 real samples from this pack, admitted NOTHING AT ALL
 * (§24.3).
 */
static void SocOnCommit(uint8_t idx, uint32_t groups)
{
    sPackInst     *in  = &s_inst[idx];
    const uint32_t cap = (in->soc.unit.capacity_mAh > 0) ? (uint32_t)in->soc.unit.capacity_mAh
                                                     : in->nameplate_mAh;
    int32_t        restLimit_mA;
    int32_t        busyLimit_mA;

    if (cap == 0u) {
        return;
    }

    /* The vendor's counter: one step per commit that refreshed it. */
    if ((groups & PACK_GRP_BIT(packGrp_charge)) != 0u) {
        /* Largest believable change between two polls.  Generous -- this is
         * catching re-seeds and clamp steps, not policing current. */
        int32_t stepQ = 0;

        if (PackSoc_NoteCounter(&in->soc, (int32_t)in->live.remaining_mAh,
                                cap / 8u, &stepQ) != 0) {
            uint8_t c;

            /* THE PACK: the string charge, unmodified. */
            PackSoc_UnitAdvance(&in->soc.unit, stepQ);

            /* EACH CELL: the string charge PLUS what the balancer moved into
             * or out of that particular cell since the previous step.
             *
             * THE CORRECTION HAPPENS HERE, BEFORE THE HAND-OFF, which is why
             * the same PackSoc_UnitAdvance serves both: the estimator is
             * handed "charge through this unit" and never learns that a
             * balancer exists. */
            for (c = 0u; c < in->cellCount; c++) {
                const int32_t net = in->bal.in_mAs[c] - in->bal.out_mAs[c];
                const int32_t dBal_mAh =
                    (in->haveBalSnap != 0u)
                        ? ((net - in->socBalSnap_mAs[c]) / 3600) : 0;

                in->socBalSnap_mAs[c] = net;
                PackSoc_UnitAdvance(&in->socCell[c], stepQ + dBal_mAh);
            }
            in->haveBalSnap = 1u;
        }
    }

    /* THE GATE IS THE WHOLE DESIGN (§5.3 of the estimation doc): an
     * estimator that admits bad samples converges CONFIDENTLY to a wrong
     * answer, which is worse than not converging.  Each of these excludes a
     * sample whose voltage is not the cell's OCV. */

    /* 1. Disturbed?  Above C/20 the pack is being worked; note the time and
     *    start the relaxation clock. */
    busyLimit_mA = (int32_t)(cap / PACK_SOC_BUSY_C_DIV);
    if ((in->live.current_mA > busyLimit_mA) ||
        (in->live.current_mA < -busyLimit_mA)) {
        in->socDisturbed_ms = (uint32_t)osKernelGetTickCount();
        in->socHaveDisturb  = 1u;
        return;
    }

    /* 2. Relaxed?  LFP polarisation decays slowly, so a low-current sample
     *    taken shortly after a heavy one still reads high.  THIS IS THE
     *    CHECK THAT WAS MISSING: without it a 30 A charge ending one second
     *    ago counted as rest, biasing every anchor upward. */
    if (in->socHaveDisturb != 0u) {
        const uint32_t since =
            (uint32_t)((uint32_t)osKernelGetTickCount() - in->socDisturbed_ms);

        if (since < PACK_SOC_RELAX_MS) {
            return;
        }
    }

    /* 3. Near rest?  Below C/50 the IR term is small enough to ignore, which
     *    matters because correcting it needs a per-cell resistance this
     *    board does not always carry. */
    restLimit_mA = (int32_t)(cap / PACK_SOC_REST_C_DIV);
    if ((in->live.current_mA > restLimit_mA) ||
        (in->live.current_mA < -restLimit_mA)) {
        return;
    }

    /* 4. In the temperature band the OCV table describes. */
    if ((in->live.tempMin_dC < PACK_SOC_TEMP_MIN_dC) ||
        (in->live.tempMax_dC > PACK_SOC_TEMP_MAX_dC)) {
        return;
    }

    /* 5. Not in protection.  A pack with an active alarm is not in a normal
     *    operating state and its voltages do not mean what they usually do. */
    if (in->live.alarms != 0u) {
        return;
    }

    if (in->live.voltage_mV == 0u) {
        return;
    }

    {
        /* Pack-level SOC uses the MEAN cell, which is what the pack voltage
         * already is once divided.  Per-cell SOC needs the cell array and is
         * the next step, not this one. */
        const uint8_t  n = (in->cellCount > 0u) ? in->cellCount : 16u;
        const uint16_t meanCell_mV = (uint16_t)(in->live.voltage_mV / n);

        PackSoc_UnitAnchorAdd(&in->soc.unit, meanCell_mV);

        /* PER CELL, from the same gated sample.  Its own voltage, its own
         * anchor -- which is what eventually makes C_i a per-cell number
         * rather than a pack number divided by 16. */
        {
            uint8_t c;

            for (c = 0u; (c < n) && (c < PACK_CELLS_MAX); c++) {
                if (in->cellsLive.cell_mV[c] != 0u) {
                    PackSoc_UnitAnchorAdd(&in->socCell[c],
                                          in->cellsLive.cell_mV[c]);
                }
            }
        }
        in->socRestSamples++;
    }

    /* Resolve on a schedule rather than per sample: the anchor's value is in
     * the averaging, and sqrt(N) is what turns a +/-1.5 % plateau reading
     * into something usable. */
    if ((in->socRestSamples % PACK_SOC_ANCHOR_EVERY) == 0u) {
        int32_t  soc;
        uint32_t sigma;

        /* ONE RESOLVE, applied to the pack unit and to every cell unit --
         * the same function, because each already carries its own charge.
         * Capacity falls out of the same call for both. */
        if (PackSoc_UnitAnchorResolve(&in->soc.unit, &soc, &sigma) != 0) {
            uint8_t c;

            (void)PackSoc_UnitApplyAnchor(&in->soc.unit, soc, sigma,
                                          in->nameplate_mAh);

            for (c = 0u; c < in->cellCount; c++) {
                int32_t  cSoc  = 0;
                uint32_t cSig  = 0;

                if (PackSoc_UnitAnchorResolve(&in->socCell[c], &cSoc,
                                              &cSig) != 0) {
                    (void)PackSoc_UnitApplyAnchor(&in->socCell[c], cSoc, cSig,
                                                  in->nameplate_mAh);
                }
            }
        }
    }
}

void PackType_PublishStats(uint8_t idx, const sPackStat *stats, uint8_t count)
{
    uint32_t saved;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u) || (stats == NULL)) {
        return;
    }
    if (count > (uint8_t)PACK_STATS_MAX) {
        count = (uint8_t)PACK_STATS_MAX;   /* truncate, never refuse */
    }

    /* Whole list at once: a reader must never see half an update. */
    saved = CoreLock();
    (void)memcpy(s_inst[idx].stats, stats, (size_t)count * sizeof(stats[0]));
    s_inst[idx].statCount = count;
    CoreUnlock(saved);
}

/**
 * THE CORE OWNS THE INTEGRATION, not the type.  Every balancer needs the same
 * arithmetic -- interval, mAs accumulation, per-cell attribution, the reset
 * window -- and a copy of it in each type is a copy that can drift.
 */
void PackType_NoteBalance(uint8_t idx, int active, int32_t current_mA,
                          uint8_t srcIdx, uint8_t sinkIdx, uint32_t now_ms)
{
    uint32_t saved;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return;
    }
    if (current_mA < 0) {
        current_mA = -current_mA;       /* magnitude; direction is src/sink */
    }

    saved = CoreLock();
    {
        sPackInst *in = &s_inst[idx];

        if (in->balSeen == 0u) {
            /* First report establishes the interval baseline and the window;
             * nothing is integrated over an interval we did not observe. */
            in->balSeen     = 1u;
            in->balStart_ms = now_ms;
            in->balLast_ms  = now_ms;
        } else {
            const uint32_t dt_ms = (uint32_t)(now_ms - in->balLast_ms);

            in->balLast_ms = now_ms;

            /* The RULES live in pack_fsm.c, where they are pure and host
             * tested; this function owns only the lock and the clock. */
            (void)PackFsm_BalanceAccumulate(&in->bal, active, current_mA,
                                            dt_ms, PACK_BAL_MAX_GAP_MS,
                                            srcIdx, sinkIdx);
        }
        in->bal.window_sec = (uint32_t)(now_ms - in->balStart_ms) / 1000u;
    }
    CoreUnlock(saved);
}

void PackType_RequestRebind(void)
{
    /* PostInt coalesces naturally: the func task re-binds from the current
     * configuration, so several requests arriving before it drains cost one
     * rebind, not one each.  idx is meaningless here -- the rebind is
     * module-wide, because a transport reconfiguration is. */
    PostInt(packInt_rebind, 0u);
}

void PackType_NoteLiveness(uint8_t idx, int answered)
{
    uint32_t saved;

    if ((idx >= PACK_MAX) || (s_inst[idx].used == 0u)) {
        return;
    }

    saved = CoreLock();
    PackFsm_NoteLiveness(&s_inst[idx].fsm, answered,
                         (uint32_t)osKernelGetTickCount());
    CoreUnlock(saved);

    if (answered != 0) {
        PostInt(packInt_liveness, idx);
    }
}

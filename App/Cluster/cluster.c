/*
 * cluster.c
 *
 * The battery cluster module's ONE impure file: the tick, the double-buffered
 * publish, nvDb persistence, the counters and the Trice status line
 * (docs/design_battery_cluster.md §2).
 *
 * NO LOCK AND NO CRITICAL SECTION ANYWHERE IN THIS MODULE, and that is what
 * makes Cluster_GetOutput legal from the RTOS timer task rather than merely
 * "probably fine".  The published snapshot is a two-slot buffer with a
 * generation counter; a configuration apply is STAGED and adopted by the
 * tick.  __DMB() is a barrier, not a lock, and is what the constraint
 * explicitly permits — without saying so, "no lock" reads as "no
 * synchronisation of any kind", which is how a plain s_gen++ between two
 * struct writes survives every debug build and fails once at -O2.
 *
 * THE CLUSTER CACHES NO PACK INDEX.  Every member is re-resolved by name each
 * tick (at most 64 short strcmp at 4 Hz), which trades a couple of
 * microseconds for the removal of an entire class of stale-index bug.  What
 * that does NOT remove is the racing-resolution class, so one
 * Pack_Subscribe(packEvt_config) slot bumps an epoch and the tick discards
 * arithmetic done across a pack-table rebuild.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Cluster/cluster.h"
#include "App/Cluster/cluster_calc.h"
#include "App/Cluster/cluster_cfg.h"
#include "App/Pack/pack.h"
#include "App/nv_record.h"

#include "cmsis_os.h"

#include "trice.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Private defines ----------------------------------------------------------*/

#define CLUSTER_CFG_MAGIC   0x434C5543u     /* "CLUC"                        */

/** Four attempts, then cluErr_busy.  During an OTA the CPU pins at 991
 *  per-mille and a >500 ms starvation of a priority-2 reader is expected — a
 *  frame source losing BMS frames while the board installs firmware it is
 *  about to reboot into is the CORRECT outcome, and "reuse the last frame"
 *  is not. */
#define CLUSTER_GET_TRIES   4u

/* Private types ------------------------------------------------------------*/

/** The persisted record.  Header FIRST, as App/nv_record.h requires. */
typedef struct {
    sNvRecordHdr hdr;
    sClusterCfg  cfg;
} sClusterCfgRecord;

/* Private variables --------------------------------------------------------*/

static sClusterCfg        s_cfg;
/* THE MEDIUM'S VIEW *AND* THE STAGING SLOT, deliberately one object.  It
 * cannot be a stack local (NvDb_Read/Write want a DMA-reachable buffer, and a
 * FreeRTOS stack is pvPortMalloc'd from .ccmheap, which DMA cannot see), and a
 * second 180-byte sClusterCfg beside it would buy nothing: the record is idle
 * from the end of Cluster_Init until an upload, and from an upload until the
 * tick adopts it the PENDING FLAG is the ownership token over exactly these
 * bytes.  Main SRAM is this board's tightest region — 5.0 KB free measured
 * 2026-09-05 — so a copy that is never live twice is a copy not made. */
static sClusterCfgRecord  s_cfgRec;

static sClusterPackIn     s_in[CLUSTER_PACK_MAX];
static sClusterResult     s_res[2];
static sClusterScratch    s_scratch;
static sClusterCalcState  s_calc;
static sPackState         s_packScratch;
static sClusterStats      s_stats;

/* THE PUBLISH SEQLOCK.  Both volatile, both written only by the tick. */
static volatile uint8_t   s_active;
static volatile uint32_t  s_gen;

static volatile uint32_t  s_packEpoch;       /* bumped by the pack callback  */
static int                s_packSub = -1;
static uint8_t            s_inited;
static uint8_t            s_provisioned;
static uint8_t            s_cfgPending;

/* Private function prototypes ----------------------------------------------*/

static void PackConfigChanged(const sPackEvent *ev, void *ctx);
static void AdoptStaged(void);
static uint8_t GatherMembers(void);
static void Publish(uint8_t slot);
static int  CopySnapshot(sClusterOutput *out, sClusterMember *members,
                         uint8_t maxMembers, uint8_t *written);

/* Private functions --------------------------------------------------------*/

/**
 * @brief  The one pack event this module wants.
 * @note   Runs on the func task, synchronously, from inside Pack_.  It does
 *         ONE thing — bump a counter — because a subscriber callback must not
 *         block and this one has nothing else it could safely do.
 */
static void PackConfigChanged(const sPackEvent *ev, void *ctx)
{
    (void)ev;
    (void)ctx;
    s_packEpoch++;
}

/** Take the staged configuration into service.  Resets the WHOLE calc state:
 *  every value in it is slot-indexed, and the slots now point at different
 *  packs. */
static void AdoptStaged(void)
{
    s_cfg = s_cfgRec.cfg;
    ClusterCalc_Reset(&s_calc, (uint8_t)cluRestart_configAdopted);
    s_provisioned = (uint8_t)((s_cfg.count > 0u) ? 1u : 0u);
    /* THE PENDING FLAG IS THE OWNERSHIP TOKEN and is cleared LAST: until it
     * is, Cluster_ConfigApply refuses with cluErr_busy and s_cfgRec belongs
     * to this function. */
    s_cfgPending = 0u;
    s_stats.cfgPending  = 0u;
    s_stats.provisioned = s_provisioned;
}

/**
 * @brief  Fill s_in[] from Pack_GetState, resolving every member by name.
 * @retval the number of members filled (== s_cfg.count)
 */
static uint8_t GatherMembers(void)
{
    uint8_t i;

    (void)memset(s_in, 0, sizeof(s_in));

    for (i = 0u; i < s_cfg.count; i++) {
        sClusterPackIn *p   = &s_in[i];
        const int       idx = Pack_FindByName(s_cfg.member[i]);

        p->packIdx = CLUSTER_PACK_NONE;
        if (idx < 0) {
            s_stats.nameUnresolvedCnt++;
            continue;
        }
        if (Pack_GetState((uint8_t)idx, &s_packScratch) != packErr_ok) {
            s_stats.packReadFailCnt++;
            continue;
        }

        p->packIdx            = (uint8_t)idx;
        p->present            = 1u;
        p->cond               = s_packScratch.cond;
        p->caps               = s_packScratch.caps;
        p->alarms             = s_packScratch.alarms;
        /* R3.3: THE ELECTRICAL GROUP, not the pack's overall age.  It is the
         * group this module's arithmetic consumes, and on a live board the
         * groups differ by two orders of magnitude — 2.2 s against 362 s. */
        p->elecAge_ms         = s_packScratch.age_ms[packGrp_electrical];
        p->voltage_mV         = s_packScratch.voltage_mV;
        p->current_mA         = s_packScratch.current_mA;
        p->remaining_mAh      = s_packScratch.remaining_mAh;
        p->capacity_mAh       = s_packScratch.capacity_mAh;
        p->nameplate_mAh      = s_packScratch.nameplate_mAh;
        p->chargeLimit_mA     = s_packScratch.chargeLimit_mA;
        p->dischargeLimit_mA  = s_packScratch.dischargeLimit_mA;
        p->chargeVoltLimit_mV = s_packScratch.chargeVoltLimit_mV;
        p->dischargeVoltLimit_mV = s_packScratch.dischargeVoltLimit_mV;
        p->soc_pm             = s_packScratch.soc_pm;
        p->socConf_pm         = s_packScratch.socConf_pm;
        p->sohConf_pm         = s_packScratch.sohConf_pm;
        p->tempMax_dC         = s_packScratch.tempMax_dC;
        p->tempMin_dC         = s_packScratch.tempMin_dC;
        p->chargeSwitch       = s_packScratch.chargeSwitch;
        p->dischargeSwitch    = s_packScratch.dischargeSwitch;
    }
    return s_cfg.count;
}

/**
 * @brief  The publisher's half of the memory model.
 * @param  slot - the INACTIVE slot, already filled by this tick
 * @note   The tick solves directly into the inactive slot, so there is no copy
 *         here: a second 376-byte move of a buffer nothing else can see would
 *         be pure cost.
 */
static void Publish(uint8_t slot)
{
    s_res[slot].pub.seq = s_gen + 1u;
    __DMB();                            /* payload before the slot flip      */
    s_active = slot;
    __DMB();                            /* slot flip before the generation   */
    s_gen++;
    s_stats.publishes++;
}

/** The reader's half.  ONE generation for output AND members, so a status
 *  page cannot render a limit beside a share from a different tick. */
static int CopySnapshot(sClusterOutput *out, sClusterMember *members,
                        uint8_t maxMembers, uint8_t *written)
{
    uint8_t attempt;

    if (written != NULL) {
        *written = 0u;
    }
    if (s_gen == 0u) {
        s_stats.getNotReadyCnt++;
        return cluErr_notReady;
    }

    for (attempt = 0u; attempt < CLUSTER_GET_TRIES; attempt++) {
        const uint32_t g0 = s_gen;
        const uint8_t  a  = s_active;
        uint8_t        n  = 0u;

        __DMB();
        if (out != NULL) {
            *out = s_res[a].pub;
        }
        if ((members != NULL) && (maxMembers > 0u)) {
            n = (maxMembers < CLUSTER_PACK_MAX) ? maxMembers
                                                : (uint8_t)CLUSTER_PACK_MAX;
            (void)memcpy(members, s_res[a].member,
                         (size_t)n * sizeof(members[0]));
        }
        __DMB();
        if (s_gen == g0) {
            if (written != NULL) {
                *written = n;
            }
            return cluErr_ok;
        }
    }

    s_stats.getBusyCnt++;
    return cluErr_busy;
}

/* Exported functions -------------------------------------------------------*/

int Cluster_Init(void)
{
    if (s_inited != 0u) {
        return cluErr_ok;                   /* idempotent                    */
    }

    (void)memset(&s_stats, 0, sizeof(s_stats));
    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    ClusterCfg_Defaults(&s_cfg.tune);
    s_cfg.version = CLUSTER_CFG_VERSION;

    if (NvRecord_Load(nvdbUser_clusterCfg, CLUSTER_CFG_MAGIC,
                      CLUSTER_CFG_VERSION, &s_cfgRec,
                      sizeof(s_cfgRec)) == 0) {
        s_cfg         = s_cfgRec.cfg;
        s_provisioned = (uint8_t)((s_cfg.count > 0u) ? 1u : 0u);
    }
    s_stats.provisioned = s_provisioned;

    ClusterCalc_Reset(&s_calc, (uint8_t)cluRestart_init);
    /* THE GENERATION STAYS AT ZERO so every accessor answers cluErr_notReady
     * until a real publish has happened.  A frame source must never transmit
     * a snapshot that is merely zeroed .bss. */
    s_gen    = 0u;
    s_active = 0u;

    s_packSub = Pack_Subscribe((uint32_t)packEvt_config, PackConfigChanged,
                               NULL);
    if (s_packSub < 0) {
        TRice("wrn:[Clu] no pack subscription slot; epoch guard is off\n");
    }

    s_inited = 1u;
    TRice("[Clu] up, %u members, provisioned %u\n",
          (unsigned)s_cfg.count, (unsigned)s_provisioned);
    return cluErr_ok;
}

void Cluster_Tick(uint32_t now_ms)
{
    uint32_t epoch0;
    uint8_t  n;

    if (s_inited == 0u) {
        return;                             /* func.c ticks before Init      */
    }
    s_stats.ticks++;

    if (s_cfgPending != 0u) {
        AdoptStaged();
    }

    epoch0 = s_packEpoch;
    n      = GatherMembers();
    if (s_packEpoch != epoch0) {
        /* The pack table was rebuilt under the walk, so every index this pass
         * resolved is suspect.  Discard rather than publish a mixture.
         *
         * INSURANCE, NOT A LIVE RACE TODAY: Pack_ConfigApply only POSTS, and
         * the rebuild plus its packEvt_config both run inside
         * Pack_HandleEvent on this same task, which dispatches at most one
         * event and then ticks.  Keeping the guard costs one comparison and
         * survives that topology changing. */
        s_stats.packCfgSkipCnt++;
        return;
    }

    {
        const uint8_t   idle = (uint8_t)(s_active ^ 1u);
        sClusterResult *r    = &s_res[idle];

        if (ClusterCalc_Solve(s_in, n, &s_cfg.tune, &s_calc, &s_scratch,
                              now_ms, r) != cluErr_ok) {
            return;
        }
        if (s_provisioned == 0u) {
            r->pub.cond = (uint8_t)cluCond_unprovisioned;
        }
        if (s_packEpoch != epoch0) {
            r->pub.clusterAlarms |= (uint32_t)cluAlarm_packCfgChanged;
        }

        /* Counters, from the events the pass reported rather than re-derived
         * from the output — two views of one tick drift apart. */
        if (r->pub.chargeWhy == (uint8_t)cluLimitWhy_noParticipant) {
            s_stats.noParticipantChgCnt++;
        }
        if (r->pub.dischargeWhy == (uint8_t)cluLimitWhy_noParticipant) {
            s_stats.noParticipantDsgCnt++;
        }
        if ((r->ev & (uint32_t)cluCalcEv_bindingChg) != 0u) {
            s_stats.bindingSampleChgCnt++;
        }
        if ((r->ev & (uint32_t)cluCalcEv_bindingDsg) != 0u) {
            s_stats.bindingSampleDsgCnt++;
        }
        if ((r->ev & (uint32_t)cluCalcEv_restartChg) != 0u) {
            s_stats.restartChgCnt++;
        }
        if ((r->ev & (uint32_t)cluCalcEv_restartDsg) != 0u) {
            s_stats.restartDsgCnt++;
        }
        if ((r->ev & ((uint32_t)cluCalcEv_stepClampedChg |
                      (uint32_t)cluCalcEv_stepClampedDsg)) != 0u) {
            s_stats.stepClampedCnt++;
        }
        if ((r->ev & ((uint32_t)cluCalcEv_slewChg |
                      (uint32_t)cluCalcEv_slewDsg)) != 0u) {
            s_stats.slewLimitedCnt++;
        }
        if (r->pub.chargeAllowed == 0u)    { s_stats.forbiddenChgCnt++; }
        if (r->pub.dischargeAllowed == 0u) { s_stats.forbiddenDsgCnt++; }
        if ((r->pub.clusterAlarms & (uint32_t)cluAlarm_voltLimitMissing) != 0u) {
            s_stats.voltLimitMissingCnt++;
        }
        if ((r->pub.clusterAlarms & (uint32_t)cluAlarm_socDiverge) != 0u) {
            s_stats.divergeSocCnt++;
        }
        if ((r->pub.clusterAlarms & (uint32_t)cluAlarm_shareDiverge) != 0u) {
            s_stats.divergeShareCnt++;
        }
        s_stats.sanitisedCnt += (uint32_t)r->sanitised;

        Publish(idle);
    }
}

int Cluster_GetOutput(sClusterOutput *out)
{
    int r;

    if (out == NULL) {
        return cluErr_badArg;
    }
    if (s_provisioned == 0u) {
        return cluErr_unprovisioned;
    }
    r = CopySnapshot(out, NULL, 0u, NULL);
    if (r != cluErr_ok) {
        return r;
    }
    /* A FROZEN SNAPSHOT IS R2.6'S FAILURE ON THE WRONG SIDE OF THE SEAM: the
     * requirement is careful that losing a pack must reduce the limits before
     * the inverter can act on the old ones, and a transmitter happily
     * repeating a stopped tick defeats that entirely. */
    if ((uint32_t)(osKernelGetTickCount() - out->tick_ms) >
        CLUSTER_OUTPUT_MAX_AGE_MS) {
        return cluErr_stale;
    }
    return cluErr_ok;
}

int Cluster_GetSnapshot(sClusterOutput *out, sClusterMember *members,
                        uint8_t maxMembers, uint8_t *written)
{
    if ((out == NULL) && (members == NULL)) {
        return cluErr_badArg;
    }
    return CopySnapshot(out, members, maxMembers, written);
}

int Cluster_Count(void)
{
    return (int)s_cfg.count;
}

int Cluster_MemberName(uint8_t slot, char *out, uint32_t cap)
{
    if ((out == NULL) || (cap == 0u)) {
        return cluErr_badArg;
    }
    out[0] = '\0';
    if (slot >= s_cfg.count) {
        return cluErr_notFound;
    }
    /* COPY-OUT, never a borrowed pointer: the configuration can be replaced
     * by the tick between this call and the caller's use of the result. */
    (void)snprintf(out, (size_t)cap, "%s", s_cfg.member[slot]);
    return cluErr_ok;
}

int Cluster_ProfileToken(char *out, uint32_t cap)
{
    if ((out == NULL) || (cap == 0u)) {
        return cluErr_badArg;
    }
    (void)snprintf(out, (size_t)cap, "%s", s_cfg.profile);
    return (s_cfg.profile[0] == '\0') ? cluErr_unprovisioned : cluErr_ok;
}

int Cluster_ConfigVerify(fClusterByteSource src, void *srcCtx,
                         sClusterCfgResult *res)
{
    sClusterCfg scratch;

    /* THE PARSE TARGET IS A CALLER-STACK sClusterCfg (180 B on http's 4 KB),
     * never the staging slot — a verify must not be able to disturb a stage
     * the tick has not yet adopted. */
    return ClusterCfg_Parse(src, srcCtx, &scratch, res);
}

int Cluster_ConfigApply(fClusterByteSource src, void *srcCtx,
                        sClusterCfgResult *res)
{
    sClusterCfg parsed;
    int         r;

    if (s_cfgPending != 0u) {
        return cluErr_busy;                 /* -> HTTP 409                   */
    }

    r = ClusterCfg_Parse(src, srcCtx, &parsed, res);
    if (r != cluErr_ok) {
        return r;
    }

    (void)memset(&s_cfgRec, 0, sizeof(s_cfgRec));
    s_cfgRec.cfg = parsed;
    if (NvRecord_Save(nvdbUser_clusterCfg, CLUSTER_CFG_MAGIC,
                      CLUSTER_CFG_VERSION, &s_cfgRec,
                      sizeof(s_cfgRec)) != 0) {
        return cluErr_transport;
    }

    /* Already in s_cfgRec.cfg — the save above put it there.  Claiming the
     * record IS the staging step. */
    s_cfgPending = 1u;                      /* claimed; the tick clears it   */
    s_stats.cfgPending = 1u;
    return cluErr_ok;                       /* STAGED, not live              */
}

int Cluster_ConfigExport(fClusterByteSink sink, void *ctx)
{
    if (s_provisioned == 0u) {
        return cluErr_unprovisioned;
    }
    return ClusterCfg_Serialize(&s_cfg, sink, ctx);
}

int Cluster_ConfigErase(void)
{
    /* Erase, not reset: with no built-in default there is nothing to reset
     * TO — a cluster configuration names packs this board may not have. */
    (void)NvRecord_Forget(nvdbUser_clusterCfg);

    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    ClusterCfg_Defaults(&s_cfg.tune);
    s_cfg.version       = CLUSTER_CFG_VERSION;
    s_provisioned       = 0u;
    s_stats.provisioned = 0u;
    /* Every slot-indexed value now refers to nothing.  The next tick
     * publishes zero limits, which is exactly what an unprovisioned board
     * should say. */
    ClusterCalc_Reset(&s_calc, (uint8_t)cluRestart_configAdopted);
    return cluErr_ok;
}

int Cluster_Stats(sClusterStats *out)
{
    if (out == NULL) {
        return cluErr_badArg;
    }
    *out = s_stats;
    return cluErr_ok;
}

void Cluster_LogStatus(void)
{
    sClusterOutput o;

    if (CopySnapshot(&o, NULL, 0u, NULL) != cluErr_ok) {
        TRice("[Clu] no snapshot yet (members %u, provisioned %u)\n",
              (unsigned)s_cfg.count, (unsigned)s_provisioned);
        return;
    }
    TRice("[Clu] %s %u/%u online, %u mV, %d mA, soc %u pm\n",
          Cluster_CondName(o.cond), (unsigned)o.onlineCnt,
          (unsigned)o.memberCnt, (unsigned)o.voltage_mV,
          (int)o.current_mA, (unsigned)o.soc_pm);
    TRice("[Clu] chg %u mA (%s/%s) dsg %u mA (%s/%s)\n",
          (unsigned)o.chargeLimit_mA,
          Cluster_LoopStateName(o.chargeLoopState),
          Cluster_LimitWhyName(o.chargeWhy),
          (unsigned)o.dischargeLimit_mA,
          Cluster_LoopStateName(o.dischargeLoopState),
          Cluster_LimitWhyName(o.dischargeWhy));
}

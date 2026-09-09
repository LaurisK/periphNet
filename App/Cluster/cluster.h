/**
 * @file    cluster.h
 * @brief   PeriphNet battery cluster module — THE public API.
 *
 * A CLUSTER IS N PACKS ON ONE DC BUS PRESENTED AS ONE BATTERY.  It is a
 * consumer of App/Pack/pack.h and a producer of one settled snapshot; it owns
 * no peripheral, no CAN handle, no Modbus and no socket.
 *
 * THE LIMIT IS MEASURED, NOT SEARCHED (docs/design_battery_cluster.md §3.2,
 * revision 3).  It is a PURE FUNCTION OF THIS TICK — no feedback, no carried
 * loop variable, no gate:
 *
 *      loadMax_pm = max over packs of ( |I_i| * 1000 / L_i )
 *      target     = tune.safetyMargin_pm * |S| / loadMax_pm
 *
 * Read it as: scale the whole bus up until the HARDEST-WORKING pack reaches
 * its own limit, then keep a margin.  |S| is the summed magnitude over packs
 * flowing this way, so with two equal 300 A packs drawing 100 A and 80 A the
 * bus could carry 180 x (300/100) = 540 A and the module publishes 486 A.
 *
 * IT CAN NEVER EXCEED margin x sum(L_i), and that is a proof rather than a
 * clamp: every pack satisfies |I_i| <= f x L_i for f = loadMax, so
 * |S| <= f x sum(L_i) and |S|/f <= sum(L_i).  Revision 2's upward step clamp
 * existed to bound a recursion and is gone with it.
 *
 * BELOW tune.lowLoadFloor_pm THE RATIO IS NOISE — two small numbers divided —
 * so a geometric prediction over the participants' own limits runs instead,
 * SMALLEST FIRST:
 *
 *      target = d*L(1) + d^2*L(2) + d^3*L(3) + ...   ,  d = predictDecay_pm
 *
 * which is conservative against sum(L_i) and never trusts N packs to share
 * evenly.  A SINGLE PACK IS CONTINUOUS ACROSS THE FLOOR — both rules give
 * margin x L — so the one-pack site never sees a step.
 *
 * WHAT THIS MODULE WILL NOT DO: command a pack; invent a number a pack does
 * not publish; take a lock; hand out a pointer into live state; run a float;
 * know what a CAN identifier is.
 *
 * FOUR CONTRACTS A CONSUMER MUST NOT GET WRONG
 *
 *   1. SIGN.  current_mA is + CHARGE, - DISCHARGE, identical to
 *      sPackState.current_mA and to both devices measured in the field.  The
 *      limits are UNSIGNED MAGNITUDES; direction is in the field name, never
 *      in a sign.  What an inverter expects is UNMEASURED and is the FRAME
 *      SOURCE's problem, in exactly one bit, so it never reaches here.
 *
 *   2. VALIDITY.  A cleared eClusterField bit means the field reads zero and
 *      MEANS NOTHING.  A zero charge-voltage limit is not "no limit", it is an
 *      instruction to stop charging.
 *
 *   3. PERMISSION AND LIMIT AGREE, ALWAYS.  A forbidden direction publishes a
 *      ZERO current limit as well as a cleared permission flag.  Permission
 *      reaches an inverter only through 0x35C, whose semantics the field
 *      capture records as UNVERIFIED; 0x351's current limit is the one field
 *      measured end-to-end as obeyed.  Encoding a refusal only in the channel
 *      with no evidence behind it, while the channel known to work says 96 A
 *      is fine, is the same failure this module refuses to make with a zero
 *      CVL, inverted.  THE CONVERSE DOES NOT HOLD: a zero limit does not imply
 *      a forbidden direction — a bus with no participant publishes zero
 *      without refusing anything.
 *
 *   4. TRANSMIT ONLY ON cluErr_ok.  Not "stop on stale" — POSITIVE, because
 *      notReady, busy, stale and unprovisioned all mean the same thing to a
 *      transmitter and an implementer will otherwise invent a policy for each.
 *      cluErr_busy MUST NOT be read as "reuse the last frame": that is the
 *      frozen-snapshot failure, and it is reachable.
 *
 * BOUNDARY: a consumer includes ONLY this header.  Files in App/Cluster must
 * not include App/Can, App/Http, App/Gw, App/Data, App/Modbus, lwIP or the
 * HAL; must never take LOCK_TCPIP_CORE; must never call a raw lwIP API.  No
 * floating point appears ANYWHERE in App/Cluster — enforced ON THE OBJECT
 * FILES, not on this text: a source grep cannot tell a rule from a sentence
 * describing the rule.
 *
 * STATUS: IMPLEMENTED, host-tested (tests/test_cluster_calc.c,
 * tests/test_cluster_cfg.c).  NOT YET RUN ON HARDWARE, and it drives nothing
 * until a CAN frame source consumes it — see design §7.4 for why that order is
 * deliberate.
 */

#ifndef CLUSTER_H_
#define CLUSTER_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants -------------------------------------------------------*/

/* ==========================================================================
 * Bounds
 * ========================================================================== */

/** SPELLED INDEPENDENTLY, NOT AS `PACK_MAX` — defining it as PACK_MAX makes
 *  the assertion `8 == 8` by substitution and it could never fire, which is
 *  the whole point of having it. */
#define CLUSTER_PACK_MAX            8u
_Static_assert(CLUSTER_PACK_MAX == PACK_MAX,
               "a cluster must hold every pack the board can hold");

#define CLUSTER_NAME_LEN            PACK_NAME_LEN   /* including NUL          */
#define CLUSTER_PROFILE_LEN         12u             /* OPAQUE token           */
#define CLUSTER_PACK_NONE           0xFFu
#define CLUSTER_OUTPUT_MAX_AGE_MS   3000u

/** THE SAFETY MARGIN, per-mille of what the measurement says the bus could
 *  carry.  The remaining 10 % absorbs sharing shifting between one tick and
 *  the next — which is the ONE assumption revision 3 makes and the closed
 *  loop did not (design §3.2). */
#define CLUSTER_DFLT_SAFETY_MARGIN_PM 900u

/** A NUMERICAL bound, not a physical one.  cluster_cfg.c rejects a configured
 *  limitMax_mA that is zero or above this, so the bound is LOAD-BEARING
 *  ARITHMETIC and a host test asserts it as such.  The limit arithmetic
 *  itself is uint64 throughout and does not depend on it. */
#define CLUSTER_LIMIT_MAX_MA        1000000u        /* 1000 A                 */

/** THREE MACRO CLASSES, AND THE PREFIX SAYS WHICH:
 *    CLUSTER_DFLT_*     a DEFAULT for a sClusterTune field.  The live value is
 *                       always tune.<field>; never read the macro at runtime.
 *    CLUSTER_*_MAX/MIN  a BOUND the parser checks a tune field against.
 *    CLUSTER_PLAUS_*    the plausibility domain for values arriving FROM A
 *                       PACK, not for configuration.
 *
 *  THE PLAUSIBLE DOMAIN.  Every value arriving from a pack is a raw integer
 *  out of a Modbus decode; a wrong decodeType or scale yields 0xFFFF0000 with
 *  no error anywhere in the system. */
#define CLUSTER_PLAUS_LIMIT_MA      1000000u        /* 1000 A per pack        */
#define CLUSTER_PLAUS_CURRENT_MA    1000000         /* +/- 1000 A per pack    */
#define CLUSTER_PLAUS_CAPACITY_MAH  2000000u        /* 2000 Ah per pack       */
#define CLUSTER_PLAUS_VOLTAGE_MV    100000u         /* 100 V — a 48 V-class
                                                       assumption; an HV pack
                                                       is rejected outright   */
#define CLUSTER_PLAUS_TEMP_MAX_DC   1500            /* +150.0 degC            */
#define CLUSTER_PLAUS_TEMP_MIN_DC   (-500)          /*  -50.0 degC            */

/* Exported types -----------------------------------------------------------*/

/* ==========================================================================
 * Enums.  Eight Cluster_*Name() accessors
 * follow, each a switch ((eType)v) — CAST TO THE ENUM TYPE, because
 * -Werror=switch does nothing on a uint8_t — fallback AFTER the switch, never
 * a `default:`.  All nine live in cluster_cfg.c, which joins
 * ENUM_NAME_SOURCES.
 * ========================================================================== */

typedef enum {
    cluErr_ok            =   0,
    cluErr_badArg        =  -1,
    cluErr_notFound      =  -2,
    cluErr_unprovisioned =  -3,
    cluErr_notReady      =  -4,   /* no real publish has happened YET         */
    cluErr_stale         =  -5,
    cluErr_busy          =  -6,   /* the publisher overtook the reader        */
    cluErr_transport     =  -7,   /* the medium refused the record            */
} eClusterErr;

typedef enum {
    cluCond_unprovisioned = 0,
    cluCond_absent,
    cluCond_degraded,           /* THE STATE HALF THE SITE'S ENERGY IS
                                   INVISIBLE IN                              */
    cluCond_online,
    cluCond_last
} eClusterCondition;

typedef enum {
    cluMember_unresolved = 0,
    cluMember_absent,
    cluMember_stale,            /* ELECTRICAL group too old                   */
    cluMember_present,          /* online, contributing to neither direction  */
    cluMember_participating,
    cluMember_last
} eClusterMemberState;

typedef enum {
    cluWhy_none = 0,
    cluWhy_nameUnresolved,
    cluWhy_notOnline,
    cluWhy_electricalStale,
    cluWhy_noLimitCap,
    cluWhy_zeroLimits,
    cluWhy_switchesOpen,
    cluWhy_implausible,         /* DROPPED, not clamped: clamping a current
                                   DOWN under-reports load, the anti-safe
                                   direction                                 */
    cluWhy_last
} eClusterMemberWhy;

/** WHICH OF THE TWO RULES PRODUCED THE TARGET.  cluLimitWhy_ answers the
 *  different question "why is the EMITTED number this"; an operator asks them
 *  separately, and the slew or the ceiling can override a target either rule
 *  produced. */
typedef enum {
    cluLoop_idle = 0,           /* no participant in this direction           */
    cluLoop_measured,           /* from live current — the normal state       */
    cluLoop_predicted,          /* the bus is too quiet to measure; the
                                   geometric fallback ran instead             */
    cluLoop_last
} eClusterLoopState;

/** The values PARTITION: exactly one applies, reported in the order
 *  forbidden > noParticipant > ceiling > predicted > measured.
 *  DECLARATION ORDER IS STORAGE ORDER and is deliberately not the priority
 *  order; neither may be inferred from the other. */
typedef enum {
    cluLimitWhy_noParticipant = 0,
    cluLimitWhy_forbidden,          /* 0 mA; the direction is not allowed     */
    cluLimitWhy_measured,           /* margin * |S| / max(I_i/L_i)            */
    cluLimitWhy_predicted,          /* below lowLoadFloor_pm; the geometric
                                       sum over participants ran instead      */
    cluLimitWhy_ceiling,
    cluLimitWhy_last
} eClusterLimitWhy;

/* THERE IS NO ABSORBING STATE TO CLOSE.  Revision 2's loop was recursive —
 * loop' = published * t / loadMax — so a zero could never be left and the
 * whole restart machinery existed to re-seed it.  Revision 3's target is a
 * PURE FUNCTION of this tick's currents and limits: it carries nothing, so a
 * quiet tick, a forbidden direction or a pack leaving costs nothing that the
 * next tick does not simply recompute. */

typedef enum {
    cluField_voltage            = 1u << 0,
    cluField_current            = 1u << 1,
    cluField_soc                = 1u << 2,  /* clear when sum capacity == 0   */
    cluField_soh                = 1u << 3,  /* clear when sum nameplate == 0  */
    cluField_chargeLimit        = 1u << 4,
    cluField_dischargeLimit     = 1u << 5,
    cluField_chargeVoltLimit    = 1u << 6,
    cluField_dischargeVoltLimit = 1u << 7,
    cluField_temperature        = 1u << 8,
    cluField_switches           = 1u << 9,
} eClusterField;

typedef enum {
    cluAlarm_noMembers        = 1u << 0,
    cluAlarm_allOffline       = 1u << 1,
    cluAlarm_memberLost       = 1u << 2,
    cluAlarm_socDiverge       = 1u << 3,
    cluAlarm_shareDiverge     = 1u << 4,  /* an OBSERVATION, never a control
                                             input                            */
    cluAlarm_busSplit         = 1u << 5,
    cluAlarm_circulating      = 1u << 6,
    cluAlarm_chargeForbidden  = 1u << 7,
    cluAlarm_dischargeForbid  = 1u << 8,
    cluAlarm_voltLimitMissing = 1u << 9,
    cluAlarm_nameUnresolved   = 1u << 10,
    cluAlarm_implausible      = 1u << 11,
    cluAlarm_packCfgChanged   = 1u << 12, /* the pack table moved under this
                                             tick; the arithmetic was skipped */
} eClusterAlarm;

typedef enum {
    cluMemFlag_bindingCharge    = 1u << 0,  /* it set loadMax, so IT is the
                                               pack the limit is scaled from  */
    cluMemFlag_bindingDischarge = 1u << 1,
    cluMemFlag_socOutlier       = 1u << 2,
    cluMemFlag_shareOutlier     = 1u << 3,
    cluMemFlag_circulating      = 1u << 4,
    cluMemFlag_implausible      = 1u << 5,
    cluMemFlag_limitSaturated   = 1u << 6,  /* L_i == 0 with |I_i| > 0, so
                                               load_pm saturated at 1000
                                               rather than dividing           */
} eClusterMemberFlag;

/* ==========================================================================
 * The published snapshot — 104 bytes.
 * THE THREE-NUMBER CAUSAL CHAIN per direction — target -> derated ->
 * published — is carried in full rather than derived, so an adapter renders
 * "why is the limit this" with NO arithmetic of its own.  It was four until
 * the rate limiter was deleted (§14.8).
 * ========================================================================== */

typedef struct {
    uint32_t seq;                   /* 0 = never published                    */
    uint32_t tick_ms;

    uint32_t voltage_mV;            /* MEAN over online packs                 */
    int32_t  current_mA;            /* + charge, - discharge.  Sum            */
    uint32_t remaining_mAh;
    uint32_t capacity_mAh;
    uint32_t nameplate_mAh;

    uint32_t chargeLimit_mA;        /* PUBLISHED — the end of the chain       */
    uint32_t dischargeLimit_mA;
    uint32_t chargeVoltLimit_mV;    /* MIN over ONLINE packs, INCLUDING one
                                       that refuses to charge                 */
    uint32_t dischargeVoltLimit_mV;

    uint32_t chargeTarget_mA;       /* what the rule produced, pre-derate     */
    uint32_t dischargeTarget_mA;
    uint32_t chargeDerated_mA;      /* published == derated UNLESS forbidden,
                                       when published is 0 and derated still
                                       shows what the rule would have said    */
    uint32_t dischargeDerated_mA;

    uint32_t alarms;                /* ePackAlarm, OR over ONLINE packs — the
                                       PACK MODULE'S vocabulary, republished
                                       as a mask, never spelled here          */
    uint32_t clusterAlarms;         /* eClusterAlarm                          */

    uint16_t soc_pm;
    uint16_t soh_pm;
    uint16_t socConf_pm;            /* MIN over contributors                  */
    uint16_t sohConf_pm;
    uint16_t voltSpread_mV;
    uint16_t fields;                /* eClusterField                          */
    uint16_t chargeLoadMax_pm;      /* max(I_i/L_i) — THE RULE'S DIVISOR, and
                                       what lowLoadFloor_pm is compared to    */
    uint16_t dischargeLoadMax_pm;
    int16_t  tempMax_dC;
    int16_t  tempMin_dC;

    uint8_t  cond;                  /* eClusterCondition                      */
    uint8_t  valid;
    uint8_t  memberCnt;             /* configured                             */
    uint8_t  onlineCnt;             /* online AND electrically fresh          */
    uint8_t  chargeAllowed;
    uint8_t  dischargeAllowed;
    uint8_t  chargeWhy;             /* eClusterLimitWhy                       */
    uint8_t  dischargeWhy;
    uint8_t  chargeLoopState;       /* eClusterLoopState                      */
    uint8_t  dischargeLoopState;
    uint8_t  chargeBindingIdx;      /* member SLOT, or CLUSTER_PACK_NONE      */
    uint8_t  dischargeBindingIdx;
    uint8_t  rsvd[4];
} sClusterOutput;

/* ==========================================================================
 * Per-member detail — 32 bytes.  IT CARRIES ITS OWN COPY of the pack-sourced
 * numbers the observability surface renders: an adapter that re-read
 * Pack_GetState would render a DIFFERENT snapshot from the one the arithmetic
 * used, so load_pm and current_mA could sit in one JSON object and contradict
 * each other.
 * ========================================================================== */

typedef struct {
    int32_t  current_mA;            /* as the arithmetic saw it               */
    uint32_t chargeLimit_mA;
    uint32_t dischargeLimit_mA;
    uint32_t elecAge_ms;
    uint32_t flags;                 /* eClusterMemberFlag — 32 BITS, so a
                                       future flag cannot truncate silently   */
    uint16_t share_pm;              /* |I_i| / |S|.  AN OBSERVATION ONLY — the
                                       estimators are gone, the requirement to
                                       REPORT share divergence is not         */
    uint16_t load_pm;               /* |I_i| / L_i.  The largest of these over
                                       the bus is the rule's whole divisor    */
    uint16_t soc_pm;
    uint8_t  packIdx;
    uint8_t  state;                 /* eClusterMemberState                    */
    uint8_t  why;                   /* eClusterMemberWhy                      */
    uint8_t  rsvd[2];
} sClusterMember;

/* 32 bits of room against 10 used, so this cannot be reached by appending. */
_Static_assert(sizeof(((sClusterMember *)0)->flags) * 8u >= 32u,
               "eClusterMemberFlag needs a 32-bit sClusterMember.flags");

typedef struct {
    uint32_t ticks, publishes;
    uint32_t packReadFailCnt, nameUnresolvedCnt;
    uint32_t packCfgSkipCnt;        /* ticks discarded because the pack table
                                       was rebuilt under the member walk      */
    uint32_t noParticipantChgCnt, noParticipantDsgCnt;
    uint32_t predictedChgCnt;       /* ticks the bus was too quiet to measure
                                       and the geometric fallback ran.  ON A
                                       TYPICAL SITE THIS IS THE MAJORITY      */
    uint32_t predictedDsgCnt;
    uint32_t forbiddenChgCnt, forbiddenDsgCnt;
    uint32_t voltLimitMissingCnt;
    uint32_t divergeSocCnt, divergeShareCnt;
    uint32_t sanitisedCnt;
    uint32_t getBusyCnt;            /* THE READER IS PRIORITY 2.  Not exotic  */
    uint32_t getNotReadyCnt;
    uint8_t  provisioned, cfgPending, rsvd[2];
} sClusterStats;

typedef fPackByteSource fClusterByteSource;
typedef fPackByteSink   fClusterByteSink;

typedef struct {
    int      ok;
    int      memberIdx;
    char     field[24];
    char     reason[64];
    uint8_t  members;
} sClusterCfgResult;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Bring the cluster up.  func.c only, AFTER Pack_Init.
 *
 * Treats "nothing stored" and NvDb_GetSize() == 0 as unprovisioned.  Leaves
 * the generation counter at zero so every accessor answers cluErr_notReady
 * until the first real publish.  Resolves no pack index.
 *
 * Takes ONE Pack_Subscribe slot, for packEvt_config alone: not caching an
 * index removes the stale-index class but not the racing-resolution class, and
 * that needed a signal.  Takes no event-ID base and no post function — this
 * module posts nothing, so FUNC_CLIENT_COUNT is unchanged.
 *
 * @retval cluErr_ok always; an unprovisioned board is not an error
 */
int  Cluster_Init(void);

/**
 * @brief  One aggregation pass.  func.c only, AFTER Pack_Tick in the same
 *         iteration.
 *
 * A NO-OP until Cluster_Init has run — func.c runs its wall-clock branch
 * whether or not the start event was dispatched.  Adoption resets the WHOLE
 * sClusterCalcState, not merely the slew: every value in it is slot-indexed.
 * Allocates nothing on the stack larger than 64 B.
 */
void Cluster_Tick(uint32_t now_ms);

/**
 * @brief  Copy the settled snapshot.
 *
 * LEGAL FROM THE RTOS TIMER TASK, and the caveat is part of the contract:
 * `Tmr Svc` is priority 2 on a 256-word stack, shares its queue with every
 * Modbus mbtick, and is 21 levels below the publisher — so `out` must be a
 * file-static in the caller, never a local.
 *
 * The memory model is explicit.  Publisher: write payload, __DMB(),
 * s_active = next (volatile), __DMB(), s_gen++ (volatile).  Reader: g0 =
 * s_gen, __DMB(), copy, __DMB(), g1 = s_gen, retry while different.  __DMB()
 * is a barrier, not a lock.  FOUR ATTEMPTS, then cluErr_busy — during an OTA
 * the CPU pins at 991 permille and a >500 ms starvation of a priority-2 task
 * is expected.
 *
 * @retval cluErr_ok, cluErr_badArg, cluErr_notReady, cluErr_stale,
 *         cluErr_busy
 */
int  Cluster_GetOutput(sClusterOutput *out);

/**
 * @brief  Output AND members from ONE generation.
 *
 * So a status page cannot render a limit beside a share from a different tick.
 * @param  written - members actually copied; may be NULL
 */
int  Cluster_GetSnapshot(sClusterOutput *out, sClusterMember *members,
                         uint8_t maxMembers, uint8_t *written);

/** @brief CONFIGURED members, matching Pack_Count(); onlineCnt is in the
 *         snapshot. */
int  Cluster_Count(void);

/** @brief Copy-out member name; zero extra static bytes. */
int  Cluster_MemberName(uint8_t slot, char *out, uint32_t cap);

/** @brief Copy-out the opaque profile token.  Never compared here. */
int  Cluster_ProfileToken(char *out, uint32_t cap);

int  Cluster_ConfigVerify(fClusterByteSource src, void *srcCtx,
                          sClusterCfgResult *res);

/**
 * @brief  Parse, persist and STAGE a configuration.
 *
 * cluErr_ok means STAGED, not live: the tick adopts it.  A pending stage
 * refuses with cluErr_busy -> HTTP 409.  The parse target is a caller-stack
 * sClusterCfg, never the staging slot; the pending flag is the ownership token
 * and the tick clears it LAST.
 */
int  Cluster_ConfigApply (fClusterByteSource src, void *srcCtx,
                          sClusterCfgResult *res);
int  Cluster_ConfigExport(fClusterByteSink sink, void *ctx);
int  Cluster_ConfigErase (void);
int  Cluster_Stats(sClusterStats *out);

const char *Cluster_CondName(uint8_t cond);
const char *Cluster_MemberStateName(uint8_t state);
const char *Cluster_MemberWhyName(uint8_t why);
const char *Cluster_LoopStateName(uint8_t state);
const char *Cluster_LimitWhyName(uint8_t why);
const char *Cluster_AlarmName(uint32_t bit);
const char *Cluster_MemberFlagName(uint32_t bit);
const char *Cluster_FieldName(uint32_t bit);

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_H_ */

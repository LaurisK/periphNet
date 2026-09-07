/**
 * @file    cluster.h
 * @brief   PeriphNet battery cluster module — THE public API.
 *
 * A CLUSTER IS N PACKS ON ONE DC BUS PRESENTED AS ONE BATTERY.  It is a
 * consumer of App/Pack/pack.h and a producer of one settled snapshot; it owns
 * no peripheral, no CAN handle, no Modbus and no socket.
 *
 * THE LIMIT IS MEASURED, NOT ESTIMATED (docs/design_battery_cluster.md §3.2):
 *
 *      loadMax_pm = max over packs of ( |I_i| * 1000 / L_i )
 *      loop'      = published * tune.loadTarget_pm / loadMax_pm
 *
 * updated ONLY while the bus is actually being driven to the published limit.
 * There is no share estimator, no conductance fit and no separate safety
 * guard; there is one loop, and its gate is the single most important thing in
 * the module.
 *
 * THE MEASUREMENT IS TAKEN AT THE EMITTED VALUE, WHICH IS WHAT FOLDS THE
 * DERATE INTO THE SETPOINT (§3.2, defect L1).  `published` above is the number
 * on the wire — already derated — so the fixed point of the emitted value is
 *
 *      published*  =  loadTarget_pm * derate_pm / 1e6  x  min(L_i / f_i)
 *
 * and the loop variable settles at loadTarget_pm/1000 x min(L_i/f_i).  BOTH
 * ARE AT OR BELOW THE BINDING PACK'S OWN LIMIT, for every derate, which is the
 * property the cascade reading (loop converged pre-derate, derate applied
 * afterwards) does not have: that one settles the inner variable 12.5 % ABOVE
 * the binding pack's limit and emits it the instant an operator raises the
 * derate to 1000.
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
 *      a forbidden direction — a loop that has not yet earned headroom
 *      publishes a small limit, not a refusal.
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

/** THE LOOP'S SETPOINT, per-mille of a pack's own limit.  The remaining 10 %
 *  absorbs sharing shifting between one tick and the next. */
#define CLUSTER_DFLT_LOAD_TARGET_PM 900u

/** A NUMERICAL bound, not a physical one: it keeps `published * loadTarget_pm`
 *  inside uint32 (which overflows above 4 772 185 mA at 900 pm).
 *  cluster_cfg.c rejects a configured limitMax_mA that is zero or above this,
 *  so the bound is LOAD-BEARING ARITHMETIC and a host test asserts it as
 *  such. */
#define CLUSTER_LIMIT_MAX_MA        1000000u        /* 1000 A                 */

/* Bound on ONE UPWARD update.  THERE IS DELIBERATELY NO DOWNWARD CLAMP: a
 * clamp on the safety action is a contradiction.  Garbage is handled where it
 * enters, by the plausibility pass, not here. */
#define CLUSTER_STEP_UP_MAX_PM      4000u
#define CLUSTER_DFLT_CONVERGE_TOL_PM 50u

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

/** riseRate_mA_per_s == 0 is REJECTED at parse rather than meaning "frozen":
 *  the step floor would still let it climb at 40 mA/s, which is a surprise,
 *  not a policy.  That floor exists ONLY so a slow rate does not stall on
 *  integer truncation of (rate * dt)/1000. */
#define CLUSTER_RISE_MIN_MA_PER_S   100u
#define CLUSTER_RISE_MAX_MA_PER_S   100000u
#define CLUSTER_RISE_STEP_MIN_MA    10u
#define CLUSTER_DT_MAX_MS           2000u

/* Exported types -----------------------------------------------------------*/

/* ==========================================================================
 * Enums — APPEND-ONLY, NEVER RENUMBERED.  Nine Cluster_*Name() accessors
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

/** WHERE THE LOOP IS.  cluLimitWhy_ answers the different question "why is the
 *  number this"; an operator asks them separately. */
typedef enum {
    cluLoop_idle = 0,           /* no participant in this direction           */
    cluLoop_searching,          /* at the start value; NO binding sample yet  */
    cluLoop_holding,            /* has a learned value; the bus is not driving
                                   the limit.  ON A TYPICAL SITE THIS IS THE
                                   COMMON STATE and it is correct             */
    cluLoop_converged,
    cluLoop_last
} eClusterLoopState;

/** WHY THE SEARCH RESTARTED. */
typedef enum {
    cluRestart_none = 0,
    cluRestart_init,
    cluRestart_configAdopted,   /* slots re-point at different packs, so EVERY
                                   slot-indexed value is meaningless          */
    cluRestart_limitFell,       /* past limitDeadband_pm.  An INCREASE does
                                   not restart: safe at the smaller limit is
                                   safe at the larger one                     */
    cluRestart_memberJoined,
    cluRestart_memberLeft,      /* the pack is still bolted to the busbar.
                                   Losing sight of it must NEVER license a
                                   higher limit                               */
    cluRestart_holdExpired,
    cluRestart_permitted,       /* forbidden -> allowed.  Without this the
                                   emitted limit steps 0 -> learned in one
                                   tick, a current step into real cells       */
    cluRestart_last
} eClusterRestart;

/** The values PARTITION: exactly one applies, reported in the order
 *  forbidden > noParticipant > ceiling > slew > binding > notBinding > start.
 *  DECLARATION ORDER IS STORAGE ORDER and is deliberately not the priority
 *  order; neither may be inferred from the other. */
typedef enum {
    cluLimitWhy_noParticipant = 0,
    cluLimitWhy_forbidden,          /* 0 mA; the direction is not allowed     */
    cluLimitWhy_start,              /* at the safe OPENING value min(L_i).
                                       NOT the fixed point's lower bound —
                                       two different numbers, and conflating
                                       them is how one gets coded as the
                                       other.  See design §3.3               */
    cluLimitWhy_notBinding,         /* holding; the bus is not driving it     */
    cluLimitWhy_binding,            /* a binding measurement set it this tick */
    cluLimitWhy_slew,
    cluLimitWhy_ceiling,
    cluLimitWhy_last
} eClusterLimitWhy;

/* THE NON-ZERO START CLOSES AN ABSORBING STATE.  loop' = published * t /
 * loadMax is multiplicative, so once the loop reaches 0 it can never leave:
 * the gate |S| >= published * bindFrac/1000 is trivially satisfied at 0 and
 * the update computes 0 * anything = 0 forever.  Hence chargeLoop_mA is NEVER
 * written to zero: the noParticipant and forbidden paths zero only the EMITTED
 * value, and a pack returning or a direction being re-permitted raises a
 * restart that re-seeds the loop. */

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
    cluMemFlag_bindingCharge    = 1u << 0,  /* it set loadMax this tick       */
    cluMemFlag_bindingDischarge = 1u << 1,
    cluMemFlag_socOutlier       = 1u << 2,
    cluMemFlag_shareOutlier     = 1u << 3,
    cluMemFlag_circulating      = 1u << 4,
    cluMemFlag_limitFell        = 1u << 5,
    cluMemFlag_joined           = 1u << 6,
    cluMemFlag_left             = 1u << 7,
    cluMemFlag_implausible      = 1u << 8,
    cluMemFlag_limitSaturated   = 1u << 9,  /* L_i == 0 with |I_i| > 0, so
                                               load_pm saturated at 1000
                                               rather than dividing           */
} eClusterMemberFlag;

/* ==========================================================================
 * The published snapshot — 112 bytes.
 * THE FOUR-NUMBER CAUSAL CHAIN per direction — loop -> derated -> slewed ->
 * published — is carried in full rather than derived, so an adapter renders
 * "why is the limit this" with NO arithmetic of its own.
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

    uint32_t chargeLoop_mA;         /* what the loop holds, pre-derate        */
    uint32_t dischargeLoop_mA;
    uint32_t chargeDerated_mA;
    uint32_t dischargeDerated_mA;
    uint32_t chargeSlewed_mA;       /* published == slewed UNLESS forbidden,
                                       when published is 0 and slewed still
                                       shows what the loop would have said    */
    uint32_t dischargeSlewed_mA;

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
    uint16_t chargeLoadMax_pm;      /* THE LOOP'S INPUT, exposed              */
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
    uint8_t  lastRestart;           /* eClusterRestart                        */
    uint8_t  rsvd[3];
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
    uint16_t load_pm;               /* the loop's whole input                 */
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
    uint32_t bindingSampleChgCnt;   /* ticks the loop actually learned from.
                                       EXPECT A SMALL FRACTION                */
    uint32_t bindingSampleDsgCnt;
    uint32_t restartChgCnt, restartDsgCnt;
    uint32_t stepClampedCnt, slewLimitedCnt;
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
const char *Cluster_RestartName(uint8_t reason);
const char *Cluster_LimitWhyName(uint8_t why);
const char *Cluster_AlarmName(uint32_t bit);
const char *Cluster_MemberFlagName(uint32_t bit);
const char *Cluster_FieldName(uint32_t bit);
void        Cluster_LogStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* CLUSTER_H_ */

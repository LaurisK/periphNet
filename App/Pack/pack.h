/**
 * @file    pack.h
 * @brief   PeriphNet battery pack module — THE public API.
 *
 * A pack is one configured, operator-named battery bound to a TYPE.  A type
 * knows one vendor protocol and translates its facts into neutral ones; the
 * core owns what every battery has in common — state, condition, the
 * staleness clock, confidence, persistence and command dispatch — and never
 * learns what a holding register or a CAN id is.
 *
 * A PACK DOES NOT KNOW IT IS IN A CLUSTER.  Membership, aggregation and "the
 * battery" appear nowhere here and never will: a pack reports its own state,
 * and the consumer decides what to consume (docs/design_battery_pack.md §3).
 *
 * WHAT THIS MODULE WILL NOT DO, so nobody proposes it again: a generic
 * write-through to the vendor's registers; a retry or an "assume it obeyed"
 * on a command whose outcome is unknown; a judgement about whether a pack has
 * been disconnected by something outside itself (§4); a borrowed pointer into
 * live state; a float.
 *
 * STATUS: IMPLEMENTED, NOT YET WIRED IN -- App_DefaultTaskEntry does not
 * call Func_Init/Func_Start, so nothing here runs on a board yet.  Every
 * declaration below is the contract from docs/design_battery_pack.md §10.
 * Where this header and that document disagree, the document wins.
 *
 * BOUNDARY (§9):
 *   - A consumer includes ONLY this header.  pack_type.h, pack_fsm.h and
 *     pack_cfg.h are module-internal.
 *   - Files in App/Pack must not include App/Mqtt, App/Http, App/Can,
 *     App/Data, lwIP, or any protocol header — except the two type files,
 *     which include their own protocol's headers and nothing else.
 *   - No float or double crosses this API.
 */

#ifndef PACK_H_
#define PACK_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Func/func.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types -----------------------------------------------------------*/

/* ==========================================================================
 * Bounds (§10)
 * ========================================================================== */

#define PACK_MAX            8u    /* instances.  NOT MB_MAX_DEVICES: a
                                     CAN-sourced pack spends no Modbus device
                                     slot, so the ceilings are unrelated     */
#define PACK_NAME_LEN      16u    /* including NUL                           */
#define PACK_BIND_LEN      24u    /* stored width of the `bind` token (§12).
                                     The token reaches a type as a borrowed
                                     const char *; this is the configuration
                                     record's field width, and the parser
                                     rejects anything longer                 */
#define PACK_CELLS_MAX     16u    /* raising to the JK's 32 costs 1.3 KB more
                                     .bss and no API change                  */
#define PACK_MAX_SUBS       8u    /* subscription table; §15 budgets it      */

/** age_ms of a group that has NEVER been delivered.  Distinct from 0, which
 *  means "delivered, this instant".  This is the one mechanism that separates
 *  "not read yet" from "read as zero"; there is no second validity mask. */
#define PACK_AGE_NEVER     0xFFFFFFFFu

/* ==========================================================================
 * Type and chemistry — PERSISTED in the pack configuration, so both are
 * APPEND-ONLY and NEVER RENUMBERED, the same rule as eNvDbUser and
 * eModbusDecodeType (§10.1).
 *
 * packType_sim is deliberately NOT allocated: §17 defers the test pack type,
 * and an append-only enum needs no reservation for it.
 * ========================================================================== */

typedef enum {
    packType_jkBms     = 0,
    packType_pylontech = 1,
    packType_last                   /* a count, never stored                 */
} ePackTypeId;

typedef enum {
    packChem_lfp       = 0,
    packChem_liIon     = 1,
    packChem_lto       = 2,
    packChem_last
} ePackChemistry;

/* ==========================================================================
 * Capabilities — what THIS INSTANCE reports (§10.2)
 *
 * A capability bit is set only when an accessor exists for it AND the live
 * configuration actually backs it.  A type declares what it COULD do; what an
 * instance advertises is what its bind() confirmed.
 *
 * The capability set governs FIELD VALIDITY: a field group listed against a
 * capability is meaningful only when that capability is set, otherwise it
 * reads zero and means nothing.  Age is a separate question, answered per
 * group by age_ms[].
 * ========================================================================== */

/** No cell.  Staging is zeroed at bind, so a plain 0 sentinel would read as
 *  "cell 0 is the balance source" — which is what it did. */
#define PACK_CELL_NONE          0xFFu

typedef enum {
    packCap_capacityAh      = 1u << 0,  /* remaining_mAh, capacity_mAh, soc  */
    packCap_learnedCapacity = 1u << 1,  /* capacity_mAh is MEASURED          */
    packCap_soh             = 1u << 2,
    packCap_temperatures    = 1u << 3,
    packCap_currentLimits   = 1u << 4,
    packCap_switchState     = 1u << 5,
    packCap_cellSummary     = 1u << 6,  /* cellCount, cellMax/Min_mV + idx   */
    packCap_cellDetail      = 1u << 7,  /* Pack_GetCells() -> per-cell mV    */
    packCap_leadResistance  = 1u << 8,  /* Pack_GetCells() -> leadRes_mOhm   */
    packCap_balancer        = 1u << 9,  /* Pack_GetCells() -> balance fields */
    packCap_cellEstimator   = 1u << 10, /* soc/soh/capacity are ESTIMATED
                                           here.  Reserved; no type sets it
                                           until the estimator lands         */
    /* BIT 11, NOT 10.  This was added on 1u << 10, which cellEstimator above
     * already held -- caught on hardware 2026-08-26.  Latent, because nothing
     * produced either bit yet; but two capabilities sharing one bit means a
     * consumer asking "can this pack report voltage limits" gets "yes" from
     * an estimator.  tests/test_pack_fsm.c now asserts all bits are distinct. */
    packCap_voltageLimits   = 1u << 11, /* charge/dischargeVoltLimit_mV      */
} ePackCap;

/* ==========================================================================
 * Commands — a dense id, and a bitmask built from it (§10.3)
 *
 * Two different things, deliberately not conflated: A COMMAND IS ONE ID (so a
 * multi-bit value cannot pass validation), A CAPABILITY SET IS A MASK.
 * PACK_CMD_BIT is the only bridge.
 * ========================================================================== */

typedef enum {
    packCmd_undefined       = 0,    /* an all-zero struct is not a command   */
    packCmd_chargeEnable    = 1,    /* value 0/1                             */
    packCmd_dischargeEnable = 2,    /* value 0/1                             */
    packCmd_balanceEnable   = 3,    /* value 0/1                             */
    packCmd_chargeLimit     = 4,    /* value mA                              */
    packCmd_dischargeLimit  = 5,    /* value mA                              */
    packCmd_last
} ePackCmdId;

#define PACK_CMD_BIT(id)   (1u << (uint32_t)(id))

/* A mask with two or more bits set must never collide with a valid id, or
 * passing a mask where an id belongs would command the wrong thing and pass
 * every check.  Ids start at 1 so the smallest two-bit mask exceeds the
 * largest id; this assert fails the build if that ever stops being true.
 * It currently sits EXACTLY on its boundary: a sixth command breaks the build.
 * That is the tripwire working.  The remedy is to widen the shift to
 * (1u << ((uint32_t)(id) + 3u)), not to delete the assert. */
_Static_assert(((1u << 1) | (1u << 2)) >= (uint32_t)packCmd_last,
               "a two-bit command mask aliases a valid ePackCmdId");

/* ==========================================================================
 * Field groups — the unit a timestamp is kept for (§10.4)
 *
 * ONE AGE FOR A WHOLE PACK IS A LIE.  A JK delivers the live electrical group
 * in a single transaction every 5 s and its cell voltages ~4-5 s behind; a
 * Pylontech pack delivers some frames of its set and not others.  So the
 * clock is per group, and the pack states its skew instead of hiding it.
 * ========================================================================== */

typedef enum {
    packGrp_electrical = 0, /* voltage_mV, current_mA — the liveness group   */
    packGrp_charge,         /* soc_pm, remaining_mAh, capacity_mAh, soh_pm   */
    packGrp_temperature,
    packGrp_limits,
    packGrp_switches,
    packGrp_alarms,
    packGrp_cells,          /* the summary here AND Pack_GetCells()          */
    packGrp_vendorInfo,
    packGrp_last
} ePackGroup;

#define PACK_GRP_BIT(g)    (1u << (uint32_t)(g))

/* ==========================================================================
 * Condition, switches, alarms, provenance (§10.5)
 * ========================================================================== */

typedef enum {
    packCond_absent = 0,    /* configured, has never answered                */
    packCond_stale,         /* answered before, not within its timeout       */
    packCond_online,
    packCond_last
} ePackCondition;

/** WHY a pack is not online.  Without this, "the battery is unreachable",
 *  "no device matches that address", "this firmware has no such type" and
 *  "bound, but no live plan covers it" all render identically as `absent`,
 *  and they are three different call-outs plus a configuration error. */
typedef enum {
    packWhy_none = 0,       /* it is online                                  */
    packWhy_noType,         /* config names a type nobody registered         */
    packWhy_typeUnavailable,/* registered but structurally cannot bind on this
                               build -- e.g. pack_pylontech before the CAN RX
                               dispatcher exists.  The operator's config is
                               correct; the firmware is the limitation        */
    packWhy_noBinding,      /* bindKey resolved to zero or several devices   */
    packWhy_notPolled,      /* bound, but no live plan covers it             */
    packWhy_noReply,        /* polled, silent                                */
    packWhy_last
} ePackAbsentReason;

typedef enum {
    packSwitch_unknown = 0, /* this type cannot report it — a first-class
                               value, not an error                           */
    packSwitch_open,
    packSwitch_closed,
    packSwitch_last
} ePackSwitch;

typedef enum {
    packAlarm_cellOverVoltage    = 1u << 0,
    packAlarm_cellUnderVoltage   = 1u << 1,
    packAlarm_packOverVoltage    = 1u << 2,
    packAlarm_packUnderVoltage   = 1u << 3,
    packAlarm_overTemperature    = 1u << 4,
    packAlarm_underTemperature   = 1u << 5,
    packAlarm_chargeOverCurrent  = 1u << 6,
    packAlarm_dischargeOverCur   = 1u << 7,
    packAlarm_cellImbalance      = 1u << 8,
    packAlarm_internalFault      = 1u << 9,
    packAlarm_protectionOpen     = 1u << 10, /* the pack's own protection has
                                                opened a contactor — §4's
                                                middle case, stated          */
} ePackAlarm;

typedef enum {
    packFlag_socEstimated   = 1u << 0,
    packFlag_sohEstimated   = 1u << 1,
    packFlag_capacityLearnt = 1u << 2,
    packFlag_bindVerified   = 1u << 3,  /* the pack read its OWN address back
                                           and it matched                    */
} ePackFlag;

/* ==========================================================================
 * The state (§10.6) — 132 bytes
 * ========================================================================== */

typedef struct {
    /* --- always valid ------------------------------------------------- */
    uint32_t        caps;               /* ePackCap, this instance          */
    uint32_t        cmds;               /* PACK_CMD_BIT set accepted.
                                           0 = read-only pack               */
    uint32_t        flags;              /* ePackFlag                        */
    uint32_t        alarms;             /* ePackAlarm                       */
    uint32_t        vendorAlarms[2];    /* the type's own raw words         */
    uint32_t        nameplate_mAh;      /* FROM CONFIGURATION, so valid even
                                           for a pack that never answered   */
    /* PER-GROUP AGE, PURELY INFORMATIONAL.  PACK_AGE_NEVER = never seen.
     *
     * THERE IS NO PER-GROUP "STALE" VERDICT, deliberately.  A group's natural
     * refresh period is a property of whatever transport plan reads it -- on a
     * JK, of the Modbus plan the operator configured -- and this module has no
     * way to know it.  Judging each group against a budget it invented made
     * `charge_current_max` "stale" fifteen seconds after a read, when it is a
     * SETTING that changes only when somebody writes it, and the pack was
     * answering perfectly well the whole time.
     *
     * STALENESS IS A PROPERTY OF THE PACK, NOT OF ITS ATTRIBUTES: if the pack
     * is talking, every register is being refreshed on its own schedule and
     * carries its own age here.  `cond` is the verdict; these are the facts.
     *
     * A CONSUMER THAT AGGREGATES MUST FILTER ON `cond` FIRST -- summing the
     * limits or the amp-hours of a pack that is not packCond_online is what
     * gives a cluster phantom capacity from a battery that left the bus. */
    uint32_t        age_ms[packGrp_last];

    /* --- packGrp_electrical ------------------------------------------- */
    uint32_t        voltage_mV;
    int32_t         current_mA;         /* + charge, - discharge            */

    /* --- packCap_capacityAh ------------------------------------------- */
    uint32_t        remaining_mAh;
    uint32_t        capacity_mAh;       /* usable; nameplate unless
                                           packFlag_capacityLearnt          */
    /* --- packCap_currentLimits ---------------------------------------- */
    /* THE OTHER HALF OF A PYLONTECH 0x351.  A cluster's first output frame
     * carries charge voltage, charge current, discharge current AND
     * discharge voltage; only the two currents were here, so the cluster
     * could not have been written without a breaking change to this struct.
     * Gated by packCap_voltageLimits, and members of packGrp_limits. */
    uint32_t        chargeVoltLimit_mV;
    uint32_t        dischargeVoltLimit_mV;
    uint32_t        chargeLimit_mA;
    uint32_t        dischargeLimit_mA;

    /* --- identity: always valid --------------------------------------- */
    char            name[PACK_NAME_LEN];

    /* --- SOC/SOH and how much to believe each ------------------------- */
    uint16_t        soc_pm;
    uint16_t        soh_pm;
    /* THE ESTIMATOR'S DISAGREEMENT WITH ITS OWN COULOMB COUNT at the last
     * anchor, per-mille (§24.4).  It is the error the plateau accumulated,
     * and the only measurement that bounds how wrong soc_pm can be while
     * between anchors.  0 when never anchored twice. */
    int16_t         socDrift_pm;
    uint16_t        socConf_pm;
    uint16_t        sohConf_pm;

    /* --- packCap_cellSummary / temperatures --------------------------- */
    uint16_t        cellMax_mV;
    uint16_t        cellMin_mV;
    int16_t         tempMax_dC;         /* 0.1 degC                         */
    int16_t         tempMin_dC;         /* 0.1 degC                         */

    /* --- small, always valid ------------------------------------------ */
    uint8_t         idx;
    uint8_t         typeId;             /* ePackTypeId                      */
    uint8_t         cond;               /* ePackCondition                   */
    uint8_t         cellCount;
    uint8_t         cellMaxIdx;
    uint8_t         cellMinIdx;
    uint8_t         chargeSwitch;       /* ePackSwitch                      */
    uint8_t         dischargeSwitch;    /* ePackSwitch                      */
    uint8_t         why;                /* ePackAbsentReason                */
} sPackState;

/* ==========================================================================
 * Per-cell detail (§10.7) — 80 bytes at 16 cells
 *
 * A separate call: ten times the size of the state, almost nobody wants it,
 * and it has ITS OWN CLOCK — on a JK it is 4-5 s behind the electrical group
 * and refreshed on a different plan.
 * ========================================================================== */

typedef struct {
    uint32_t        age_ms;             /* PACK_AGE_NEVER if never          */
    int32_t         balanceCurrent_mA;  /* packCap_balancer                 */
    uint16_t        cell_mV[PACK_CELLS_MAX];        /* packCap_cellDetail   */
    uint16_t        leadRes_mOhm[PACK_CELLS_MAX];   /* packCap_leadResistance */
    uint16_t        balanceDuty_pm;     /* packCap_balancer                 */
    uint8_t         cellCount;          /* entries actually filled          */
    uint8_t         balanceSrcIdx;      /* PACK_CELL_NONE = none            */
    uint8_t         balanceSinkIdx;     /* PACK_CELL_NONE = none            */
    uint8_t         balanceActive;
} sPackCells;

/* ==========================================================================
 * Results (§10.8)
 *
 * Negative, like eModbusErr; no _undefined = 0, for the same reason the rest
 * of the project omits one.  APPEND-ONLY — it crosses into consumers and onto
 * the HTTP surface.
 *
 * The last four are produced by a TYPE through PackType_CommandDone, not by
 * the core.  packErr_unknownOutcome is the core's, and only the core's.
 * ========================================================================== */

typedef enum {
    packErr_ok             =   0,
    packErr_badArg         =  -1,
    packErr_notFound       =  -2,
    packErr_notSupported   =  -3,   /* command not advertised by THIS pack   */
    packErr_outOfRange     =  -4,   /* outside the instance's bounds         */
    packErr_notOnline      =  -5,
    packErr_busy           =  -6,   /* a command is already in flight        */
    packErr_timeout        =  -7,   /* the type never completed in time      */
    packErr_unknownOutcome =  -8,   /* went stale mid-command                */
    packErr_refused        =  -9,   /* the transport refused it outright     */
    packErr_full           = -10,   /* subscription table                    */
    packErr_unprovisioned  = -11,
    packErr_transport      = -12,   /* the wire failed; nothing was decided  */
} ePackErr;

/* ==========================================================================
 * Events (§10.9)
 *
 * THE EVENT CARRIES NO STATE PAYLOAD — only what changed and for which
 * instance.  A subscriber wanting values calls Pack_GetState.  That removes
 * every borrowed-pointer lifetime rule the Modbus surface has to spell out.
 * ========================================================================== */

typedef enum {
    packEvt_state     = 1u << 0,  /* one or more field groups refreshed      */
    packEvt_condition = 1u << 1,
    packEvt_alarm     = 1u << 2,
    packEvt_command   = 1u << 3,
    packEvt_config    = 1u << 4,  /* every cached index is now suspect       */
    packEvt_released  = 1u << 5,
    packEvt_all       = 0x3Fu,
} ePackEventType;

typedef struct {
    ePackEventType type;
    uint32_t       tick_ms;
    const char    *name;        /* BORROWED for the call — saves every
                                   subscriber a lookup, creates no lifetime
                                   rule, dies with the call                  */
    uint8_t        idx;
    union {
        struct { uint32_t groups; }         state;      /* PACK_GRP_BIT     */
        struct { uint8_t  from; uint8_t to; } condition;
        struct { uint32_t added; uint32_t cleared; } alarm;
        struct { uint8_t cmd; int32_t value; int16_t result; } command;
    } u;
} sPackEvent;

typedef void (*fPackSubscriber)(const sPackEvent *ev, void *ctx);

/* ==========================================================================
 * Commanding (§10.9)
 * ========================================================================== */

typedef struct {
    int32_t     value;      /* scaled per command; 0/1 for booleans          */
    ePackCmdId  cmd;
    const char *origin;     /* "cluster", "cli", "http".  Borrowed for the
                               call; the core copies what it logs            */
} sPackCommand;

typedef void (*fPackCmdDone)(uint8_t idx, const sPackCommand *cmd,
                             int16_t result, void *ctx);

#define PACK_CMD_TIMEOUT_MAX_MS  60000u

/* ==========================================================================
 * Configuration (§10.10)
 *
 * Same streaming byte-source contract as the Modbus compiler, so the HTTP
 * body cursor already in http_server.c drives it unchanged.
 * ========================================================================== */

typedef int (*fPackByteSource)(void *ctx, uint8_t *buf, uint32_t maxLen);
typedef int (*fPackByteSink)  (void *ctx, const char *data, uint32_t len);

typedef struct {
    int      ok;
    int      packIdx;           /* first failure location; -1 = n/a          */
    char     field[24];         /* offending key, or "json" for syntax       */
    char     reason[64];
    uint8_t  packs;             /* how many parsed before the failure        */
} sPackCfgResult;

/* ==========================================================================
 * Diagnostics (§10.11)
 * ========================================================================== */

typedef struct {
    uint32_t updates;       /* state commits                                 */
    uint32_t cmdAccepted;
    uint32_t cmdOk;
    uint32_t cmdFailed;
    uint32_t cmdUnknown;    /* went stale mid-command — the one to watch     */
    uint32_t staleEvents;   /* online -> stale transitions                   */
    uint32_t cmdRefused;    /* refused synchronously, before any wire
                               traffic -- the counter that says consumers
                               are asking for things this pack will not do */
    uint32_t bindFailures;  /* instances that could not bind at all; pairs
                               with ePackAbsentReason to answer "why is
                               this pack absent"                          */
    uint32_t lateCompletes; /* a type answered AFTER the core gave up        */
    uint8_t  provisioned;
} sPackStats;

/* ==========================================================================
 * The shared functionality task's view of this module (§13)
 * ========================================================================== */

/** Event ids this module occupies in the shared task's packed space.  FOR
 *  App/Func/func.c ONLY — a consumer never sees an internal event id, and the
 *  count is exported rather than the enum so the two cannot drift. */
#define PACK_EVT_COUNT     8u

/* The shared task's post function is fFuncPost, declared ONCE in
 * App/Func/func.h and included above.  The module never sees the queue or the
 * task itself, only its event-id base and this. */

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Bring the pack module up: load the configuration, build every
 *         instance, bind each to its registered type, and arm the staleness
 *         clocks.
 *
 * WHEN IMPLEMENTED it must: read nvdbUser_packCfg through NvRecord_Load and
 * treat "nothing valid stored" AND NvDb_GetSize() == 0 as UNPROVISIONED, not
 * as a fault (§14 item 2: the first boot after the OTA that introduces packs
 * has no space yet, the second one does); create every configured instance
 * with condition packCond_absent; call each type's bind(); and set caps/cmds
 * from what bind() confirmed and from nothing else.
 *
 * Called by App/Func/func.c and by NOTHING ELSE — the module is handed its
 * event-id base and a post function and never sees the queue or the task.
 * Requires NvDb_Init().  ZERO PACKS IS A VALID, FIRST-CLASS STATE, reported
 * as unprovisioned rather than as an error.
 *
 * @param  evtIdBase - first packed event id of this module's contiguous range
 * @param  post - the shared task's post function
 * @retval packErr_ok, or packErr_badArg when post is NULL
 * @note   Shared functionality task, at init.  May block briefly (bind walks
 *         a Modbus catalogue from flash).
 */
int Pack_Init(uint16_t evtIdBase, fFuncPost post);

/**
 * @brief  Dispatch one of the module's internal events.
 *
 *         FOR App/Func/func.c ONLY, like Pack_Init.  A consumer never calls
 *         it and never learns what a local event id means: func.c strips its
 *         base and hands over the remainder, so the shared task stays
 *         ignorant of the module and the module stays ignorant of the queue.
 *
 * @param  localEvt - the id with func.c's base already subtracted
 * @param  arg - the event's payload word; an instance index for most
 * @note   Runs on the shared functionality task.  Must not block.
 */
void Pack_HandleEvent(uint16_t localEvt, void *arg);

/**
 * @brief  THE TICK.  Evaluate every instance's condition and expire command
 *         deadlines.
 *
 *         FOR App/Func/func.c ONLY.  This is the half of the contract that
 *         SILENCE needs: a pack that stops answering posts nothing, so the
 *         only thing that can notice is something which runs anyway.
 *
 * @param  now_ms - the shared task's monotonic clock
 * @note   Runs on the shared functionality task.  Must not block.
 */
void Pack_Tick(uint32_t now_ms);

/**
 * @brief  How many pack instances the live configuration declares.
 * @retval 0 when unprovisioned — a valid state, not an error
 * @note   Any task; not ISR-callable.  Never blocks.
 */
int Pack_Count(void);

/**
 * @brief  Resolve an operator-assigned name to an instance index.
 *
 * NAME IS THE IDENTITY.  An index is a position and may change at a config
 * reload; a name may not.  A consumer caching an index re-resolves it on
 * packEvt_config.
 *
 * @param  name - NUL-terminated, matched exactly
 * @retval >= 0 the index, packErr_notFound, or packErr_badArg
 * @note   Any task; not ISR-callable.  Never blocks.
 */
int Pack_FindByName(const char *name);

/**
 * @brief  Copy one instance's whole state.
 *
 * COPIES, so there are no borrowed pointers and no lifetime rules.  age_ms[]
 * are computed inside the SAME critical section as the copy,
 * so an age can never disagree with the value it describes.
 *
 * WHEN IMPLEMENTED: a field group whose capability is clear reads zero and
 * MEANS NOTHING (§10.2); a group never delivered carries PACK_AGE_NEVER.
 *
 * @param  idx - instance index, < Pack_Count()
 * @param  out - destination, caller-owned
 * @retval packErr_ok, packErr_notFound, packErr_badArg
 * @note   Any TASK, NOT an ISR — it takes a task-context critical section.
 *         Never blocks; masks interrupts for well under 1 us.
 */
int Pack_GetState(uint8_t idx, sPackState *out);

/**
 * @brief  Copy one instance's per-cell detail, which has its own clock.
 * @param  idx - instance index
 * @param  out - destination, caller-owned
 * @retval packErr_ok, packErr_notSupported when the instance advertises no
 *         cell capability, packErr_notFound, packErr_badArg
 * @note   Any TASK, NOT an ISR.  Never blocks.
 */
int Pack_GetCells(uint8_t idx, sPackCells *out);

/**
 * @brief  Subscribe to pack events.
 *
 * DOES NOT POST, deliberately: a posted subscribe cannot return "table full",
 * which is a real init-time error.
 *
 * Callbacks run on the shared functionality task, synchronously, and MUST NOT
 * BLOCK — same reasoning and same remedy as docs/modbus.md §4.7.  ORDERING:
 * the core commits the live slot BEFORE it dispatches, so a subscriber
 * calling Pack_GetState from inside its callback is guaranteed to see at
 * least the update the event announced.
 *
 * @param  evtMask - ePackEventType bits wanted; packEvt_released is delivered
 *                   regardless
 * @param  cb - the callback; must not block, must not be NULL
 * @param  ctx - passed back unchanged
 * @retval >= 0 a handle, packErr_full, packErr_badArg
 * @note   Any task; not ISR-callable.  Never blocks.
 */
int Pack_Subscribe(uint32_t evtMask, fPackSubscriber cb, void *ctx);

/**
 * @brief  Release a subscription.
 *
 * POSTS, and is legal from inside a callback.  THE FINAL packEvt_released
 * CALL IS THE RELEASE POINT — delivered regardless of evtMask, and after it
 * returns the module never calls again and ctx may be freed.
 *
 * @param  handle - from Pack_Subscribe
 * @retval packErr_ok, packErr_notFound, packErr_badArg
 * @note   Any task, including from inside a callback.  Never blocks.
 */
int Pack_Unsubscribe(int handle);

/**
 * @brief  Issue one command to one pack.
 *
 * ASYNCHRONOUS.  packErr_ok means ACCEPTED, not done: the command reaches the
 * wire later and its outcome arrives through @p done.  Anything else is a
 * synchronous refusal and @p done does NOT fire.
 *
 * The refusal rules, all safety-motivated (§6, §10.9), every one of them
 * applied BEFORE anything reaches a wire:
 *   1. not advertised (!(cmds & PACK_CMD_BIT(cmd)))  -> packErr_notSupported
 *   2. outside the instance's bounds — THERE IS NO CLAMPING -> packErr_outOfRange
 *   3. not packCond_online                           -> packErr_notOnline
 *   4. one already in flight for this pack           -> packErr_busy
 *   5. going stale mid-command completes packErr_unknownOutcome — never
 *      "assume it obeyed"
 *   6. @p done ALWAYS fires for an accepted command, within timeout_ms plus
 *      one tick, whatever the type does or fails to do
 *   7. every command is logged with origin, value and result, and counted
 *   8. a configuration change — this module's or the Modbus module's —
 *      completes it packErr_unknownOutcome
 *
 * CONSEQUENCE, stated so no consumer invents its own: rules 3 and 4 mean the
 * module WILL NOT HOLD INTENT ACROSS A STALE PERIOD.  Re-asserting a command
 * when a pack returns is the CONSUMER'S policy, deliberately not this
 * module's.
 *
 * @param  idx - instance index
 * @param  cmd - the command; borrowed for the call, including cmd->origin
 * @param  timeout_ms - 1..PACK_CMD_TIMEOUT_MAX_MS.  0 IS REJECTED — an
 *                      unbounded deadline is the same as no guarantee
 * @param  done - completion callback; runs on the shared task, must not block
 * @param  ctx - passed back unchanged
 * @retval packErr_ok (accepted), or one of the refusals above
 * @note   Any task; not ISR-callable.  Validates and claims synchronously,
 *         posts the wire work.  Never blocks.
 */
int Pack_Command(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms,
                 fPackCmdDone done, void *ctx);

/**
 * @brief  The bounds this instance accepts for one command.
 *
 * Exists so an HA number entity, a CLI or the future cluster can render the
 * limits without guessing.  Both bounds are inclusive and in the command's
 * own scaled unit; booleans report {0, 1}.
 *
 * @param  idx - instance index
 * @param  cmd - the command id
 * @param  min - out, inclusive lower bound
 * @param  max - out, inclusive upper bound
 * @retval packErr_ok, packErr_notSupported when the instance does not
 *         advertise it, packErr_notFound, packErr_badArg
 * @note   Any task; not ISR-callable.  Never blocks.
 */
int Pack_CommandBounds(uint8_t idx, ePackCmdId cmd, int32_t *min, int32_t *max);

/**
 * @brief  Validate a pack configuration document WITHOUT writing anything.
 *
 * ONE PARSER, ONE PASS, ONE RESULT STRUCT: Verify and Apply differ only in
 * whether they commit, so there is never a second validator that can disagree
 * with the first.
 *
 * @param  src - streaming byte source, the HTTP body cursor in practice
 * @param  srcCtx - passed to @p src
 * @param  res - filled on success AND on failure; res->field names the
 *               offending key and res->packIdx the offending pack
 * @retval packErr_ok, packErr_badArg
 * @note   The CALLER'S task (http), not posted — an upload takes seconds and
 *         posting it would stall the shared task.  MAY BLOCK.
 */
int Pack_ConfigVerify(fPackByteSource src, void *srcCtx, sPackCfgResult *res);

/**
 * @brief  Parse, commit and adopt a pack configuration.
 *
 * WHEN IMPLEMENTED it must rebuild every instance, unbind and rebind every
 * type, persist to nvdbUser_packCfg, and raise packEvt_config.  OUTSTANDING
 * COMMANDS COMPLETE packErr_unknownOutcome — the binding they were issued
 * against no longer exists, so they are neither dropped nor assumed to have
 * landed.  This is refusal path eight of §10.9.
 *
 * @param  src - streaming byte source
 * @param  srcCtx - passed to @p src
 * @param  res - filled on success AND on failure
 * @retval packErr_ok, packErr_badArg, packErr_unprovisioned when nvDb has no
 *         space for nvdbUser_packCfg yet (§14 item 2)
 * @note   The caller's task (http).  MAY BLOCK.
 */
int Pack_ConfigApply(fPackByteSource src, void *srcCtx, sPackCfgResult *res);

/**
 * @brief  Re-serialise the active configuration as JSON.
 *
 * Data-faithful, not byte-identical — the same contract as
 * GET /api/modbus/config/download.
 *
 * @param  sink - streaming byte sink
 * @param  ctx - passed to @p sink
 * @retval packErr_ok, packErr_unprovisioned, packErr_badArg
 * @note   The caller's task (http).  MAY BLOCK.
 */
int Pack_ConfigExport(fPackByteSink sink, void *ctx);

/**
 * @brief  Erase the pack configuration; the board becomes UNPROVISIONED.
 *
 * Erase, not Reset: with no built-in default there is nothing to reset TO.
 * Unbinds every type and completes every outstanding command
 * packErr_unknownOutcome, exactly as Pack_ConfigApply does.
 *
 * @retval packErr_ok
 * @note   The caller's task (http).  MAY BLOCK.
 */
int Pack_ConfigErase(void);

/**
 * @brief  Copy the module's lifetime counters.
 * @param  out - destination
 * @retval packErr_ok, packErr_badArg
 * @note   Any TASK, NOT an ISR — copies under the same critical section.
 */
int Pack_Stats(sPackStats *out);

/* ==========================================================================
 * Per-type statistics (§23.1)
 *
 * WHAT A PACK KNOWS ABOUT ITSELF, and that set differs per vendor: a JK
 * exposes MOS temperature, precharge state and lifetime throughput; a
 * Pylontech-speaking pack will expose something else entirely.
 *
 * A GENERIC LIST, deliberately, not a union per type.  The cluster reads the
 * typed sPackState fields and ignores all of this; a UI, MQTT bridge or CLI
 * walks the list and renders whatever is there with no per-vendor code.  That
 * is what keeps §2's rule -- a consumer never learns which vendor is behind a
 * pack -- true for the one consumer that actually aggregates.
 *
 * DESCRIPTIVE, NEVER LOAD-BEARING.  Nothing in this module may read a
 * statistic back to make a decision; that is what the typed fields are for.
 * A statistic something depends on is a field in the wrong place.
 * ========================================================================== */

/* Longest interval still counted as observed.  Beyond it we assume we stopped
 * looking rather than that nothing happened. */
/* Near-rest samples per resolved anchor.  The value is in the averaging:
 * one 2 mV/%% sample is +/-1.5 %% SOC, and only sqrt(N) makes it usable. */
#define PACK_SOC_ANCHOR_EVERY   120u

#define PACK_BAL_MAX_GAP_MS     60000u

#define PACK_STAT_NAME_LEN      24u
#define PACK_STATS_MAX          16u

typedef enum {
    packStatFlag_none    = 0u,
    packStatFlag_counter = 1u << 0,  /* monotonic; a delta is meaningful     */
    packStatFlag_vendor  = 1u << 1,  /* raw vendor value, not normalised     */
} ePackStatFlag;

typedef struct {
    char     name[PACK_STAT_NAME_LEN];
    int32_t  value;             /* SCALED INTEGER -- no float crosses this
                                   API, here as anywhere else               */
    uint16_t unit;              /* MB_UNIT_*, shared with the Modbus module
                                   so one renderer serves both              */
    int8_t   scale_pow10;       /* real value = value x 10^scale_pow10       */
    uint8_t  flags;             /* ePackStatFlag                            */
} sPackStat;

/**
 * @brief  How many statistics this pack publishes.
 * @param  idx - instance index
 * @retval count, or a negative ePackErr
 * @note   Any task.  Never blocks.
 */
int Pack_StatCount(uint8_t idx);

/**
 * @brief  Read one statistic.
 * @param  idx - instance index
 * @param  n - 0 .. Pack_StatCount()-1
 * @param  out - filled on success
 * @retval packErr_ok, packErr_badArg, packErr_notFound
 * @note   Any task.  Never blocks.  Copied under the core lock, so a type
 *         republishing mid-read cannot tear the entry.
 */
int Pack_StatGet(uint8_t idx, uint8_t n, sPackStat *out);

/* ==========================================================================
 * Balance transfer accounting (§23.3)
 *
 * The balancer is the ONE thing that breaks the shared-coulomb identity: every
 * cell in a series string carries the same current except for what the
 * balancer moves between two of them.  Measuring that is what lets a capacity
 * deviation be separated from a mere state-of-charge offset -- see §22, where
 * a single voltage snapshot could rank cells but could not convict one.
 * ========================================================================== */

typedef struct {
    /* Charge the balancer has moved, per cell, in MILLIAMP-SECONDS.  mAs
     * rather than mAh so the sample path never divides; a consumer divides by
     * 3600 once, at the point of display. */
    int32_t  in_mAs[PACK_CELLS_MAX];    /* added TO this cell               */
    int32_t  out_mAs[PACK_CELLS_MAX];   /* taken FROM this cell             */

    /* net = in - out.  A cell the balancer keeps having to CHARGE is below
     * the pack in capacity, so capacityDelta carries the OPPOSITE sign of
     * net transfer.  RELATIVE TO THE PACK MEDIAN, never an absolute cell
     * capacity -- that still needs the two-knee measurement of §22. */
    int32_t  capacityDelta_mAh[PACK_CELLS_MAX];

    uint32_t window_sec;        /* how long this accumulation has run       */
    uint32_t activeSamples;     /* intervals with the balancer actually on  */
    uint8_t  cellCount;
} sPackBalanceStats;

/**
 * @brief  Read the balance-transfer accumulation.
 * @param  idx - instance index
 * @param  out - filled on success
 * @retval packErr_ok, packErr_notSupported when the type has no balancer,
 *         packErr_badArg
 * @note   Any task.  Copied under the core lock.
 */
int Pack_BalanceStats(uint8_t idx, sPackBalanceStats *out);

/**
 * @brief  Zero the accumulation and start a fresh measurement window.
 * @param  idx - instance index
 * @retval packErr_ok, packErr_badArg
 * @note   Any task.  The operator's call: these totals are meaningful only
 *         against a stated window.
 */
int Pack_BalanceReset(uint8_t idx);

/* ==========================================================================
 * Per-cell SOC and capacity (§26)
 *
 * A SEPARATE STRUCT, NOT PART OF sPackCells, and deliberately: sPackCells is
 * the TYPE's staging buffer and is held twice per instance (live + staging).
 * This is derived by the CORE from published values, so putting it there
 * would have paid for a second copy that nothing ever writes -- 2 KB of main
 * SRAM on an 8-instance build, for nothing.
 * ========================================================================== */

typedef struct {
    /* THAT CELL's state of charge, not the pack's.  Cells share the string
     * current but not their capacities, so a small cell moves further per
     * amp-hour and reaches its limits first -- which is the entire reason a
     * pack-level SOC cannot answer "which cell will fail". */
    int16_t  soc_pm[PACK_CELLS_MAX];        /* -1 = not anchored yet        */

    /* MEASURED between two anchors: C_i = dQ_i / dSOC_i, where dQ_i differs
     * per cell only by the balance transfer.  An ABSOLUTE amp-hour figure,
     * unlike sPackBalanceStats.capacityDelta_mAh, which is a deviation from
     * the pack median.  0 until the measurement has been possible. */
    int32_t  capacity_mAh[PACK_CELLS_MAX];
    uint16_t capConf_pm[PACK_CELLS_MAX];

    int8_t   weakestIdx;            /* by measured capacity; -1 = unknown   */
    uint8_t  measuredCount;         /* how many cells have a capacity yet   */
    uint8_t  cellCount;
} sPackCellEstimate;

/**
 * @brief  Per-cell SOC and measured capacity.
 * @param  idx - instance index
 * @param  out - filled on success
 * @retval packErr_ok, packErr_notSupported when the type reports no cells,
 *         packErr_badArg
 * @note   Any task.  Copied under the core lock.
 */
int Pack_GetCellEstimate(uint8_t idx, sPackCellEstimate *out);

/* --- names, for a UI ------------------------------------------------------
 *
 * Thin forwarders over pack_cfg.c's tables.  They exist so a consumer can
 * render a type and parse a command name WITHOUT including pack_cfg.h, which
 * is module-internal and which CMake rule 6 refuses (§9).  Rendering a pack
 * and accepting a named command are ordinary consumer needs; reaching into
 * the configuration parser to do it is not.
 * ------------------------------------------------------------------------ */

/**
 * @brief  The config key for an ePackTypeId ("jkbms", "pylontech").
 * @param  typeId - ePackTypeId, e.g. sPackState.typeId
 * @retval the key, or NULL when no such type
 * @note   Pure.  Any task.
 */
const char *Pack_TypeName(uint8_t typeId);

/**
 * @brief  Map a command name ("chargeLimit", ...) to an ePackCmdId.
 * @param  name - NUL-terminated
 * @param  out - the id, written only on success
 * @retval packErr_ok, packErr_notFound, packErr_badArg
 * @note   Pure.  Any task.
 */
int Pack_CmdIdFromName(const char *name, ePackCmdId *out);

/**
 * @brief  The name for an ePackCmdId — the reverse of Pack_CmdIdFromName.
 * @param  cmd - ePackCmdId
 * @retval the name, or NULL when no such command
 * @note   Pure.  Any task.
 */
const char *Pack_CmdName(ePackCmdId cmd);

/**
 * @brief  Log one line per instance plus the counters — `pack status` on the
 *         CLI.
 * @note   Any task; uses Trice, so NEVER from an lwIP callback.  May block on
 *         the Trice buffer.
 */
void Pack_LogStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* PACK_H_ */

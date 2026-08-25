/**
 * @file    pack_type.h
 * @brief   The blueprint contract — included by TYPES, never by consumers
 *          (docs/design_battery_pack.md §11).
 *
 * A type knows one protocol and one map; it never sees the queue, the task,
 * the subscription table, a lock, or another type.
 *
 * THE SEAM THIS EXISTS TO CREATE: a pull type publishes from the Modbus
 * callback (modbus task) and a push type publishes from its CAN RX ISR, and
 * THE CORE CANNOT TELL THEM APART.  That is what makes one core serve both.
 *
 * A TYPE MAY NOT TAKE A LOCK.  The ban is mechanical, not advisory: a lock is
 * a context decision, and a type does not know its context statically — the
 * same pack_pylontech.c code is reachable from an ISR and from a task.  So a
 * type is simply not allowed to have one, and the class of error becomes
 * unrepeatable rather than merely fixed.  taskENTER_CRITICAL,
 * taskDISABLE_INTERRUPTS and __disable_irq are banned in pack_jkbms.c and
 * pack_pylontech.c.
 *
 * STATUS: SCAFFOLDING.  The contract is complete; no implementation is behind
 * it.
 */

#ifndef PACK_TYPE_H_
#define PACK_TYPE_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types -----------------------------------------------------------*/

/** One command's accepted domain.  Inclusive at both ends; booleans use
 *  {0, 1} and the core bounds-checks them like anything else, which is what
 *  stops a stray 2 from reaching a contactor. */
typedef struct {
    int32_t     min_scaled;
    int32_t     max_scaled;
    ePackCmdId  cmd;
} sPackCmdBound;

/** What the core tells a type at bind time.
 *
 * NOTE WHAT IS NOT HERE: no devOrd, no array position, no ordinal of any
 * kind.  bindKey is an opaque, TYPE-parsed token (§12) naming a PHYSICAL
 * address, because a devOrd is a position in another module's array and
 * inserting a device ahead of it silently rebinds every pack after it to a
 * different battery. */
typedef struct {
    const char *name;               /* borrowed for the call only            */
    const char *bindKey;            /* borrowed for the call only, §12       */
    uint32_t    nameplate_mAh;
    uint32_t    staleAfter_ms;      /* the electrical group's budget         */
    uint32_t    cellStaleAfter_ms;  /* the cell group's, which differs       */
    uint32_t    cmdAllow;           /* PACK_CMD_BIT set the OPERATOR permits;
                                       the type may NARROW, NEVER WIDEN      */
    uint8_t     idx;
    uint8_t     cellCount;
    uint8_t     chemistry;          /* ePackChemistry, for the estimator     */
} sPackBindInfo;

/** What the type answers with — CONFIRMED against the live transport, never
 *  declared statically.  Advertising a capability with no accessor, or a
 *  command against a config carrying no writable point, is how a consumer
 *  learns to distrust the whole API (§16 item 10). */
typedef struct {
    uint32_t             caps;      /* confirmed against the live transport  */
    uint32_t             cmds;
    const sPackCmdBound *bounds;    /* one per bit in cmds; STORAGE IS THE
                                       TYPE'S and must outlive the bind      */
    uint8_t              boundCount;
} sPackBindResult;

/** The blueprint one type registers exactly once. */
typedef struct {
    const char *name;               /* config key: "jkbms", "pylontech"      */
    ePackTypeId id;
    uint32_t    capsMax;            /* the most any instance could offer     */
    uint32_t    cmdsMax;
    uint32_t    defaultStaleAfter_ms;
    uint32_t    defaultCellStaleAfter_ms;
    const sPackCmdBound *ceiling;   /* the widest bounds ANY instance of this
                                       type could accept.  The config parser
                                       rejects a `commands` bound wider than
                                       this; it is the only numeric ceiling
                                       in the system                          */
    uint8_t     ceilingCount;
    uint8_t     maxInstances;       /* 0 = up to PACK_MAX.  A transport that
                                       cannot distinguish two of its packs
                                       says so HERE rather than letting a
                                       config declare three of them and
                                       having them overwrite each other      */

    /**
     * @brief Resolve bindKey against the live transport and confirm what this
     *        instance can actually do.
     * @retval packErr_ok, or a refusal.  A BIND THAT FAILS LEAVES THE
     *         INSTANCE packCond_absent WITH caps = 0 — a reportable state,
     *         not a boot failure.
     * @note  SHARED FUNC TASK ONLY.  MAY BLOCK BRIEFLY — walking a Modbus
     *        point catalogue is flash I/O, and a bind is a control path.
     */
    int  (*bind)  (const sPackBindInfo *info, sPackBindResult *res);

    /**
     * @brief Drop every resource this instance holds; no completion follows.
     * @note  SHARED FUNC TASK ONLY.  May block briefly.
     */
    int  (*unbind)(uint8_t idx);

    /**
     * @brief Put one already-validated command on the wire.
     *
     * ASYNCHRONOUS, AND ITS RETURN VALUE IS ACCEPTANCE ONLY.  packErr_ok
     * means the type has taken it; the outcome must later arrive through
     * PackType_CommandDone(), EXACTLY ONCE, within timeout_ms.  Anything else
     * is a refusal and NO COMPLETION FOLLOWS.
     *
     * The core already validated capability and bounds; A TYPE MUST NOT
     * RE-CHECK THEM AND MUST NOT CLAMP.
     *
     * @note  SHARED FUNC TASK ONLY.  May block briefly.
     */
    int  (*submit)(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms);

    /**
     * @brief Optional periodic hook, ~4 Hz on the shared task.  NULL is legal.
     *
     * Where a PUSH type closes a partial frame set that stopped arriving, and
     * where a PULL type notices its transport went quiet.
     *
     * @note  SHARED FUNC TASK.  MUST NOT BLOCK.
     */
    void (*tick)  (uint32_t now_ms);
} sPackType;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Register one blueprint.  Called once per type, before Pack_Init
 *         binds anything.
 * @param  type - static storage; the core keeps the pointer
 * @retval packErr_ok, packErr_full, packErr_badArg, packErr_busy when this
 *         id is already registered
 * @note   Shared func task, at init.  Never blocks.
 */
int PackType_Register(const sPackType *type);

/* ==========================================================================
 * Delivering an update (§11.1)
 *
 * sPackRaw is sPackState MINUS everything the core owns — name, idx,
 * condition, ages, staleness, confidence caps, capability confirmation.  One
 * staging slot per instance, SINGLE-WRITER BY CONSTRUCTION (its owning
 * instance), and the core touches it only inside the same lock.
 * ========================================================================== */

typedef struct {
    uint32_t voltage_mV;
    int32_t  current_mA;
    uint32_t remaining_mAh;
    uint32_t capacity_mAh;
    uint32_t chargeLimit_mA;
    uint32_t dischargeLimit_mA;
    uint32_t alarms;                /* ePackAlarm                            */
    uint32_t vendorAlarms[2];
    uint32_t flags;                 /* ePackFlag the TYPE owns               */
    uint16_t soc_pm;
    uint16_t soh_pm;
    uint16_t socConf_pm;            /* the type's honest opinion; THE CORE   */
    uint16_t sohConf_pm;            /* ONLY EVER LOWERS IT                   */
    uint16_t cellMax_mV;
    uint16_t cellMin_mV;
    int16_t  tempMax_dC;
    int16_t  tempMin_dC;
    uint8_t  cellMaxIdx;
    uint8_t  cellMinIdx;
    uint8_t  chargeSwitch;          /* ePackSwitch                           */
    uint8_t  dischargeSwitch;       /* ePackSwitch                           */
} sPackRaw;

/**
 * @brief  The instance's staging slot, to accumulate into before publishing.
 * @param  idx - instance index
 * @retval the slot, or NULL if the instance is unbound
 * @note   Any context.  Never blocks.  The slot is this instance's alone.
 */
sPackRaw *PackType_Staging(uint8_t idx);

/**
 * @brief  The instance's cell staging slot.
 * @param  idx - instance index
 * @retval the slot, or NULL if the instance advertises no cell capability
 * @note   Any context.  Never blocks.
 */
sPackCells *PackType_CellStaging(uint8_t idx);

/**
 * @brief  Commit staging into the live slot and announce it.
 *
 * LEGAL FROM ANY CONTEXT, INCLUDING AN ISR.  It is the one place that chooses
 * between the task- and ISR-context critical section and between the task-
 * and ISR-context queue post — exactly as func.c chooses between its two post
 * macros.  It never blocks and copies ~90 bytes with interrupts masked
 * (~0.5 us).
 *
 * @param  idx - instance index
 * @param  groups - a PACK_GRP_BIT set of WHAT THIS COMMIT ACTUALLY REFRESHED.
 *                  A type that got only half a frame set says so, and THE
 *                  GROUPS IT DID NOT NAME KEEP THEIR PREVIOUS AGE — which is
 *                  what makes a partial delivery visible instead of silent.
 * @note   Any context.  MUST NOT BLOCK.
 */
void PackType_Publish(uint8_t idx, uint32_t groups);

/**
 * @brief  Commit cell staging.  Separate because the cell group has its own
 *         clock — on a JK it runs 4-5 s behind the electrical group.
 * @param  idx - instance index
 * @note   Any context.  MUST NOT BLOCK.
 */
void PackType_PublishCells(uint8_t idx);

/**
 * @brief  The type -> core completion path for one submitted command.
 *
 * LEGAL FROM ANY CONTEXT — a Modbus type calls it from fModbusReqDone on the
 * modbus task.  A completion for a command the core has ALREADY FINISHED
 * (timeout expired, pack went stale, configuration replaced) is DISCARDED AND
 * COUNTED as sPackStats.lateCompletes.  A late answer must never be written
 * into a decision the consumer has already been told about.
 *
 * @param  idx - instance index
 * @param  result - packErr_ok, packErr_refused (the pack answered and
 *                  rejected it), packErr_transport (the wire failed, nothing
 *                  was decided) or packErr_timeout (the type gave up).
 *                  packErr_unknownOutcome is the CORE'S and must never be
 *                  passed here.
 * @note   Any context.  MUST NOT BLOCK.
 */
void PackType_CommandDone(uint8_t idx, ePackErr result);

/**
 * @brief  Report a transport fact that is not a measurement.
 *
 * The pack answered with an exception, the port has no driver, the frame set
 * arrived incomplete.  It feeds condition and confidence WITHOUT FAKING A
 * VALUE, which is why it is not a publish.
 *
 * @param  idx - instance index
 * @param  answered - non-zero if the pack answered at all
 * @note   Any context.  MUST NOT BLOCK.
 */
void PackType_NoteLiveness(uint8_t idx, int answered);

#ifdef __cplusplus
}
#endif

#endif /* PACK_TYPE_H_ */

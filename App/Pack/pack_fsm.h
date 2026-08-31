/**
 * @file    pack_fsm.h
 * @brief   Condition, staleness, confidence and command validation — the pure
 *          part (docs/design_battery_pack.md §9, §17).
 *
 * LIBC ONLY.  NO FreeRTOS, NO HAL, NO Trice, NO flash.  Everything here is a
 * pure function of (previous state, recorded group ticks, now_ms), which is
 * why tests/ compiles this file directly.  App/Net/wg_conf.c is the precedent.
 *
 * TIME IS ALWAYS AN ARGUMENT, never read.  Every function that needs the
 * clock takes now_ms, in the same monotonic millisecond base the core uses
 * (osKernelGetTickCount() on the board, whatever the test says on the host).
 * Wrap at 2^32 ms (~49.7 days) is handled by unsigned subtraction throughout;
 * nothing here compares two absolute timestamps directly.
 *
 * THE ONE RULE THAT SHAPES THE WHOLE FILE: SILENCE IS NOT AN EVENT.  A pack
 * that stops answering posts nothing, so a condition transition can only be
 * decided by something that runs anyway — the tick.  PackFsm_NotePublish and
 * PackFsm_NoteLiveness RECORD EVIDENCE and never change the condition;
 * PackFsm_Evaluate is the ONLY function that moves it.  The core calls
 * Evaluate immediately after every commit and once per 250 ms tick, so the
 * two paths meet at exactly one decision point.
 *
 * STATUS: IMPLEMENTED and host-tested (tests/test_pack_fsm.c).  Pure, libc
 * only, wrap-safe: every age is an unsigned difference and nothing compares
 * absolute tick stamps.
 */

#ifndef PACK_FSM_H_
#define PACK_FSM_H_

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack.h"
#include "App/Pack/pack_type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported types -----------------------------------------------------------*/

/* ==========================================================================
 * Confidence decay (§10.6)
 *
 * "Both are capped by the core at what the electrical group's age permits, so
 * a pack going quiet loses confidence BEFORE it goes stale, and a type can
 * never claim more than its freshness earns."
 *
 * The concrete contract, which the design states as a requirement and not as
 * a curve:
 *
 *     age <= staleAfter/PACK_CONF_FULL_DIV   -> cap = 1000 per-mille
 *     staleAfter/DIV < age < staleAfter      -> LINEAR ramp down to 0
 *     age >= staleAfter, or PACK_AGE_NEVER   -> cap = 0
 *
 * The cap is applied as a ceiling, never as a floor: the core only ever
 * LOWERS a type's honest opinion.
 * ========================================================================== */

#define PACK_CONF_FULL_PM     1000u   /* per-mille, full confidence          */
#define PACK_CONF_FULL_DIV       4u   /* fresh until staleAfter/4 has passed */

/** Per-instance condition and staleness state.  Plain data; the core embeds
 *  one of these per instance and never reaches into it from outside this
 *  file's functions. */
typedef struct {
    uint32_t groupTick_ms[packGrp_last]; /* absolute stamp of last delivery  */
    uint32_t groupSeen;                  /* PACK_GRP_BIT set EVER delivered  */
    /* THE ONLY BUDGET.  It answers one question -- "is this pack talking?"
     * -- and it is measured against the liveness group, because any answer
     * from the pack proves the pack is there.  There is deliberately no
     * second, per-attribute budget: see sPackState.age_ms. */
    uint32_t staleAfter_ms;
    uint32_t caps;                       /* ePackCap, confirmed at bind      */
    uint8_t  cond;                       /* ePackCondition                   */
    uint8_t  why;                        /* ePackAbsentReason, computed      */
    uint8_t  bindWhy;                    /* ePackAbsentReason the BIND left:
                                            noType / noBinding / notPolled,
                                            or packWhy_none when the binding
                                            itself is sound                  */
} sPackFsm;

/** The inputs Pack_Command validates against.  A struct rather than eight
 *  arguments so a test can build one refusal case per field, and so adding a
 *  ninth rule cannot silently skip a caller. */
typedef struct {
    uint32_t             cmds;          /* PACK_CMD_BIT set THIS INSTANCE
                                           advertises; 0 = read-only pack    */
    const sPackCmdBound *bounds;        /* the type's storage, one per bit   */
    uint8_t              boundCount;
    uint8_t              cond;          /* ePackCondition                    */
    uint8_t              inFlight;      /* 1 = a command is already claimed  */
    uint8_t              provisioned;   /* 0 = no configuration at all       */
} sPackCmdCtx;

/** One in-flight command slot.
 *
 * `seq` is the generation counter that makes a LATE completion recognisable:
 * it increments on every claim AND on every finish, so a completion carrying
 * a stale seq is discarded and counted rather than written into a decision
 * the consumer has already been told about (§11.1). */
typedef struct {
    uint32_t deadline_ms;   /* absolute; issued_ms + timeout_ms              */
    uint32_t issued_ms;
    uint32_t seq;           /* generation; odd == in flight is NOT assumed,
                               the state field is authoritative              */
    int32_t  value;
    uint8_t  cmd;           /* ePackCmdId                                    */
    uint8_t  inFlight;      /* 1 = claimed and not yet finished              */
} sPackCmdSlot;

/* Exported functions -------------------------------------------------------*/

/* --- condition and staleness ------------------------------------------- */

/**
 * @brief  Put one instance's condition state into its start-of-life shape.
 *
 * WHEN IMPLEMENTED: every group tick is cleared and groupSeen is 0, so every
 * age reads PACK_AGE_NEVER; cond is packCond_absent; bindWhy is whatever the
 * bind left behind and why is derived from it.
 *
 * @param  fsm - the instance's state
 * @param  staleAfter_ms - how long without ANY answer means stale, > 0
 * @param  caps - the capability set bind() confirmed
 * @note   Pure.  Any context.  Never blocks.
 */
void PackFsm_Init(sPackFsm *fsm, uint32_t staleAfter_ms, uint32_t caps);

/**
 * @brief  Record why the binding, rather than the battery, is the problem.
 * @param  fsm - the instance's state
 * @param  why - packWhy_noType / noBinding / notPolled, or packWhy_none when
 *               the binding is sound and only the pack's silence is at issue
 * @note   Pure.  Shared func task in practice (it follows a bind).
 */
void PackFsm_SetBindReason(sPackFsm *fsm, ePackAbsentReason why);

/**
 * @brief  Record that @p groups were refreshed at @p now_ms.
 *
 * DOES NOT CHANGE THE CONDITION — see the file header.  Groups NOT named keep
 * their previous tick, which is what makes a partial delivery visible instead
 * of silent (§11.1).
 *
 * @param  fsm - the instance's state
 * @param  groups - PACK_GRP_BIT set actually refreshed; 0 is a no-op
 * @param  now_ms - the caller's monotonic clock
 * @retval the PACK_GRP_BIT set that was newly seen for the FIRST time (so the
 *         core can tell "first ever delivery" from "refresh")
 * @note   Pure.  Any context, including an ISR (the core calls it inside its
 *         critical section).  Never blocks.
 */
uint32_t PackFsm_NotePublish(sPackFsm *fsm, uint32_t groups, uint32_t now_ms);

/**
 * @brief  Record a transport fact that carries no measurement.
 *
 * @p answered non-zero refreshes the LIVENESS group's tick (packGrp_electrical)
 * without touching any value — the pack proved it is there.  @p answered zero
 * records nothing at all: a failed exchange is not evidence of a time, and
 * the wall clock already covers it.
 *
 * @param  fsm - the instance's state
 * @param  answered - non-zero if the pack answered
 * @param  now_ms - the caller's monotonic clock
 * @note   Pure.  Any context.  Never blocks.
 */
void PackFsm_NoteLiveness(sPackFsm *fsm, int answered, uint32_t now_ms);

/**
 * @brief  THE TICK.  Decide the condition from the recorded ticks and the
 *         wall clock.  The ONLY function that moves `cond`.
 *
 * WHEN IMPLEMENTED:
 *   - a pack whose electrical group has NEVER been delivered is
 *     packCond_absent, whatever else arrived;
 *   - a pack whose electrical age is <= staleAfter_ms is packCond_online;
 *   - a pack that HAS been delivered and whose electrical age EXCEEDS
 *     staleAfter_ms is packCond_stale.  Strictly exceeds: age == budget is
 *     still online, so the budget is inclusive and a 5 s lap against a 15 s
 *     budget has no boundary surprise;
 *   - `why` becomes packWhy_none when online, otherwise bindWhy when bindWhy
 *     is not packWhy_none, otherwise packWhy_noReply;
 *   - a pack NEVER returns to packCond_absent once it has answered — absent
 *     means "has never answered", and rewriting history would lose the
 *     distinction the reason codes exist to keep.
 *
 * @param  fsm - the instance's state
 * @param  now_ms - the caller's monotonic clock
 * @param  from - out, optional; the condition before the call
 * @param  to - out, optional; the condition after
 * @retval 1 if the condition changed (the core raises packEvt_condition), 0
 *         if it did not
 * @note   Pure.  Shared func task in practice; safe anywhere.  Never blocks.
 */
int PackFsm_Evaluate(sPackFsm *fsm, uint32_t now_ms,
                     ePackCondition *from, ePackCondition *to);

/**
 * @brief  One group's age.
 * @param  fsm - the instance's state
 * @param  grp - the group
 * @param  now_ms - the caller's monotonic clock
 * @retval milliseconds since that group was last delivered, or
 *         PACK_AGE_NEVER when it never has been.  PACK_AGE_NEVER IS DISTINCT
 *         FROM 0: 0 means "delivered, this instant".
 * @note   Pure.  Any context.  Never blocks.
 */
uint32_t PackFsm_AgeMs(const sPackFsm *fsm, ePackGroup grp, uint32_t now_ms);


/**
 * @brief  The ceiling the electrical group's freshness puts on any confidence.
 *
 * See the PACK_CONF_FULL_* block above for the exact curve.  The core applies
 * it to socConf_pm and sohConf_pm as a MINIMUM operation, never as an
 * assignment: a type that reports low confidence keeps it.
 *
 * @param  fsm - the instance's state
 * @param  now_ms - the caller's monotonic clock
 * @retval 0..1000 per-mille
 * @note   Pure.  Any context.  Never blocks.
 */
uint16_t PackFsm_ConfidenceCap_pm(const sPackFsm *fsm, uint32_t now_ms);

/* --- balance transfer accounting (§23.3) -------------------------------- */

/**
 * @brief  Fold one observation interval into the per-cell accumulation.
 *
 * PURE ARITHMETIC, so it is host-tested rather than only exercised on a
 * board.  The caller owns the lock and the clock; this owns the rules:
 *   - nothing is integrated unless the balancer was ACTIVE over the interval;
 *   - an interval longer than @p maxGap_ms is DROPPED, not scaled -- a gap
 *     that long means we stopped observing (rebind, config swap, stalled
 *     transport) and charge that flowed unobserved is not ours to attribute;
 *   - charge is credited to @p sinkIdx and debited from @p srcIdx, each only
 *     when it names a real cell;
 *   - accumulation is in MILLIAMP-SECONDS so the sample path never divides.
 *
 * @param  bal - the accumulation, mutated in place
 * @param  active - non-zero if the balancer was transferring
 * @param  current_mA - transfer magnitude (sign ignored)
 * @param  dt_ms - length of the interval
 * @param  maxGap_ms - longest interval still considered observed
 * @param  srcIdx - cell drained, or PACK_CELL_NONE
 * @param  sinkIdx - cell charged, or PACK_CELL_NONE
 * @retval 1 if charge was attributed, 0 if the interval was skipped
 * @note   Pure.  Any context.
 */
int PackFsm_BalanceAccumulate(sPackBalanceStats *bal, int active,
                              int32_t current_mA, uint32_t dt_ms,
                              uint32_t maxGap_ms,
                              uint8_t srcIdx, uint8_t sinkIdx);

/**
 * @brief  Derive per-cell capacity deviation from the accumulation.
 *
 * net[c] = in[c] - out[c].  The reference is the pack's own MEDIAN net
 * transfer, so a balancer that favours one end uniformly does not read as
 * every cell being bad.  The result is NEGATED: a cell the balancer keeps
 * having to CHARGE holds less than the pack.
 *
 * RELATIVE, NOT ABSOLUTE.  This says "cell 6 is 400 mAh below the pack
 * median", never "cell 6 is 258 Ah" -- an absolute per-cell capacity needs
 * the two-knee measurement of §22.
 *
 * @param  bal - the accumulation
 * @param  delta_mAh_out - array of at least bal->cellCount entries
 * @note   Pure.  Any context.
 */
void PackFsm_BalanceDerive(const sPackBalanceStats *bal,
                           int32_t *delta_mAh_out);

/* --- capability gating -------------------------------------------------- */

/**
 * @brief  The capability bits a field group needs before its values mean
 *         anything.
 *
 * packGrp_electrical needs none — voltage and current are what every battery
 * has, and they are the liveness group.  Everything else maps onto §10.2.
 *
 * @param  grp - the group
 * @retval an ePackCap mask; 0 when the group is unconditionally meaningful
 * @note   Pure.  Any context.
 */
uint32_t PackFsm_GroupCapMask(ePackGroup grp);

/**
 * @brief  Does this instance's capability set make @p grp meaningful?
 *
 * "A field group listed against a capability is meaningful only when that
 * capability is set; otherwise it reads zero and MEANS NOTHING" (§10.2).  A
 * consumer asks this rather than inspecting caps itself, so the mapping lives
 * in one place.
 *
 * @param  caps - the instance's confirmed capability set
 * @param  grp - the group
 * @retval 1 when meaningful, 0 when it must be read as meaningless
 * @note   Pure.  Any context.
 */
int PackFsm_GroupIsMeaningful(uint32_t caps, ePackGroup grp);

/* --- command validation ------------------------------------------------- */

/**
 * @brief  Find the bound declared for one command id.
 * @param  bounds - the type's array
 * @param  count - its length
 * @param  cmd - the command id
 * @retval the entry, or NULL when the type declared none
 * @note   Pure.  Any context.
 */
const sPackCmdBound *PackFsm_FindBound(const sPackCmdBound *bounds,
                                       uint8_t count, ePackCmdId cmd);

/**
 * @brief  Apply every synchronous refusal rule of §10.9 to one command.
 *
 * WHEN IMPLEMENTED, in this order, because the earlier answers are the more
 * specific ones and a caller should learn the most specific reason:
 *   1. NULL @p ctx or @p cmd, cmd->cmd outside 0..packCmd_last-1, timeout_ms
 *      == 0, timeout_ms > PACK_CMD_TIMEOUT_MAX_MS  -> packErr_badArg.
 *      NOTE the id range check is what stops a CAPABILITY MASK being passed
 *      where a dense id belongs.
 *   2. !ctx->provisioned                            -> packErr_unprovisioned
 *   3. !(ctx->cmds & PACK_CMD_BIT(cmd))             -> packErr_notSupported,
 *      BEFORE anything reaches a wire
 *   4. ctx->cond != packCond_online                 -> packErr_notOnline
 *   5. ctx->inFlight                                -> packErr_busy
 *   6. no bound declared for an advertised command  -> packErr_notSupported
 *      (advertising without a bound is the §16-item-10 failure, and the safe
 *      reading is "not really supported")
 *   7. value outside [min_scaled, max_scaled]       -> packErr_outOfRange.
 *      THERE IS NO CLAMPING.  Booleans carry {0, 1} bounds like anything
 *      else, so a stray 2 for chargeEnable is refused by the same rule.
 *
 * @param  ctx - the instance's command context
 * @param  cmd - the command; borrowed
 * @param  timeout_ms - the caller's deadline
 * @retval packErr_ok when the command may be claimed, otherwise the refusal
 * @note   Pure.  Any task.  Never blocks.
 */
ePackErr PackFsm_ValidateCommand(const sPackCmdCtx *ctx,
                                 const sPackCommand *cmd, uint32_t timeout_ms);

/**
 * @brief  Claim the in-flight slot for a command that has already passed
 *         PackFsm_ValidateCommand.
 *
 * WHEN IMPLEMENTED: bumps seq, records the value, the id, issued_ms and
 * deadline_ms = now_ms + timeout_ms, and sets inFlight.  Returns the seq the
 * caller must hand back to PackFsm_CmdComplete.
 *
 * @param  slot - the instance's slot
 * @param  cmd - the command
 * @param  timeout_ms - validated deadline
 * @param  now_ms - the caller's monotonic clock
 * @param  seq_out - out, the generation of this claim
 * @retval packErr_ok, packErr_busy when already claimed, packErr_badArg
 * @note   Pure.  Any task; the core calls it inside its critical section.
 */
ePackErr PackFsm_CmdClaim(sPackCmdSlot *slot, const sPackCommand *cmd,
                          uint32_t timeout_ms, uint32_t now_ms,
                          uint32_t *seq_out);

/**
 * @brief  Deliver a type's completion, or recognise it as LATE.
 *
 * "A completion for a command the core has ALREADY FINISHED is DISCARDED AND
 * COUNTED as sPackStats.lateCompletes.  A late answer must never be written
 * into a decision the consumer has already been told about" (§11.1).
 *
 * WHEN IMPLEMENTED: a completion is live only when the slot is still
 * inFlight AND @p seq matches the claim's generation.  A live completion
 * clears the slot, bumps seq and reports the type's result.  Anything else is
 * late and NOTHING in the slot changes.
 *
 * @param  slot - the instance's slot
 * @param  seq - the generation the type was given at claim time
 * @param  result - what the type decided
 * @param  deliver - out, optional; the result the consumer should be told,
 *                   written only when the completion is live
 * @retval 1 when the completion is live and must be delivered, 0 when it is
 *         LATE and must be discarded and counted
 * @note   Pure.  Any context, including an ISR.
 */
int PackFsm_CmdComplete(sPackCmdSlot *slot, uint32_t seq, ePackErr result,
                        ePackErr *deliver);

/**
 * @brief  The tick's half of the command contract: finish a slot the type
 *         will never finish.
 *
 * WHEN IMPLEMENTED, in this order:
 *   - not inFlight                     -> 0, nothing to do
 *   - @p configChanged                 -> packErr_unknownOutcome (§10.9 rule 8;
 *     this module's config change or the Modbus module's, which invalidates a
 *     jkbms instance's devOrd resolution the same way)
 *   - cond != packCond_online          -> packErr_unknownOutcome (rule 5;
 *     NEVER "assume it obeyed")
 *   - now_ms past deadline_ms          -> packErr_timeout
 * A finish clears inFlight and bumps seq, so any later completion from the
 * type is recognised as late by PackFsm_CmdComplete.
 *
 * @param  slot - the instance's slot
 * @param  cond - the instance's condition right now
 * @param  configChanged - non-zero when a configuration replaced the binding
 * @param  now_ms - the caller's monotonic clock
 * @param  result_out - out, the result to complete with
 * @retval 1 when the slot was finished here, 0 when it was left alone
 * @note   Pure.  Shared func task.  Never blocks.
 */
int PackFsm_CmdExpire(sPackCmdSlot *slot, ePackCondition cond,
                      int configChanged, uint32_t now_ms,
                      ePackErr *result_out);

#ifdef __cplusplus
}
#endif

#endif /* PACK_FSM_H_ */

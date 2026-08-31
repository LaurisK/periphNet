/*
 * pack_fsm.c
 *
 * Condition, staleness, confidence and command validation — the pure part of
 * the pack module (docs/design_battery_pack.md §9, §17).
 *
 * LIBC ONLY.  No FreeRTOS, no HAL, no Trice, no flash.  tests/ compiles this
 * file directly; App/Net/wg_conf.c is the precedent.
 *
 * TIME IS ALWAYS AN ARGUMENT.  Every age is an unsigned difference, so the
 * 2^32 ms (~49.7 day) wrap needs no special case: nothing here compares two
 * absolute stamps directly.
 *
 * The contract each function must meet is in pack_fsm.h, function by
 * function; this file deliberately carries no second copy of it.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_fsm.h"

#include <string.h>

/* Private functions --------------------------------------------------------*/

/* A group's recorded age, with "never delivered" as a distinct answer.
 * Shared by AgeMs and ConfidenceCap so the two can never
 * disagree about what "never" means. */
static uint32_t group_age(const sPackFsm *fsm, ePackGroup grp, uint32_t now_ms)
{
    if ((fsm->groupSeen & PACK_GRP_BIT(grp)) == 0u) {
        return PACK_AGE_NEVER;
    }
    return (uint32_t)(now_ms - fsm->groupTick_ms[grp]);
}

/* `why` is derived, never stored independently: a binding fault outranks the
 * pack's silence, because it is the more specific and more actionable answer.
 * Online is unconditionally packWhy_none. */
static uint8_t derive_why(const sPackFsm *fsm, ePackCondition cond)
{
    if (cond == packCond_online) {
        return (uint8_t)packWhy_none;
    }
    if (fsm->bindWhy != (uint8_t)packWhy_none) {
        return fsm->bindWhy;
    }
    return (uint8_t)packWhy_noReply;
}

/* Exported functions -------------------------------------------------------*/

void PackFsm_Init(sPackFsm *fsm, uint32_t staleAfter_ms, uint32_t caps)
{
    if (fsm == NULL) {
        return;
    }

    memset(fsm, 0, sizeof(*fsm));

    fsm->staleAfter_ms     = staleAfter_ms;
    fsm->caps              = caps;
    fsm->cond              = (uint8_t)packCond_absent;
    fsm->bindWhy           = (uint8_t)packWhy_none;
    fsm->why               = derive_why(fsm, packCond_absent);
}

void PackFsm_SetBindReason(sPackFsm *fsm, ePackAbsentReason why)
{
    if (fsm == NULL) {
        return;
    }

    fsm->bindWhy = (uint8_t)why;
    fsm->why     = derive_why(fsm, (ePackCondition)fsm->cond);
}

uint32_t PackFsm_NotePublish(sPackFsm *fsm, uint32_t groups, uint32_t now_ms)
{
    uint32_t firstEver = 0u;
    uint32_t g;

    if ((fsm == NULL) || (groups == 0u)) {
        return 0u;
    }

    for (g = 0u; g < (uint32_t)packGrp_last; g++) {
        const uint32_t bit = PACK_GRP_BIT(g);

        if ((groups & bit) == 0u) {
            /* A group this commit did not name keeps its previous tick.  That
             * is what makes a partial delivery visible instead of silent. */
            continue;
        }
        if ((fsm->groupSeen & bit) == 0u) {
            firstEver |= bit;
        }
        fsm->groupSeen        |= bit;
        fsm->groupTick_ms[g]   = now_ms;
    }

    /* Deliberately does NOT touch cond: silence is not an event, so only the
     * tick may decide (see the file header of pack_fsm.h). */
    return firstEver;
}

void PackFsm_NoteLiveness(sPackFsm *fsm, int answered, uint32_t now_ms)
{
    if ((fsm == NULL) || (answered == 0)) {
        /* A failed exchange is not evidence of a time.  The wall clock in
         * PackFsm_Evaluate already covers it, so nothing is recorded. */
        return;
    }

    fsm->groupSeen                        |= PACK_GRP_BIT(packGrp_electrical);
    fsm->groupTick_ms[packGrp_electrical]  = now_ms;
}

int PackFsm_Evaluate(sPackFsm *fsm, uint32_t now_ms,
                     ePackCondition *from, ePackCondition *to)
{
    ePackCondition before;
    ePackCondition after;
    uint32_t       age;

    if (fsm == NULL) {
        return 0;
    }

    before = (ePackCondition)fsm->cond;
    age    = group_age(fsm, packGrp_electrical, now_ms);

    if (age == PACK_AGE_NEVER) {
        /* Never answered.  Nothing else that arrived can make it present:
         * the electrical group IS the liveness group. */
        after = packCond_absent;
    } else if (age <= fsm->staleAfter_ms) {
        /* Inclusive, so a 5 s lap against a 15 s budget has no boundary
         * surprise. */
        after = packCond_online;
    } else {
        after = packCond_stale;
    }

    /* A pack never returns to absent once it has answered — absent means "has
     * never answered", and rewriting that would lose the distinction the
     * reason codes exist to keep. */
    if ((after == packCond_absent) && (before != packCond_absent)) {
        after = packCond_stale;
    }

    fsm->cond = (uint8_t)after;
    fsm->why  = derive_why(fsm, after);

    if (from != NULL) {
        *from = before;
    }
    if (to != NULL) {
        *to = after;
    }
    return (before != after) ? 1 : 0;
}

uint32_t PackFsm_AgeMs(const sPackFsm *fsm, ePackGroup grp, uint32_t now_ms)
{
    if ((fsm == NULL) || ((uint32_t)grp >= (uint32_t)packGrp_last)) {
        return PACK_AGE_NEVER;
    }
    return group_age(fsm, grp, now_ms);
}


uint16_t PackFsm_ConfidenceCap_pm(const sPackFsm *fsm, uint32_t now_ms)
{
    uint32_t age;
    uint32_t knee;
    uint64_t num;

    if (fsm == NULL) {
        return 0u;
    }

    age = group_age(fsm, packGrp_electrical, now_ms);
    if ((age == PACK_AGE_NEVER) || (age >= fsm->staleAfter_ms)) {
        return 0u;
    }

    knee = fsm->staleAfter_ms / PACK_CONF_FULL_DIV;
    if (age <= knee) {
        return (uint16_t)PACK_CONF_FULL_PM;
    }

    /* Linear from full at the knee to zero at the budget.  64-bit only to keep
     * the multiply safe for any budget a config might carry. */
    num = (uint64_t)PACK_CONF_FULL_PM * (uint64_t)(fsm->staleAfter_ms - age);
    return (uint16_t)(num / (uint64_t)(fsm->staleAfter_ms - knee));
}

int PackFsm_BalanceAccumulate(sPackBalanceStats *bal, int active,
                              int32_t current_mA, uint32_t dt_ms,
                              uint32_t maxGap_ms,
                              uint8_t srcIdx, uint8_t sinkIdx)
{
    int32_t q_mAs;

    if ((bal == NULL) || (active == 0) || (dt_ms == 0u) ||
        (dt_ms > maxGap_ms) || (bal->cellCount == 0u)) {
        return 0;
    }
    if (current_mA < 0) {
        current_mA = -current_mA;       /* magnitude; src/sink carry direction */
    }

    /* mA x ms / 1000 = mAs.  Done in 64-bit because the product overflows
     * int32 for a long interval at a high balance current, and the whole
     * point of accumulating in mAs is that the sample path never divides by
     * 3600. */
    q_mAs = (int32_t)(((int64_t)current_mA * (int64_t)dt_ms) / 1000);
    if (q_mAs == 0) {
        return 0;
    }

    if (sinkIdx < bal->cellCount) {
        bal->in_mAs[sinkIdx] += q_mAs;
    }
    if (srcIdx < bal->cellCount) {
        bal->out_mAs[srcIdx] += q_mAs;
    }
    bal->activeSamples++;
    return 1;
}

void PackFsm_BalanceDerive(const sPackBalanceStats *bal, int32_t *delta_mAh_out)
{
    int32_t net[PACK_CELLS_MAX];
    int32_t tmp[PACK_CELLS_MAX];
    int32_t median;
    uint8_t i;
    uint8_t j;

    if ((bal == NULL) || (delta_mAh_out == NULL) || (bal->cellCount == 0u)) {
        return;
    }

    for (i = 0u; i < bal->cellCount; i++) {
        net[i] = bal->in_mAs[i] - bal->out_mAs[i];
        tmp[i] = net[i];
    }

    /* Insertion sort: cellCount is at most PACK_CELLS_MAX, so this is
     * cheaper than being clever and has no worst case worth worrying about. */
    for (i = 1u; i < bal->cellCount; i++) {
        const int32_t v = tmp[i];

        for (j = i; (j > 0u) && (tmp[j - 1u] > v); j--) {
            tmp[j] = tmp[j - 1u];
        }
        tmp[j] = v;
    }
    median = tmp[bal->cellCount / 2u];

    for (i = 0u; i < bal->cellCount; i++) {
        /* mAs -> mAh, and NEGATED: transfer IN means capacity BELOW. */
        delta_mAh_out[i] = -((net[i] - median) / 3600);
    }
}

uint32_t PackFsm_GroupCapMask(ePackGroup grp)
{
    switch (grp) {
    case packGrp_charge:
        return (uint32_t)packCap_capacityAh;
    case packGrp_temperature:
        return (uint32_t)packCap_temperatures;
    case packGrp_limits:
        return (uint32_t)packCap_currentLimits;
    case packGrp_switches:
        return (uint32_t)packCap_switchState;
    case packGrp_cells:
        return (uint32_t)packCap_cellSummary;

    case packGrp_electrical:
        /* Voltage and current are what every battery has, and this is the
         * liveness group — so it is unconditional. */
    case packGrp_alarms:
    case packGrp_vendorInfo:
    default:
        return 0u;
    }
}

int PackFsm_GroupIsMeaningful(uint32_t caps, ePackGroup grp)
{
    const uint32_t mask = PackFsm_GroupCapMask(grp);

    if (mask == 0u) {
        return 1;
    }
    return ((caps & mask) == mask) ? 1 : 0;
}

const sPackCmdBound *PackFsm_FindBound(const sPackCmdBound *bounds,
                                       uint8_t count, ePackCmdId cmd)
{
    uint8_t i;

    if (bounds == NULL) {
        return NULL;
    }
    for (i = 0u; i < count; i++) {
        if (bounds[i].cmd == cmd) {
            return &bounds[i];
        }
    }
    return NULL;
}

ePackErr PackFsm_ValidateCommand(const sPackCmdCtx *ctx,
                                 const sPackCommand *cmd, uint32_t timeout_ms)
{
    const sPackCmdBound *bound;

    /* 1. Arguments.  The id-range check is what stops a CAPABILITY MASK being
     *    passed where a dense id belongs: ids start at 1 so the smallest
     *    two-bit mask exceeds the largest id (pack.h's _Static_assert). */
    if ((ctx == NULL) || (cmd == NULL)) {
        return packErr_badArg;
    }
    if ((cmd->cmd == packCmd_undefined) ||
        ((uint32_t)cmd->cmd >= (uint32_t)packCmd_last)) {
        return packErr_badArg;
    }
    if ((timeout_ms == 0u) || (timeout_ms > PACK_CMD_TIMEOUT_MAX_MS)) {
        /* An unbounded deadline is the same as no guarantee. */
        return packErr_badArg;
    }

    /* 2. Nothing is configured, so nothing can be commanded. */
    if (ctx->provisioned == 0u) {
        return packErr_unprovisioned;
    }

    /* 3. Not advertised — refused BEFORE anything reaches a wire. */
    if ((ctx->cmds & PACK_CMD_BIT(cmd->cmd)) == 0u) {
        return packErr_notSupported;
    }

    /* 4. Commanding an absent or stale pack is a programming error, not a
     *    retry: the module will not hold intent across a stale period. */
    if (ctx->cond != (uint8_t)packCond_online) {
        return packErr_notOnline;
    }

    /* 5. Queueing would make "what state is this pack in" unanswerable. */
    if (ctx->inFlight != 0u) {
        return packErr_busy;
    }

    /* 6. Advertised without a bound is the §16-item-10 failure; the safe
     *    reading is "not really supported". */
    bound = PackFsm_FindBound(ctx->bounds, ctx->boundCount, cmd->cmd);
    if (bound == NULL) {
        return packErr_notSupported;
    }

    /* 7. Bounds are CHECKED, never clamped.  Booleans carry {0, 1} like
     *    anything else, so a stray 2 for chargeEnable is refused here. */
    if ((cmd->value < bound->min_scaled) || (cmd->value > bound->max_scaled)) {
        return packErr_outOfRange;
    }

    return packErr_ok;
}

ePackErr PackFsm_CmdClaim(sPackCmdSlot *slot, const sPackCommand *cmd,
                          uint32_t timeout_ms, uint32_t now_ms,
                          uint32_t *seq_out)
{
    if ((slot == NULL) || (cmd == NULL)) {
        return packErr_badArg;
    }
    if (slot->inFlight != 0u) {
        return packErr_busy;
    }

    slot->seq++;
    slot->cmd         = (uint8_t)cmd->cmd;
    slot->value       = cmd->value;
    slot->issued_ms   = now_ms;
    slot->deadline_ms = now_ms + timeout_ms;
    slot->inFlight    = 1u;

    if (seq_out != NULL) {
        *seq_out = slot->seq;
    }
    return packErr_ok;
}

int PackFsm_CmdComplete(sPackCmdSlot *slot, uint32_t seq, ePackErr result,
                        ePackErr *deliver)
{
    if (slot == NULL) {
        return 0;
    }
    if ((slot->inFlight == 0u) || (slot->seq != seq)) {
        /* LATE.  The core has already finished this command and told the
         * consumer; a late answer must never be written into that decision.
         * Nothing in the slot changes — the caller counts it. */
        return 0;
    }

    slot->inFlight = 0u;
    slot->seq++;

    if (deliver != NULL) {
        *deliver = result;
    }
    return 1;
}

int PackFsm_CmdExpire(sPackCmdSlot *slot, ePackCondition cond,
                      int configChanged, uint32_t now_ms,
                      ePackErr *result_out)
{
    ePackErr result;

    if ((slot == NULL) || (slot->inFlight == 0u)) {
        return 0;
    }

    if (configChanged != 0) {
        /* The binding this command was issued against no longer exists. */
        result = packErr_unknownOutcome;
    } else if (cond != packCond_online) {
        /* NEVER "assume it obeyed" — the write may or may not have landed. */
        result = packErr_unknownOutcome;
    } else if ((int32_t)(now_ms - slot->deadline_ms) > 0) {
        /* Inclusive, like the staleness budget: at exactly the deadline
         * the command still has its last instant. */
        result = packErr_timeout;
    } else {
        return 0;
    }

    slot->inFlight = 0u;
    slot->seq++;

    if (result_out != NULL) {
        *result_out = result;
    }
    return 1;
}

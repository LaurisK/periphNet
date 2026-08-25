/*
 * pack_fsm.c
 *
 * Condition, staleness, confidence and command validation — the pure part of
 * the pack module (docs/design_battery_pack.md §9, §17).
 *
 * LIBC ONLY.  No FreeRTOS, no HAL, no Trice, no flash.  tests/ compiles this
 * file directly; App/Net/wg_conf.c is the precedent.
 *
 * STATUS: SCAFFOLDING.  Every body below is a stub that returns the documented
 * "nothing valid" answer — never a success it did not earn.  The contract each
 * one must meet is in pack_fsm.h, function by function; this file deliberately
 * carries no second copy of it.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_fsm.h"

#include <string.h>

/* Private functions --------------------------------------------------------*/

/* Silence an unused-parameter warning without pretending the parameter was
 * used.  A stub that consumed its arguments would be a stub that lied. */
#define PACK_FSM_UNUSED(x)   ((void)(x))

/* Exported functions -------------------------------------------------------*/

void PackFsm_Init(sPackFsm *fsm, uint32_t staleAfter_ms,
                  uint32_t cellStaleAfter_ms, uint32_t caps)
{
    PACK_FSM_UNUSED(staleAfter_ms);
    PACK_FSM_UNUSED(cellStaleAfter_ms);
    PACK_FSM_UNUSED(caps);

    /* STUB.  Zeroing is not the contract: groupSeen must end up 0 AND every
     * age must read PACK_AGE_NEVER, which zeroed ticks alone do not give. */
    if (NULL != fsm) {
        memset(fsm, 0, sizeof(*fsm));
    }
}

void PackFsm_SetBindReason(sPackFsm *fsm, ePackAbsentReason why)
{
    PACK_FSM_UNUSED(fsm);
    PACK_FSM_UNUSED(why);
    /* STUB — records nothing, so `why` never becomes anything but none. */
}

uint32_t PackFsm_NotePublish(sPackFsm *fsm, uint32_t groups, uint32_t now_ms)
{
    PACK_FSM_UNUSED(fsm);
    PACK_FSM_UNUSED(groups);
    PACK_FSM_UNUSED(now_ms);
    return 0u;      /* STUB — nothing was recorded, so nothing is newly seen */
}

void PackFsm_NoteLiveness(sPackFsm *fsm, int answered, uint32_t now_ms)
{
    PACK_FSM_UNUSED(fsm);
    PACK_FSM_UNUSED(answered);
    PACK_FSM_UNUSED(now_ms);
    /* STUB — records nothing. */
}

int PackFsm_Evaluate(sPackFsm *fsm, uint32_t now_ms,
                     ePackCondition *from, ePackCondition *to)
{
    PACK_FSM_UNUSED(fsm);
    PACK_FSM_UNUSED(now_ms);

    if (NULL != from) {
        *from = packCond_absent;
    }
    if (NULL != to) {
        *to = packCond_absent;
    }
    return 0;       /* STUB — the condition never moves */
}

uint32_t PackFsm_AgeMs(const sPackFsm *fsm, ePackGroup grp, uint32_t now_ms)
{
    PACK_FSM_UNUSED(fsm);
    PACK_FSM_UNUSED(grp);
    PACK_FSM_UNUSED(now_ms);
    return PACK_AGE_NEVER;  /* STUB — "never delivered" is the honest answer
                               for a module that has recorded nothing */
}

uint32_t PackFsm_GroupsStale(const sPackFsm *fsm, uint32_t now_ms)
{
    PACK_FSM_UNUSED(fsm);
    PACK_FSM_UNUSED(now_ms);
    return 0u;      /* STUB.  NOTE this is the WRONG answer on purpose: with
                       nothing delivered every group is stale, and returning 0
                       is what the tests catch */
}

uint16_t PackFsm_ConfidenceCap_pm(const sPackFsm *fsm, uint32_t now_ms)
{
    PACK_FSM_UNUSED(fsm);
    PACK_FSM_UNUSED(now_ms);
    return 0u;      /* STUB — believe nothing, which is the safe failure */
}

uint32_t PackFsm_GroupCapMask(ePackGroup grp)
{
    PACK_FSM_UNUSED(grp);
    return 0u;      /* STUB — claims every group is unconditional */
}

int PackFsm_GroupIsMeaningful(uint32_t caps, ePackGroup grp)
{
    PACK_FSM_UNUSED(caps);
    PACK_FSM_UNUSED(grp);
    return 0;       /* STUB — nothing is meaningful yet */
}

const sPackCmdBound *PackFsm_FindBound(const sPackCmdBound *bounds,
                                       uint8_t count, ePackCmdId cmd)
{
    PACK_FSM_UNUSED(bounds);
    PACK_FSM_UNUSED(count);
    PACK_FSM_UNUSED(cmd);
    return NULL;    /* STUB — no bound is ever found */
}

ePackErr PackFsm_ValidateCommand(const sPackCmdCtx *ctx,
                                 const sPackCommand *cmd, uint32_t timeout_ms)
{
    PACK_FSM_UNUSED(ctx);
    PACK_FSM_UNUSED(cmd);
    PACK_FSM_UNUSED(timeout_ms);
    return packErr_notSupported;    /* STUB — refuse everything.  A validator
                                       that defaulted to ok would put an
                                       unvalidated write on a live battery */
}

ePackErr PackFsm_CmdClaim(sPackCmdSlot *slot, const sPackCommand *cmd,
                          uint32_t timeout_ms, uint32_t now_ms,
                          uint32_t *seq_out)
{
    PACK_FSM_UNUSED(slot);
    PACK_FSM_UNUSED(cmd);
    PACK_FSM_UNUSED(timeout_ms);
    PACK_FSM_UNUSED(now_ms);

    if (NULL != seq_out) {
        *seq_out = 0u;
    }
    return packErr_notSupported;    /* STUB — nothing is ever claimed */
}

int PackFsm_CmdComplete(sPackCmdSlot *slot, uint32_t seq, ePackErr result,
                        ePackErr *deliver)
{
    PACK_FSM_UNUSED(slot);
    PACK_FSM_UNUSED(seq);
    PACK_FSM_UNUSED(result);
    PACK_FSM_UNUSED(deliver);
    return 0;       /* STUB — every completion reads as LATE, which is the
                       discard-and-count side and therefore the safe one */
}

int PackFsm_CmdExpire(sPackCmdSlot *slot, ePackCondition cond,
                      int configChanged, uint32_t now_ms,
                      ePackErr *result_out)
{
    PACK_FSM_UNUSED(slot);
    PACK_FSM_UNUSED(cond);
    PACK_FSM_UNUSED(configChanged);
    PACK_FSM_UNUSED(now_ms);
    PACK_FSM_UNUSED(result_out);
    return 0;       /* STUB — nothing is ever expired, so `done` would never
                       fire.  §10.9 rule 6 is exactly what the tests assert */
}

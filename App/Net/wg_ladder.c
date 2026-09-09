/**
 * @file    wg_ladder.c
 * @brief   The recovery ladder's pure core.  See wg_ladder.h for the rule.
 */
#include "App/Net/wg_ladder.h"

#include <stddef.h>

void WgLadder_Init(sWgLadder *st)
{
    if (st == NULL) {
        return;
    }
    st->downSince_ms  = 0u;
    st->lastAction_ms = 0u;
    st->haveClock     = 0u;
    st->reloadDone    = 0u;
    st->rebootsUsed   = 0u;
    st->rung          = 0u;
}

void WgLadder_RestoreReboots(sWgLadder *st, uint8_t used)
{
    if (st == NULL) {
        return;
    }
    st->rebootsUsed = (used > WG_LADDER_MAX_REBOOTS)
                    ? (uint8_t)WG_LADDER_MAX_REBOOTS : used;
}

uint32_t WgLadder_OutageAge(const sWgLadder *st, uint32_t now_ms)
{
    if (st == NULL || !st->haveClock) {
        return 0u;
    }
    return (uint32_t)(now_ms - st->downSince_ms);
}

/* Park the clock: the ladder is not trying, so no time accumulates and no rung
 * can come due.  Used both for "nobody asked for a tunnel" and for a healthy
 * link, which is why it also clears the once-per-outage rung-2 latch. */
static void park(sWgLadder *st, uint32_t now_ms)
{
    st->downSince_ms  = now_ms;
    st->lastAction_ms = now_ms;
    st->haveClock     = 1u;
    st->reloadDone    = 0u;
    st->rung          = 0u;
}

eWgLadderAction WgLadder_Step(sWgLadder *st, const sWgLadderIn *in)
{
    uint32_t age;

    if (st == NULL || in == NULL) {
        return wgLadder_none;
    }

    /* A tunnel that was deliberately stopped must never be restarted by the
     * ladder, and an unprovisioned board must stay quiet rather than hammer a
     * hub it has no key for — the same reason WgLink_Start() refuses to fall
     * back to a shared identity. */
    if (!in->wantRunning || !in->hasIdentity) {
        park(st, in->now_ms);
        return wgLadder_none;
    }

    if (!st->haveClock) {
        park(st, in->now_ms);
    }

    /* THE reset, and the only one.  Note what is NOT here: no rung resets this
     * clock, and `running` alone is not evidence — a rebuilt peer is "running"
     * within milliseconds and has proved nothing.  Only a fresh handshake or a
     * packet in counts, which is exactly what aliveAge_ms measures. */
    if (in->running &&
        in->aliveAge_ms != WG_LADDER_AGE_NEVER &&
        in->aliveAge_ms <= in->staleAfter_ms) {
        park(st, in->now_ms);
        return wgLadder_none;
    }

    age = (uint32_t)(in->now_ms - st->downSince_ms);

    /* Rung 3 — terminal.  Gated on the board's own network being sane: with
     * the link down or no default route the fault is demonstrably off-board, a
     * reboot cannot fix it, and all it would do is interrupt local control.
     * Skipping the rung leaves rung 1 rotating, which is the right behaviour
     * for an outage the board did not cause. */
    if (age >= WG_LADDER_TERMINAL_MS &&
        st->rebootsUsed < (uint8_t)WG_LADDER_MAX_REBOOTS &&
        in->localNetOk) {
        st->rebootsUsed++;
        /* Reset before the caller acts: the reboot is armed on a deadline and
         * the ladder will run again before it lands, so without this the rung
         * would re-fire and burn the whole budget in one pass. */
        st->downSince_ms  = in->now_ms;
        st->lastAction_ms = in->now_ms;
        st->reloadDone    = 0u;
        st->rung          = 3u;
        return wgLadder_terminalReboot;
    }

    /* Rung 2 — once per outage. */
    if (age >= WG_LADDER_RELOAD_MS && !st->reloadDone) {
        st->reloadDone    = 1u;
        st->lastAction_ms = in->now_ms;
        st->rung          = 2u;
        return wgLadder_reloadConfig;
    }

    /* Rung 1 — and the §5.1 retry, which shares the cadence because both are
     * "the current attempt has demonstrably failed, try another".  The timer
     * is lastAction_ms, deliberately NOT the outage clock. */
    if ((uint32_t)(in->now_ms - st->lastAction_ms) >= WG_LADDER_REBUILD_MS) {
        st->lastAction_ms = in->now_ms;
        st->rung          = 1u;
        return in->running ? wgLadder_rebuildRotate : wgLadder_start;
    }

    return wgLadder_none;
}

/* Fallback AFTER the switch, never a default: a default: would satisfy
 * -Wswitch and defeat the mechanism that makes a new enumerator a build
 * failure rather than a "?" someone finds in the field. */
const char *WgLadder_ActionName(eWgLadderAction action)
{
    switch (action) {
    case wgLadder_none:           return "none";
    case wgLadder_start:          return "start";
    case wgLadder_rebuildRotate:  return "rebuild+rotate";
    case wgLadder_reloadConfig:   return "reload-config";
    case wgLadder_terminalReboot: return "terminal-reboot";
    case wgLadder_last:           break;
    }
    return "?";
}

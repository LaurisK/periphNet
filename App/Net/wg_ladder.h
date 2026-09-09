/**
 * @file    wg_ladder.h
 * @brief   THE RECOVERY LADDER for the WireGuard link — the pure decision core.
 *
 * Given how long the hub has been silent, this says what to do about it and
 * nothing else: it owns no peripheral, calls no lwIP function, keeps no file
 * statics and touches no clock of its own.  The caller supplies `now_ms` and
 * the observations; the caller performs whatever comes back.  That is what
 * makes the whole escalation testable on a host, which matters because every
 * rung above the first is by design almost never exercised in the field.
 *
 * Design: docs/design_tunnel_watchdog.md.  Incident that produced it:
 * docs/issue_wg_sodas_offline_2026-09-06.md.
 *
 * ## The one rule
 *
 * **Evidence resets the clock.  Actions never do.**
 *
 * A corrective action that resets the escalation timer guarantees the
 * escalation can never fire — rebuild every 15 minutes and a 6-hour rung stays
 * permanently 15 minutes away.  This is not hypothetical: it is the shape of
 * the §5.1 defect (`s_downSince_ms = now` on the path that should have been
 * accumulating), and it is why the FWU confirm window refuses to treat traffic
 * as a kick.
 *
 * It has a second, sharper edge here.  `WgLink_Stop()` clears the liveness
 * latch, so `WgLink_AliveAge()` reads NEVER immediately after **every** rung-1
 * rebuild.  A ladder that trusted that field as its clock would restart its own
 * escalation on every action it took.  Hence `sWgLadder` carries its own
 * outage clock, advanced by the caller's `now_ms` and reset only by *fresh*
 * evidence.
 */
#ifndef APP_NET_WG_LADDER_H
#define APP_NET_WG_LADDER_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Rung timings.  All are wall-clock since the hub was last heard from, NOT
 * since the last action.
 * ------------------------------------------------------------------------ */

/** Rung 1 — rebuild the peer and rotate the source port.  Repeats.  Also the
 *  retry cadence for a tunnel that failed to start at all (§5.1). */
#define WG_LADDER_REBUILD_MS        900000u     /* 15 min */

/** Rung 2 — re-read the stored config from flash and restart.  Once per
 *  outage.  The only rung that can repair a corrupted in-RAM `s_cfg`, which is
 *  the one genuinely non-deterministic way `WgLink_Start()` fails. */
#define WG_LADDER_RELOAD_MS        3600000u     /* 60 min */

/** Rung 3 — terminal reboot.  Six hours, not three: a three-hour WAN outage at
 *  an LTE site is ordinary, and a rung that fires on ordinary events is a
 *  scheduled reboot rather than a last resort.  Six hours still lets rung 1 try
 *  24 distinct source ports first. */
#define WG_LADDER_TERMINAL_MS     21600000u     /* 6 h */

/** Terminal reboots allowed per power-on.  Bounded because the sodas failure
 *  was deterministic across reboots: unbounded, this rung would have reset a
 *  working controller every six hours forever while it ran two live packs.
 *  The counter lives in the CCM survivor block, so it clears on a power cycle
 *  and only on a power cycle. */
#define WG_LADDER_MAX_REBOOTS            3u

/** No evidence has ever been seen.  Mirrors WG_LINK_AGE_NEVER; kept separate
 *  so this header stays free of wg_link.h and remains host-compilable. */
#define WG_LADDER_AGE_NEVER     0xFFFFFFFFu

/** What the caller should do.  Never renumbered — reported over HTTP. */
typedef enum {
    wgLadder_none = 0,          /**< nothing due                             */
    wgLadder_start,             /**< not running though it should be (§5.1)  */
    wgLadder_rebuildRotate,     /**< rung 1: rebuild peer + rotate port      */
    wgLadder_reloadConfig,      /**< rung 2: reload config from flash        */
    wgLadder_terminalReboot,    /**< rung 3: record, then reboot             */
    wgLadder_last
} eWgLadderAction;

/** Everything the decision depends on.  All observations, no handles. */
typedef struct {
    uint32_t now_ms;            /**< monotonic; wrap-safe differences only    */
    uint32_t aliveAge_ms;       /**< WG_LADDER_AGE_NEVER when no evidence     */
    uint32_t staleAfter_ms;     /**< evidence older than this is not evidence */
    uint8_t  wantRunning;       /**< intent: someone asked for the tunnel     */
    uint8_t  running;           /**< state: the tunnel is actually up         */
    uint8_t  hasIdentity;       /**< provisioned; else the board stays quiet  */
    uint8_t  localNetOk;        /**< link up + a default route (gates rung 3) */
} sWgLadderIn;

/** Ladder state.  Caller-owned, so the core keeps no statics. */
typedef struct {
    uint32_t downSince_ms;      /**< the outage clock — evidence resets it    */
    uint32_t lastAction_ms;     /**< rung-1 cadence, separate by design       */
    uint8_t  haveClock;
    uint8_t  reloadDone;        /**< rung 2 is once per outage                */
    uint8_t  rebootsUsed;
    uint8_t  rung;              /**< last rung acted on, 0 when healthy       */
} sWgLadder;

/** @brief Zero the ladder.  Its clock starts at the first WgLadder_Step(). */
void WgLadder_Init(sWgLadder *st);

/**
 * @brief  Advance the ladder and return the single action now due.
 *
 * Call it on a cadence far finer than the rungs (5 s is what the board uses);
 * it is cheap and returns wgLadder_none almost always.  At most one action per
 * call, highest rung first — but rung 1 keeps running underneath rungs 2 and 3,
 * because rotating the source port stays the move most likely to work.
 */
eWgLadderAction WgLadder_Step(sWgLadder *st, const sWgLadderIn *in);

/** @brief  How long the hub has been silent, for status.  0 when healthy. */
uint32_t WgLadder_OutageAge(const sWgLadder *st, uint32_t now_ms);

/**
 * @brief  Restore the terminal-reboot count after a terminal reboot.
 *
 * The budget is per power-on, so it has to survive the very reset it counts.
 * The caller reads it from the CCM survivor block and hands it back here.
 */
void WgLadder_RestoreReboots(sWgLadder *st, uint8_t used);

/** @brief  Name of an action.  Never NULL; "?" for an out-of-range value. */
const char *WgLadder_ActionName(eWgLadderAction action);

#endif /* APP_NET_WG_LADDER_H */

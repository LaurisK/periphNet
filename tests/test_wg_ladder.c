/**
 * Unit tests for App/Net/wg_ladder.c — the WireGuard recovery ladder
 * (docs/design_tunnel_watchdog.md).
 *
 * These exist because every rung above the first is, by design, almost never
 * exercised in the field: the escalation only runs during an outage nobody is
 * watching, and the two outages that motivated it each cost a site visit. A
 * ladder that is wrong is worse than no ladder — it either never fires or
 * reboots a healthy controller — so the contract is asserted here rather than
 * discovered on a board at the far end of a dead tunnel.
 *
 * The load-bearing assertions are the two that encode the design's single
 * rule, "evidence resets the clock, actions never do":
 *   - test_rebuild_does_not_defer_escalation
 *   - test_stopped_tunnel_is_never_restarted
 * The first is the bug this module exists to prevent; the second is the one a
 * naive fix introduces.
 */

#include "test_util.h"

#include "App/Net/wg_ladder.h"

#define STALE_MS    360000u     /* WG_LINK_STALE_MS, without the lwIP header */

/* A healthy, running, provisioned board with a working local network. */
static sWgLadderIn base_in(uint32_t now_ms)
{
    sWgLadderIn in;
    in.now_ms        = now_ms;
    in.aliveAge_ms   = 0u;
    in.staleAfter_ms = STALE_MS;
    in.wantRunning   = 1u;
    in.running       = 1u;
    in.hasIdentity   = 1u;
    in.localNetOk    = 1u;
    return in;
}

/* Run the ladder from t0 to t0+span at 5 s intervals (the board's cadence),
 * counting what came out.  `silent` selects an outage: no evidence at all. */
static void drive(sWgLadder *st, uint32_t *t, uint32_t span_ms, int silent,
                  int running, int localNetOk, unsigned counts[wgLadder_last])
{
    /* Step-counted, not `while (t < t0 + span)`: the end value wraps for the
     * clock-wrap case and the loop would silently never run. */
    uint32_t steps = span_ms / 5000u;

    while (steps-- > 0u) {
        sWgLadderIn in = base_in(*t);
        eWgLadderAction a;

        in.running    = (uint8_t)running;
        in.localNetOk = (uint8_t)localNetOk;
        if (silent) {
            in.aliveAge_ms = WG_LADDER_AGE_NEVER;
        }

        a = WgLadder_Step(st, &in);
        if ((unsigned)a < (unsigned)wgLadder_last) {
            counts[a]++;
        }
        *t += 5000u;
    }
}

/* ============================================================================
 * A healthy tunnel is left alone
 * ========================================================================= */

static void test_healthy_tunnel_does_nothing(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    drive(&st, &t, 24u * 3600u * 1000u, 0, 1, 1, c);   /* a whole day */

    TEST_ASSERT(c[wgLadder_rebuildRotate] == 0u);
    TEST_ASSERT(c[wgLadder_reloadConfig] == 0u);
    TEST_ASSERT(c[wgLadder_terminalReboot] == 0u);
    TEST_ASSERT(c[wgLadder_start] == 0u);
    TEST_ASSERT(WgLadder_OutageAge(&st, t) < 10000u);
    TEST_ASSERT(st.rung == 0u);
}

/* Evidence that is present but STALE is not evidence.  A tunnel whose last
 * handshake is older than the stale window is down, however recent the port
 * thinks its keypair is. */
static void test_stale_evidence_is_not_evidence(void)
{
    sWgLadder   st;
    sWgLadderIn in;

    WgLadder_Init(&st);
    in = base_in(1000u);
    (void)WgLadder_Step(&st, &in);

    in = base_in(1000u + WG_LADDER_REBUILD_MS + 5000u);
    in.aliveAge_ms = STALE_MS + 1u;         /* just over the line */
    TEST_ASSERT(WgLadder_Step(&st, &in) == wgLadder_rebuildRotate);
}

/* ============================================================================
 * Rung 1 — rebuild and rotate, repeating
 * ========================================================================= */

static void test_rung1_fires_at_the_cadence(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    /* Just under three cadences of silence. */
    drive(&st, &t, (3u * WG_LADDER_REBUILD_MS) - 10000u, 1, 1, 1, c);

    TEST_ASSERT(c[wgLadder_rebuildRotate] == 2u);
    TEST_ASSERT(c[wgLadder_terminalReboot] == 0u);
    TEST_ASSERT(st.rung == 1u);
}

/* THE assertion this module exists for.  A rebuild must not defer the
 * escalation: if it did, rung 1 firing every 15 minutes would hold rung 3
 * permanently 15 minutes away and the terminal rung could never arrive. That
 * is exactly how 325 rebuilds achieved nothing over three days. */
static void test_rebuild_does_not_defer_escalation(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    /* Silent right through the terminal horizon, with rung 1 firing all the
     * way.  The terminal rung must still arrive on time. */
    drive(&st, &t, WG_LADDER_TERMINAL_MS + 60000u, 1, 1, 1, c);

    TEST_ASSERT(c[wgLadder_rebuildRotate] >= 20u);   /* ~24 rotations tried */
    TEST_ASSERT(c[wgLadder_reloadConfig] == 1u);     /* once per outage     */
    TEST_ASSERT(c[wgLadder_terminalReboot] == 1u);
}

/* ============================================================================
 * Rung 2 — reload the stored config, once per outage
 * ========================================================================= */

static void test_rung2_is_once_per_outage(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    drive(&st, &t, WG_LADDER_RELOAD_MS + (2u * WG_LADDER_REBUILD_MS), 1, 1, 1, c);
    TEST_ASSERT(c[wgLadder_reloadConfig] == 1u);

    /* The hub comes back, then goes away again: that is a NEW outage and rung
     * 2 is due once more. */
    {
        sWgLadderIn in = base_in(t);
        (void)WgLadder_Step(&st, &in);          /* evidence: clock reset */
    }
    t += 5000u;
    memset(c, 0, sizeof(c));
    drive(&st, &t, WG_LADDER_RELOAD_MS + WG_LADDER_REBUILD_MS, 1, 1, 1, c);
    TEST_ASSERT(c[wgLadder_reloadConfig] == 1u);
}

/* ============================================================================
 * Rung 3 — terminal, bounded and gated
 * ========================================================================= */

static void test_terminal_is_bounded(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    /* Four terminal horizons of silence, but the budget is three.  Without the
     * bound this is a board resetting itself every six hours forever while it
     * runs a live battery. */
    drive(&st, &t, 5u * WG_LADDER_TERMINAL_MS, 1, 1, 1, c);

    TEST_ASSERT(c[wgLadder_terminalReboot] == WG_LADDER_MAX_REBOOTS);
    TEST_ASSERT(st.rebootsUsed == WG_LADDER_MAX_REBOOTS);
    /* Rung 1 keeps working after the budget is gone — rotation is still the
     * move most likely to help. */
    TEST_ASSERT(c[wgLadder_rebuildRotate] > 20u);
}

/* With the local network down the fault is demonstrably off-board, so a reboot
 * cannot repair it and would only interrupt local control. */
static void test_terminal_gated_on_local_network(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    drive(&st, &t, 3u * WG_LADDER_TERMINAL_MS, 1, 1, 0 /* !localNetOk */, c);

    TEST_ASSERT(c[wgLadder_terminalReboot] == 0u);
    TEST_ASSERT(c[wgLadder_rebuildRotate] > 20u);   /* still rotating */
}

/* The budget is per power-on, so it has to survive the very reset it counts. */
static void test_reboot_budget_is_restored(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    WgLadder_RestoreReboots(&st, WG_LADDER_MAX_REBOOTS);
    drive(&st, &t, 3u * WG_LADDER_TERMINAL_MS, 1, 1, 1, c);
    TEST_ASSERT(c[wgLadder_terminalReboot] == 0u);

    /* And a nonsense value cannot widen the budget. */
    WgLadder_Init(&st);
    WgLadder_RestoreReboots(&st, 250u);
    TEST_ASSERT(st.rebootsUsed == WG_LADDER_MAX_REBOOTS);
}

/* ============================================================================
 * Intent — §5.1, and the bug a naive fix for it introduces
 * ========================================================================= */

/* A tunnel that never started is retried.  Before this the two gates that
 * would have retried it both keyed on `running`, the flag the failure clears,
 * so one failed WgLink_Start() stranded the board until a power cycle. */
static void test_failed_start_is_retried(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    drive(&st, &t, (3u * WG_LADDER_REBUILD_MS) - 10000u, 1, 0 /* !running */, 1, c);

    TEST_ASSERT(c[wgLadder_start] == 2u);
    TEST_ASSERT(c[wgLadder_rebuildRotate] == 0u);   /* nothing to rebuild yet */
}

/* THE assertion that keeps the fix honest.  An operator "wg stop" must never
 * be undone by the ladder — the naive way to close §5.1 is to drop the
 * `running` gate, which resurrects a tunnel somebody deliberately stopped. */
static void test_stopped_tunnel_is_never_restarted(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };
    uint32_t  end;

    WgLadder_Init(&st);
    end = t + (7u * 24u * 3600u * 1000u);           /* a week */
    while (t < end) {
        sWgLadderIn in = base_in(t);
        eWgLadderAction a;

        in.running     = 0u;
        in.wantRunning = 0u;                        /* explicitly stopped */
        in.aliveAge_ms = WG_LADDER_AGE_NEVER;

        a = WgLadder_Step(&st, &in);
        if ((unsigned)a < (unsigned)wgLadder_last) {
            c[a]++;
        }
        t += 5000u;
    }

    TEST_ASSERT(c[wgLadder_start] == 0u);
    TEST_ASSERT(c[wgLadder_rebuildRotate] == 0u);
    TEST_ASSERT(c[wgLadder_terminalReboot] == 0u);
    TEST_ASSERT(WgLadder_OutageAge(&st, t) < 10000u);
}

/* An unprovisioned board stays quiet: it has no key, and hammering a hub it
 * cannot authenticate to is the behaviour WgLink_Start() already refuses. */
static void test_unprovisioned_board_stays_quiet(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };
    uint32_t  end = t + (2u * WG_LADDER_TERMINAL_MS);

    WgLadder_Init(&st);
    while (t < end) {
        sWgLadderIn in = base_in(t);
        eWgLadderAction a;

        in.running     = 0u;
        in.hasIdentity = 0u;
        in.aliveAge_ms = WG_LADDER_AGE_NEVER;

        a = WgLadder_Step(&st, &in);
        if ((unsigned)a < (unsigned)wgLadder_last) {
            c[a]++;
        }
        t += 5000u;
    }

    TEST_ASSERT(c[wgLadder_start] == 0u);
    TEST_ASSERT(c[wgLadder_terminalReboot] == 0u);
}

/* ============================================================================
 * Recovery and hygiene
 * ========================================================================= */

static void test_recovery_clears_the_ladder(void)
{
    sWgLadder st;
    uint32_t  t = 1000u;
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    drive(&st, &t, WG_LADDER_RELOAD_MS + WG_LADDER_REBUILD_MS, 1, 1, 1, c);
    TEST_ASSERT(st.rung != 0u);

    {
        sWgLadderIn in = base_in(t);
        TEST_ASSERT(WgLadder_Step(&st, &in) == wgLadder_none);
    }
    TEST_ASSERT(st.rung == 0u);
    TEST_ASSERT(WgLadder_OutageAge(&st, t) == 0u);
}

/* sys_now() is a free-running uint32 of milliseconds and wraps every ~49.7
 * days.  A board that has been up that long is exactly the one whose recovery
 * must still work. */
static void test_clock_wrap(void)
{
    sWgLadder st;
    uint32_t  t = 0xFFFFFF00u;                      /* about to wrap */
    unsigned  c[wgLadder_last] = { 0 };

    WgLadder_Init(&st);
    drive(&st, &t, (2u * WG_LADDER_REBUILD_MS) + 10000u, 1, 1, 1, c);
    TEST_ASSERT(c[wgLadder_rebuildRotate] == 2u);
}

static void test_null_inputs_are_safe(void)
{
    sWgLadder   st;
    sWgLadderIn in = base_in(0u);

    WgLadder_Init(&st);
    WgLadder_Init(NULL);
    WgLadder_RestoreReboots(NULL, 1u);
    TEST_ASSERT(WgLadder_Step(NULL, &in) == wgLadder_none);
    TEST_ASSERT(WgLadder_Step(&st, NULL) == wgLadder_none);
    TEST_ASSERT(WgLadder_OutageAge(NULL, 0u) == 0u);
}

/* Every enumerator has a name, and none of them can break the JSON they are
 * emitted into. */
static void test_action_names(void)
{
    unsigned i;

    for (i = 0u; i < (unsigned)wgLadder_last; i++) {
        const char *n = WgLadder_ActionName((eWgLadderAction)i);
        TEST_ASSERT(n != NULL);
        TEST_ASSERT(n[0] != '\0');
        TEST_ASSERT(strchr(n, '"') == NULL);
        TEST_ASSERT(strchr(n, '\\') == NULL);
    }
    TEST_ASSERT(strcmp(WgLadder_ActionName(wgLadder_last), "?") == 0);
    TEST_ASSERT(strcmp(WgLadder_ActionName((eWgLadderAction)99), "?") == 0);
}

int main(void)
{
    RUN_TEST(test_healthy_tunnel_does_nothing);
    RUN_TEST(test_stale_evidence_is_not_evidence);
    RUN_TEST(test_rung1_fires_at_the_cadence);
    RUN_TEST(test_rebuild_does_not_defer_escalation);
    RUN_TEST(test_rung2_is_once_per_outage);
    RUN_TEST(test_terminal_is_bounded);
    RUN_TEST(test_terminal_gated_on_local_network);
    RUN_TEST(test_reboot_budget_is_restored);
    RUN_TEST(test_failed_start_is_retried);
    RUN_TEST(test_stopped_tunnel_is_never_restarted);
    RUN_TEST(test_unprovisioned_board_stays_quiet);
    RUN_TEST(test_recovery_clears_the_ladder);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_null_inputs_are_safe);
    RUN_TEST(test_action_names);

    printf("\n%s: %d failure(s)\n", __FILE__, test_failures);
    return test_failures == 0 ? 0 : 1;
}

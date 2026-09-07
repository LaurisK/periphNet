/**
 * Unit tests for App/Cluster/cluster_calc.c — the closed-loop limit search,
 * its gate, the restart triggers, the sanitiser, the slew, the aggregation
 * and the divergence detectors
 * (docs/design_battery_cluster.md §3, test surface in §5).
 *
 * THE CASES THAT MATTER MOST ARE THE ONES THAT PUT A TOO-HIGH CURRENT LIMIT
 * ON A LIVE INVERTER, and each of them is named after the defect it closes:
 *
 *   loop_does_not_update_when_the_bus_is_not_binding    §3.2.1, the trap
 *   loop_never_exceeds_min_limit_over_share             L2, the gate's ceiling
 *   restart_on_member_leave_never_raises_the_limit      B2 / L3
 *   loop_load_does_not_wrap_at_an_absurd_current        L4
 *   charge_limit_is_zero_when_charge_is_forbidden       B3
 *
 * No board, no RTOS, no flash: the core is a pure function of a snapshot.
 */

#include "test_util.h"

#include "App/Cluster/cluster.h"
#include "App/Cluster/cluster_calc.h"
#include "App/Cluster/cluster_cfg.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Fixture
 * ============================================================================ */

/* The slew is a separate mechanism and most loop cases want it out of the way.
 * At the parse-time maximum rate and the clamped maximum dt, one tick moves
 * 200 A — so a rise reaches its target in one step and the LOOP is what the
 * assertion sees. */
#define FAST_RISE   CLUSTER_RISE_MAX_MA_PER_S
#define BIG_DT      CLUSTER_DT_MAX_MS

typedef struct {
    sClusterPackIn    in[CLUSTER_PACK_MAX];
    sClusterTune      tune;
    sClusterCalcState st;
    sClusterScratch   sc;
    sClusterResult    out;
    uint32_t          now_ms;
} sFix;

static void fix_init(sFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    ClusterCfg_Defaults(&f->tune);
    f->tune.riseRate_mA_per_s = FAST_RISE;
}

/** Derate out of the way: the convergence table in §3.2 is stated at
 *  derate = 1000, and a test that folded 0.8 into every expected number would
 *  be asserting the derate rather than the loop. */
static void fix_no_derate(sFix *f)
{
    f->tune.chargeDerate_pm    = 1000u;
    f->tune.dischargeDerate_pm = 1000u;
}

static void pack_online(sClusterPackIn *p, uint8_t idx,
                        uint32_t limChg_mA, uint32_t limDsg_mA)
{
    (void)memset(p, 0, sizeof(*p));
    p->packIdx           = idx;
    p->present           = 1u;
    p->cond              = (uint8_t)packCond_online;
    p->caps              = (uint32_t)packCap_currentLimits |
                           (uint32_t)packCap_capacityAh    |
                           (uint32_t)packCap_temperatures  |
                           (uint32_t)packCap_switchState;
    p->chargeLimit_mA    = limChg_mA;
    p->dischargeLimit_mA = limDsg_mA;
    p->voltage_mV        = 51200u;
    p->chargeSwitch      = (uint8_t)packSwitch_closed;
    p->dischargeSwitch   = (uint8_t)packSwitch_closed;
    p->elecAge_ms        = 100u;
    p->socConf_pm        = 1000u;
    p->sohConf_pm        = 1000u;
}

static void step(sFix *f, uint8_t n, uint32_t dt_ms)
{
    f->now_ms += dt_ms;
    TEST_ASSERT(ClusterCalc_Solve(f->in, n, &f->tune, &f->st, &f->sc,
                                  f->now_ms, &f->out) == cluErr_ok);
}

/** Run until the published value stops moving.  THE FIRST TICK ALWAYS MOVES
 *  BY THE STEP FLOOR ALONE — st->started is 0, so dt is 0, because there is
 *  no previous tick to measure an interval from — and at FAST_RISE each tick
 *  after that adds 200 A.  Iterating rather than counting keeps the loop
 *  assertions about the LOOP instead of about how many ticks a ramp took. */
static void settle(sFix *f, uint8_t n)
{
    unsigned t;

    for (t = 0u; t < 16u; t++) {
        const uint32_t chg = f->out.pub.chargeLimit_mA;
        const uint32_t dsg = f->out.pub.dischargeLimit_mA;

        step(f, n, BIG_DT);
        if ((t > 0u) && (f->out.pub.chargeLimit_mA == chg) &&
            (f->out.pub.dischargeLimit_mA == dsg)) {
            return;
        }
    }
    TEST_ASSERT(0);         /* the slew never settled: the test is wrong */
}

/* ============================================================================
 * A — shapes and invariants
 * ============================================================================ */

static void test_struct_sizes_are_as_budgeted(void)
{
    /* R4.1: main SRAM is the binding constraint and these are what is spent.
     * A silent growth here is 16 bytes per pack per buffer. */
    TEST_ASSERT(sizeof(sClusterOutput) == 112u);
    TEST_ASSERT(sizeof(sClusterMember) == 32u);
    TEST_ASSERT(sizeof(sClusterPackIn) == 64u);
    TEST_ASSERT(sizeof(sClusterCalcState) == 112u);
    TEST_ASSERT(sizeof(sClusterScratch) == 96u);
    TEST_ASSERT(sizeof(sClusterTune) == 36u);
    TEST_ASSERT(sizeof(sClusterCfg) == 180u);
}

static void test_pack_max_equals_pack_module_max(void)
{
    /* Spelled independently in cluster.h precisely so this can be a real
     * comparison rather than 8 == 8 by substitution. */
    TEST_ASSERT(CLUSTER_PACK_MAX == PACK_MAX);
}

static void test_enum_bits_are_distinct(void)
{
    const uint32_t alarms[] = {
        cluAlarm_noMembers, cluAlarm_allOffline, cluAlarm_memberLost,
        cluAlarm_socDiverge, cluAlarm_shareDiverge, cluAlarm_busSplit,
        cluAlarm_circulating, cluAlarm_chargeForbidden,
        cluAlarm_dischargeForbid, cluAlarm_voltLimitMissing,
        cluAlarm_nameUnresolved, cluAlarm_implausible,
        cluAlarm_packCfgChanged,
    };
    const uint32_t flags[] = {
        cluMemFlag_bindingCharge, cluMemFlag_bindingDischarge,
        cluMemFlag_socOutlier, cluMemFlag_shareOutlier,
        cluMemFlag_circulating, cluMemFlag_limitFell, cluMemFlag_joined,
        cluMemFlag_left, cluMemFlag_implausible, cluMemFlag_limitSaturated,
    };
    const uint32_t fields[] = {
        cluField_voltage, cluField_current, cluField_soc, cluField_soh,
        cluField_chargeLimit, cluField_dischargeLimit,
        cluField_chargeVoltLimit, cluField_dischargeVoltLimit,
        cluField_temperature, cluField_switches,
    };
    uint32_t seen;
    unsigned i;

    /* packCap_voltageLimits was once added on a bit packCap_cellEstimator
     * already held — found on hardware, latent only because neither bit was
     * produced yet.  This is that bug, pre-empted. */
    seen = 0u;
    for (i = 0u; i < (sizeof(alarms) / sizeof(alarms[0])); i++) {
        TEST_ASSERT((seen & alarms[i]) == 0u);
        seen |= alarms[i];
    }
    seen = 0u;
    for (i = 0u; i < (sizeof(flags) / sizeof(flags[0])); i++) {
        TEST_ASSERT((seen & flags[i]) == 0u);
        seen |= flags[i];
    }
    seen = 0u;
    for (i = 0u; i < (sizeof(fields) / sizeof(fields[0])); i++) {
        TEST_ASSERT((seen & fields[i]) == 0u);
        seen |= fields[i];
    }
    /* sClusterMember.flags must stay wide enough for the whole enum. */
    TEST_ASSERT(sizeof(((sClusterMember *)0)->flags) * 8u >= 32u);
}

static void test_enum_values_are_not_renumbered(void)
{
    /* They reach HTTP and the persisted configuration record. */
    TEST_ASSERT((int)cluCond_unprovisioned == 0);
    TEST_ASSERT((int)cluCond_online        == 3);
    TEST_ASSERT((int)cluCond_last          == 4);
    TEST_ASSERT((int)cluMember_unresolved    == 0);
    TEST_ASSERT((int)cluMember_participating == 4);
    TEST_ASSERT((int)cluWhy_none        == 0);
    TEST_ASSERT((int)cluWhy_implausible == 7);
    TEST_ASSERT((int)cluLoop_idle      == 0);
    TEST_ASSERT((int)cluLoop_converged == 3);
    TEST_ASSERT((int)cluRestart_none      == 0);
    TEST_ASSERT((int)cluRestart_permitted == 7);
    TEST_ASSERT((int)cluLimitWhy_noParticipant == 0);
    TEST_ASSERT((int)cluLimitWhy_ceiling       == 6);
    TEST_ASSERT((int)cluErr_ok    ==  0);
    TEST_ASSERT((int)cluErr_busy  == -6);
}

/* ============================================================================
 * B — the closed-loop search
 * ============================================================================ */

/** THE GREEDY CASE from §3.2's table: L = (100, 100) A, f = (0.8, 0.2),
 *  cold start 100 A, one binding measurement -> 112.5 A, and that is the
 *  fixed point. */
static void test_loop_converges_in_one_step_greedy(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);

    settle(&f, 2u);
    /* The safe opening value is min(L_i), NOT max: with L = (10, 100) A and
     * the small pack taking 60 % of the bus, a start at 100 A would put 60 A
     * through a 10 A pack for at least one tick. */
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 100000u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 100000u);

    /* Now drive the bus TO the limit, shared 80/20. */
    f.in[0].current_mA = 80000;
    f.in[1].current_mA = 20000;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 800u);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 112500u);
    TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_binding);
    TEST_ASSERT(f.out.member[0].flags & cluMemFlag_bindingCharge);

    /* And it STAYS there: the fixed point is independent of A. */
    f.in[0].current_mA = 90000;
    f.in[1].current_mA = 22500;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 112500u);
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_converged);
}

/** THE IDENTICAL CASE: f = (0.5, 0.5) gives the full sum, 180 A at the 90 %
 *  setpoint, which is what "identical packs get N x L" has to mean once a
 *  margin exists. */
static void test_loop_converges_in_one_step_identical(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    settle(&f, 2u);

    f.in[0].current_mA = 50000;
    f.in[1].current_mA = 50000;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 500u);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 180000u);
}

/** §3.2.1 — THE ONE THING THAT MAKES THE MECHANISM DANGEROUS IF OMITTED.
 *  100 A published, 10 A drawn: the ungated rule computes 1125 A and then
 *  LATCHES there, reporting itself converged at ten times too high. */
static void test_loop_does_not_update_when_the_bus_is_not_binding(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    settle(&f, 2u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 100000u);

    f.in[0].current_mA = 8000;      /* 10 % of the published limit */
    f.in[1].current_mA = 2000;
    step(&f, 2u, BIG_DT);

    TEST_ASSERT(f.out.pub.chargeLoop_mA  == 100000u);   /* NOT 1125000 */
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 100000u);
    TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_start);
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_searching);
}

/** Defect L2, as a property.  The gate gives published <= 1000|S|/bindFrac
 *  and the update is loop' = published x target / loadMax, so with
 *  bindFrac_pm >= loadTarget_pm the loop can never publish above the binding
 *  pack's own limit.  Ten thousand pseudorandom two-pack buses. */
static void test_loop_never_exceeds_min_limit_over_share(void)
{
    unsigned iter;
    unsigned violations = 0u;

    srand(20260906u);
    for (iter = 0u; iter < 10000u; iter++) {
        sFix     f;
        uint32_t L0 = 1000u + (uint32_t)(rand() % 300000);
        uint32_t L1 = 1000u + (uint32_t)(rand() % 300000);
        uint32_t f0 = 1u + (uint32_t)(rand() % 999);   /* pack 0's share, pm */
        unsigned t;

        fix_init(&f);
        fix_no_derate(&f);
        pack_online(&f.in[0], 0u, L0, L0);
        pack_online(&f.in[1], 1u, L1, L1);

        for (t = 0u; t < 12u; t++) {
            /* The bus follows the published limit exactly, split by a fixed
             * share — the operating point where the gate always opens and the
             * loop is therefore free to climb as far as it can. */
            const uint32_t bus  = f.out.pub.chargeLimit_mA;
            const uint32_t was  = f.out.pub.chargeLoop_mA;
            uint64_t       i0, i1;

            i0 = ((uint64_t)bus * f0) / 1000u;
            i1 = (uint64_t)bus - i0;
            f.in[0].current_mA = (int32_t)i0;
            f.in[1].current_mA = (int32_t)i1;
            step(&f, 2u, BIG_DT);

            /* m = min over the charging packs of L_i / f_i, computed from the
             * currents THIS TICK ACTUALLY SAW.  Taking it from the nominal
             * share instead would make integer truncation in the split — not
             * the loop — the thing under test. */
            if ((f.out.pub.chargeLoop_mA != was) && (bus > 0u)) {
                uint64_t m = (uint64_t)0xFFFFFFFFFFFFULL;

                if (i0 > 0u) {
                    const uint64_t m0 = ((uint64_t)L0 * bus) / i0;

                    if (m0 < m) { m = m0; }
                }
                if (i1 > 0u) {
                    const uint64_t m1 = ((uint64_t)L1 * bus) / i1;

                    if (m1 < m) { m = m1; }
                }
                if ((uint64_t)f.out.pub.chargeLoop_mA > m) {
                    violations++;
                }
            }
        }
    }
    TEST_ASSERT(violations == 0u);
}

/** THE TRUNCATION DEFECT, as the concrete case that found it.  load_pm feeds
 *  a DIVISION, so a load truncated down yields a limit rounded up, and the
 *  error is unbounded in ratio exactly where load_pm is small.  Found by the
 *  property sweep above: published 10 mA (the slew's opening step) against a
 *  4.779 A pack taking 70 % of the bus.  True load 1.4 pm, truncated to 1, and
 *  the loop leapt to 9.0 A against a 6.827 A ceiling — 32 % over, with the
 *  gate open and every other rule satisfied. */
static void test_loop_load_rounds_up_so_a_small_bus_cannot_inflate_the_limit(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 125973u, 125973u);
    pack_online(&f.in[1], 1u,   4779u,   4779u);

    step(&f, 1u + 1u, 250u);            /* opening step: published = 10 mA   */
    TEST_ASSERT(f.out.pub.chargeLimit_mA == CLUSTER_RISE_STEP_MIN_MA);

    f.in[0].current_mA = 3;             /* 30 % of a 10 mA bus               */
    f.in[1].current_mA = 7;
    step(&f, 2u, BIG_DT);

    /* m = min(L_i x |S| / I_i) = min(125973 x 10 / 3, 4779 x 10 / 7)
     *   = min(419910, 6827) = 6827 mA. */
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 2u);      /* ceil(1.465), not 1 */
    TEST_ASSERT(f.out.pub.chargeLoop_mA <= 6827u);
}

/** THE RAMP-IN TRAP, found on hardware and not by any of the cases above.
 *
 *  The slew starts at zero and walks upward.  While it is walking, the emitted
 *  value is not a limit the inverter is respecting — it is a number the board
 *  is moving through the bus current — so the gate opens for a reason that has
 *  nothing to do with the battery being at its limit, and the update ratchets
 *  the loop DOWN to wherever the ramp crossed the load.
 *
 *  IT THEN LATCHES: re-opening the gate needs a bus draw at 90 % of a limit
 *  the loop has just made too small to reach.
 *
 *  MEASURED ON BOARD 1 (Pd1.1.50, 2026-09-07), and these are its real numbers:
 *  a 150 A pack, a steady 1.007 A charge, the default 5 A/s rise.  It settled
 *  at a published charge limit of 4.112 A with `why: notBinding` and stayed
 *  there — a 36x throughput loss.  This test reproduces it exactly and must
 *  FAIL against the pre-fix loop. */
static void test_loop_does_not_learn_while_the_slew_is_still_ramping_in(void)
{
    sFix     f;
    unsigned t;

    fix_init(&f);
    f.tune.riseRate_mA_per_s = CLUSTER_DFLT_RISE_MA_PER_S;   /* 5 A/s        */
    pack_online(&f.in[0], 0u, 150000u, 150000u);
    f.in[0].current_mA = 1007;                  /* the board's real reading  */

    /* 40 s of 250 ms ticks: long enough for the ramp to cross 1.007 A many
     * times over and to reach the derated start value. */
    for (t = 0u; t < 160u; t++) {
        step(&f, 1u, 250u);
    }

    /* The loop must still be holding the SAFE OPENING VALUE, min(L_i) — it
     * has been given no measurement worth learning from. */
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 150000u);
    /* And the emitted value is that, derated: 150 A x 0.80 = 120 A, which is
     * what the JK itself would have sent.  NOT 4.1 A. */
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 120000u);
    TEST_ASSERT(f.st.chgBindSeen == 0u);
}

/** The other half of the same rule: once the slew HAS settled, a genuine
 *  measurement at the limit is still learned from.  Without this the fix
 *  above could be "never learn anything" and pass. */
static void test_loop_still_learns_once_the_slew_has_settled(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);                             /* ramp in, no current       */
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 100000u);
    TEST_ASSERT(f.st.chgSlewing == 0u);

    f.in[0].current_mA = 100000;                /* the bus really is at it   */
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 1000u);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 90000u);      /* 0.9 x 100 A      */
    TEST_ASSERT(f.st.chgBindSeen == 1u);
}

/** Review B5, and the reason it is not merely a division guard: a zero limit
 *  with current flowing is a pack ALREADY over its rating.  It saturates at
 *  1000 per-mille, which makes the loop reduce. */
static void test_loop_saturates_load_for_a_zero_limit_pack(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    settle(&f, 2u);

    /* Pack 1's own limit collapses to zero while it still carries current. */
    f.in[1].chargeLimit_mA = 0u;
    f.in[0].current_mA = 60000;
    f.in[1].current_mA = 40000;
    step(&f, 2u, BIG_DT);

    TEST_ASSERT(f.out.member[1].flags & cluMemFlag_limitSaturated);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 1000u);
    /* It also left the participating set, so the leave clamp applies and the
     * limit may only go DOWN. */
    TEST_ASSERT(f.out.pub.chargeLoop_mA <= 100000u);
}

static void test_loop_ignores_a_zero_loadmax(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);
    /* No current at all in either direction: the degenerate case of the gate,
     * and it must never reach the division. */
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 100000u);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 0u);
}

/** Defect L4.  1000 A against a 15 A limit is load_pm = 66 666, and a plain
 *  narrowing to uint16 sends 65536 to exactly 0 — the value the loop reads as
 *  "no current, do not update".  The most extreme overload the system can
 *  have must not be indistinguishable from no current at all. */
static void test_loop_load_does_not_wrap_at_an_absurd_current(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 15000u, 15000u);
    settle(&f, 1u);

    f.in[0].current_mA = 1000000;               /* 1000 A, at the domain edge */
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 0xFFFFu);      /* saturated     */
    TEST_ASSERT(f.out.member[0].load_pm    == 0xFFFFu);
    /* And it acted on it: the limit collapsed rather than holding. */
    TEST_ASSERT(f.out.pub.chargeLoop_mA < 15000u);
}

/** abs(INT32_MIN) is undefined behaviour and sign-extends through a
 *  (uint64_t) cast.  The value is outside the plausible domain, so the pack
 *  is DROPPED — but it must be dropped by an evaluation that did not trap. */
static void test_abs_of_int32_min_does_not_sign_extend(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[1].current_mA = INT32_MIN;
    step(&f, 2u, 250u);

    TEST_ASSERT(f.out.member[1].state == (uint8_t)cluMember_absent);
    TEST_ASSERT(f.out.member[1].why   == (uint8_t)cluWhy_implausible);
    TEST_ASSERT(f.out.member[1].flags & cluMemFlag_implausible);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_implausible);
    TEST_ASSERT(f.out.pub.onlineCnt == 1u);
    TEST_ASSERT(f.out.sanitised == 1u);
}

/** A discharging bus must not tune the CHARGE limit (§3.5 item 6). */
static void test_loop_uses_only_the_controlled_direction(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);

    f.in[0].current_mA = -95000;                /* hard discharge            */
    step(&f, 1u, BIG_DT);

    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 0u);        /* nothing charging */
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 100000u);      /* untouched        */
    TEST_ASSERT(f.out.pub.dischargeLoadMax_pm == 950u);   /* the loop that
                                                             should have moved */
    TEST_ASSERT(f.out.pub.dischargeLoop_mA != 100000u);
}

/* ============================================================================
 * C — the start value and the restart triggers
 * ============================================================================ */

static void test_start_is_min_limit_not_max(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 10000u, 10000u);      /* the small pack        */
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    settle(&f, 2u);

    TEST_ASSERT(f.out.pub.chargeLoop_mA == 10000u);
    TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_start);
}

static void test_restart_on_a_participant_limit_decrease(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    settle(&f, 2u);
    f.in[0].current_mA = 150000;
    f.in[1].current_mA = 150000;
    step(&f, 2u, BIG_DT);                       /* earn a binding sample     */
    TEST_ASSERT(f.st.chgBindSeen == 1u);

    f.in[0].chargeLimit_mA = 30000;             /* a 90 % derate by the pack */
    step(&f, 2u, BIG_DT);
    TEST_ASSERT(f.out.pub.lastRestart == (uint8_t)cluRestart_limitFell);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 30000u);
    TEST_ASSERT(f.out.member[0].flags & cluMemFlag_limitFell);
}

static void test_no_restart_on_a_participant_limit_increase(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);

    f.in[0].chargeLimit_mA = 300000;    /* safe at the smaller limit ⟹ safe  */
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.lastRestart == (uint8_t)cluRestart_none);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 100000u);
}

/** Noise inside the deadband must not restart; a MONOTONIC RAMP must, and
 *  that is why the reference is latched at restart rather than refreshed
 *  every tick. */
static void test_deadband_suppresses_noise_but_not_a_ramp(void)
{
    sFix     f;
    unsigned t;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.limitDeadband_pm = 20u;                  /* 2 %                   */
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);

    /* +/- 1 % noise: never trips. */
    for (t = 0u; t < 6u; t++) {
        f.in[0].chargeLimit_mA = ((t & 1u) != 0u) ? 99000u : 101000u;
        step(&f, 1u, BIG_DT);
        TEST_ASSERT(f.out.pub.lastRestart == (uint8_t)cluRestart_none);
    }

    /* A 1 %-per-tick ramp down: against a per-tick reference this would never
     * trip at all, and the loop would hold a value learned against a pack
     * that has since derated itself into the ground. */
    fix_init(&f);
    fix_no_derate(&f);
    f.tune.limitDeadband_pm = 20u;
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);
    {
        uint32_t L = 100000u;
        int      tripped = 0;

        for (t = 0u; t < 5u; t++) {
            L -= 1000u;
            f.in[0].chargeLimit_mA = L;
            step(&f, 1u, BIG_DT);
            if (f.out.pub.lastRestart == (uint8_t)cluRestart_limitFell) {
                tripped = 1;
                break;
            }
        }
        TEST_ASSERT(tripped == 1);
    }
}

/** REVIEW B2, REOPENED AS L3.  Three packs, L = (300, 300, 30) A, the loop
 *  has earned 60 A.  The 30 A pack's electrical group goes stale — IT IS
 *  STILL BOLTED TO THE BUSBAR.  min(L_i) over the new set is 300 A, so an
 *  unclamped restart would raise the emitted limit toward 255 A and put ~127 A
 *  through a 30 A pack that is invisible to loadMax. */
static void test_restart_on_member_leave_never_raises_the_limit(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    pack_online(&f.in[2], 2u,  30000u,  30000u);
    settle(&f, 3u);

    /* Teach the loop a modest value: the small pack is greedy. */
    f.in[0].current_mA =  2000;
    f.in[1].current_mA =  2000;
    f.in[2].current_mA = 26000;
    step(&f, 3u, BIG_DT);
    {
        const uint32_t learned = f.out.pub.chargeLoop_mA;

        TEST_ASSERT(learned <= 300000u);

        /* And now it vanishes from view. */
        f.in[2].elecAge_ms = 400000u;
        step(&f, 3u, BIG_DT);
        TEST_ASSERT(f.out.member[2].state == (uint8_t)cluMember_stale);
        TEST_ASSERT(f.out.pub.lastRestart == (uint8_t)cluRestart_memberLeft);
        TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_memberLost);
        /* THE CLAUSE THAT CLOSES B2: loop = min(loop_before, min_new(L_i)). */
        TEST_ASSERT(f.out.pub.chargeLoop_mA <= learned);
        TEST_ASSERT(f.out.pub.chargeLoop_mA < 300000u);
    }
}

static void test_restart_when_a_pack_joins(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[1].cond = (uint8_t)packCond_absent;
    settle(&f, 2u);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 300000u);

    /* The newcomer is small, and has never been measured. */
    f.in[1].cond = (uint8_t)packCond_online;
    f.in[1].chargeLimit_mA = 20000u;
    f.in[1].dischargeLimit_mA = 20000u;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT(f.out.pub.lastRestart == (uint8_t)cluRestart_memberJoined);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 20000u);
    TEST_ASSERT(f.out.member[1].flags & cluMemFlag_joined);
}

static void test_restart_after_hold_expiry(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.holdMaxAge_ms = 5000u;
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);
    f.in[0].current_mA = 95000;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.st.chgBindSeen == 1u);

    f.in[0].current_mA = 0;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_holding);
    step(&f, 1u, BIG_DT);
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.lastRestart == (uint8_t)cluRestart_holdExpired);
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_searching);
}

static void test_charge_and_discharge_loops_are_independent(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 200000u);
    settle(&f, 1u);
    TEST_ASSERT(f.out.pub.chargeLoop_mA    == 100000u);
    TEST_ASSERT(f.out.pub.dischargeLoop_mA == 200000u);

    f.in[0].current_mA = -190000;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 100000u);        /* untouched */
    TEST_ASSERT(f.out.pub.dischargeLoop_mA != 200000u);
}

static void test_zero_limit_pack_is_out_of_the_participating_set(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 0u, 100000u);
    step(&f, 1u, 250u);

    TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_noParticipant);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
    TEST_ASSERT(f.out.pub.dischargeWhy != (uint8_t)cluLimitWhy_noParticipant);
}

static void test_open_charge_switch_excludes_charge_only(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.in[0].chargeSwitch = (uint8_t)packSwitch_open;
    settle(&f, 1u);

    TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
    TEST_ASSERT(f.out.pub.chargeAllowed == 0u);
    TEST_ASSERT(f.out.pub.dischargeLimit_mA > 0u);
    TEST_ASSERT(f.out.pub.dischargeAllowed == 1u);
}

static void test_all_packs_offline_publishes_zero(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA > 0u);

    /* R2.6 holds BY CONSTRUCTION: an empty participating set is a zero limit
     * with no event and no branch of its own. */
    f.in[0].cond = (uint8_t)packCond_stale;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
    TEST_ASSERT(f.out.pub.dischargeLimit_mA == 0u);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_allOffline);
    TEST_ASSERT(f.out.pub.cond == (uint8_t)cluCond_absent);
    TEST_ASSERT(f.out.pub.valid == 0u);
}

/** The zaliakalnis reading: online at 2.2 s on the electrical group, and a
 *  different group at 362 s.  R3.3 judges on the group the arithmetic
 *  consumes, or a live pack is dropped and a dead number is trusted. */
static void test_electrically_stale_pack_is_excluded(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.in[0].elecAge_ms = 2200u;
    step(&f, 1u, 250u);
    TEST_ASSERT(f.out.member[0].state == (uint8_t)cluMember_participating);

    f.in[0].elecAge_ms = 362000u;
    step(&f, 1u, 250u);
    TEST_ASSERT(f.out.member[0].state == (uint8_t)cluMember_stale);
    TEST_ASSERT(f.out.member[0].why == (uint8_t)cluWhy_electricalStale);
    TEST_ASSERT(f.out.pub.onlineCnt == 0u);
}

/* ============================================================================
 * D — permission, and the limit that must agree with it
 * ============================================================================ */

/** REVIEW B3.  A forbidden direction publishes a ZERO limit as well as a
 *  cleared flag — encoding the refusal only in 0x35C, whose semantics are
 *  UNVERIFIED, while 0x351 (the field measured end-to-end as obeyed) still
 *  says 96 A is fine, is the same failure as a zero CVL, inverted. */
static void test_charge_limit_is_zero_when_charge_is_forbidden(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    settle(&f, 2u);
    {
        const uint32_t learned = f.out.pub.chargeLoop_mA;

        TEST_ASSERT(f.out.pub.chargeLimit_mA > 0u);

        /* A pack at cell over-voltage that has NOT isolated itself. */
        f.in[1].alarms = (uint32_t)packAlarm_cellOverVoltage;
        step(&f, 2u, BIG_DT);

        TEST_ASSERT(f.out.pub.chargeAllowed == 0u);
        TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
        TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_forbidden);
        TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_chargeForbidden);
        /* But the LOOP is untouched: a transient alarm must not destroy what
         * the loop learned, and chargeSlewed_mA still shows what it would
         * have said. */
        TEST_ASSERT(f.out.pub.chargeLoop_mA == learned);
        TEST_ASSERT(f.out.pub.chargeSlewed_mA > 0u);
        /* Discharge is untouched — permission is per direction. */
        TEST_ASSERT(f.out.pub.dischargeAllowed == 1u);
        TEST_ASSERT(f.out.pub.dischargeLimit_mA > 0u);
    }
}

/** THE CONVERSE DOES NOT HOLD.  A loop that has not yet earned headroom
 *  publishes a SMALL limit, not a refusal. */
static void test_zero_limit_does_not_imply_forbidden(void)
{
    sFix f;

    fix_init(&f);
    f.tune.riseRate_mA_per_s = CLUSTER_RISE_MIN_MA_PER_S;
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    step(&f, 1u, 250u);                 /* first tick: the slew starts at 0  */

    TEST_ASSERT(f.out.pub.chargeLimit_mA < 100u);
    TEST_ASSERT(f.out.pub.chargeAllowed == 1u);
}

/** A pack that has already opened its own charge path does not need to stop
 *  the others; a pack that has not, does. */
static void test_overvoltage_pack_that_isolated_itself_does_not_veto(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[1].alarms       = (uint32_t)packAlarm_cellOverVoltage;
    f.in[1].chargeSwitch = (uint8_t)packSwitch_open;
    settle(&f, 2u);

    TEST_ASSERT(f.out.pub.chargeAllowed == 1u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA > 0u);
}

/** A fleet of types that cannot report switches must not be forbidden
 *  forever — including on the very first tick, when the slew is still at
 *  zero. */
static void test_all_unknown_switches_fall_back_to_permitted(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.in[0].chargeSwitch    = (uint8_t)packSwitch_unknown;
    f.in[0].dischargeSwitch = (uint8_t)packSwitch_unknown;
    step(&f, 1u, 250u);

    TEST_ASSERT(f.out.member[0].state == (uint8_t)cluMember_participating);
    TEST_ASSERT(f.out.pub.chargeAllowed == 1u);
    TEST_ASSERT(f.out.pub.dischargeAllowed == 1u);
}

static void test_permitting_again_ramps_from_where_it_was(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);
    f.in[0].alarms = (uint32_t)packAlarm_cellOverVoltage;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeAllowed == 0u);

    f.in[0].alarms = 0u;
    step(&f, 1u, BIG_DT);
    /* Without the restart the emitted limit would step 0 -> learned in one
     * tick, which is a current step into real cells. */
    TEST_ASSERT(f.out.pub.lastRestart == (uint8_t)cluRestart_permitted);
    TEST_ASSERT(f.out.pub.chargeAllowed == 1u);
}

/* ============================================================================
 * E — the slew
 * ============================================================================ */

static void test_slew_decrease_is_immediate_increase_is_limited(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.riseRate_mA_per_s = 5000u;       /* 5 A/s, the shipping default   */
    pack_online(&f.in[0], 0u, 200000u, 200000u);

    /* THE FIRST TICK HAS NO INTERVAL — st->started is 0, so dt is 0 and the
     * rise is the step floor alone.  That floor exists only so a slow rate
     * does not stall on integer truncation of (rate * dt)/1000. */
    step(&f, 1u, 250u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == CLUSTER_RISE_STEP_MIN_MA);
    TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_slew);
    step(&f, 1u, 250u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == (CLUSTER_RISE_STEP_MIN_MA + 1250u));
    step(&f, 1u, 250u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == (CLUSTER_RISE_STEP_MIN_MA + 2500u));

    /* A collapse arrives in full at the next publish. */
    f.in[0].chargeLimit_mA = 1000u;
    step(&f, 1u, 250u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 1000u);
}

static void test_slew_step_is_clamped_at_dt_max(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.riseRate_mA_per_s = 1000u;
    pack_online(&f.in[0], 0u, 500000u, 500000u);

    step(&f, 1u, 250u);
    /* A scheduling gap, a debugger halt or a config apply must not deliver a
     * giant step: dt is clamped at 2000 ms whatever the wall clock says. */
    step(&f, 1u, 600000u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA <= (250u + 2000u));
}

static void test_slew_is_wrap_safe_across_the_millisecond_rollover(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.now_ms = 0xFFFFFF00u;
    step(&f, 1u, 250u);
    step(&f, 1u, 250u);                 /* now_ms has wrapped past 2^32      */
    TEST_ASSERT(f.now_ms < 0x1000u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA > 0u);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 100000u);
}

static void test_slew_rate_at_the_parse_bound_does_not_overflow(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.riseRate_mA_per_s = CLUSTER_RISE_MAX_MA_PER_S;
    f.tune.limitMax_mA       = CLUSTER_LIMIT_MAX_MA;
    pack_online(&f.in[0], 0u, CLUSTER_LIMIT_MAX_MA, CLUSTER_LIMIT_MAX_MA);
    step(&f, 1u, CLUSTER_DT_MAX_MS);            /* no interval yet          */
    step(&f, 1u, CLUSTER_DT_MAX_MS);
    /* 1e5 x 2000 = 2e8, comfortably inside uint32 — the PARSE BOUND is what
     * holds that, which is why it is load-bearing arithmetic. */
    TEST_ASSERT(f.out.pub.chargeLimit_mA ==
                (CLUSTER_RISE_STEP_MIN_MA + 200000u));
}

/* ============================================================================
 * F — aggregation
 * ============================================================================ */

/** THE SODAS CASE.  593.2 Ah of 1254.7 is 47.3 %, not the 46 % a mean of the
 *  two SOCs gives.  Half the site's stored energy was invisible to the
 *  inverter, and this arithmetic is the whole reason the module exists. */
static void test_soc_is_charge_weighted_not_averaged(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].capacity_mAh  = 660000u;
    f.in[0].remaining_mAh = 138600u;        /* 21 %                          */
    f.in[0].soc_pm        = 210u;
    f.in[1].capacity_mAh  = 594700u;
    f.in[1].remaining_mAh = 454600u;
    f.in[1].soc_pm        = 710u;
    f.in[0].nameplate_mAh = 660000u;
    f.in[1].nameplate_mAh = 660000u;

    step(&f, 2u, 250u);
    TEST_ASSERT(f.out.pub.remaining_mAh == 593200u);
    TEST_ASSERT(f.out.pub.capacity_mAh  == 1254700u);
    TEST_ASSERT(f.out.pub.soc_pm == 472u);          /* NOT 460               */
    TEST_ASSERT(f.out.pub.fields & cluField_soc);
    /* SOH over the SAME set, else it is meaningless. */
    TEST_ASSERT(f.out.pub.soh_pm == 950u);          /* 1254700/1320000       */
    TEST_ASSERT(f.out.pub.fields & cluField_soh);
}

static void test_soc_does_not_overflow_at_eight_thousand_amp_hours(void)
{
    sFix    f;
    uint8_t i;

    fix_init(&f);
    for (i = 0u; i < 8u; i++) {
        pack_online(&f.in[i], i, 300000u, 300000u);
        f.in[i].capacity_mAh  = 1000000u;       /* 1000 Ah each              */
        f.in[i].remaining_mAh =  750000u;
        f.in[i].nameplate_mAh = 1000000u;
    }
    step(&f, 8u, 250u);
    /* 8 000 000 mAh x 1000 is 8e9, past uint32 — the intermediate is uint64
     * or this reads as garbage. */
    TEST_ASSERT(f.out.pub.soc_pm == 750u);
    TEST_ASSERT(f.out.pub.soh_pm == 1000u);
}

static void test_soc_and_soh_fields_clear_when_their_denominator_is_zero(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.in[0].capacity_mAh  = 0u;
    f.in[0].nameplate_mAh = 0u;
    step(&f, 1u, 250u);

    /* A cleared bit means the field reads zero and MEANS NOTHING; no division
     * was reachable. */
    TEST_ASSERT((f.out.pub.fields & cluField_soc) == 0u);
    TEST_ASSERT((f.out.pub.fields & cluField_soh) == 0u);
    TEST_ASSERT(f.out.pub.soc_pm == 0u);
}

/** §7.4 — the cluster DECLARES the dependency and refuses to publish.  A zero
 *  CVL is not "no limit", it is an instruction to stop charging. */
static void test_voltage_limits_are_invalid_when_nobody_advertises_them(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    step(&f, 1u, 250u);

    TEST_ASSERT((f.out.pub.fields & cluField_chargeVoltLimit) == 0u);
    TEST_ASSERT((f.out.pub.fields & cluField_dischargeVoltLimit) == 0u);
    TEST_ASSERT(f.out.pub.chargeVoltLimit_mV == 0u);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_voltLimitMissing);
}

/** THE R3.1 ANSWER, ASSERTED.  A contactor opens to CURRENT, not to voltage:
 *  a pack at cell over-voltage opens its charge MOS and leaves P_chg, and if
 *  CVL were taken over participants only its low ceiling would vanish from
 *  the published figure at exactly the moment it matters. */
static void test_cvl_includes_a_pack_that_refuses_to_charge(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].caps |= (uint32_t)packCap_voltageLimits;
    f.in[1].caps |= (uint32_t)packCap_voltageLimits;
    f.in[0].chargeVoltLimit_mV    = 56000u;
    f.in[0].dischargeVoltLimit_mV = 44000u;
    f.in[1].chargeVoltLimit_mV    = 53500u;     /* the refusing pack          */
    f.in[1].dischargeVoltLimit_mV = 46000u;
    f.in[1].chargeSwitch          = (uint8_t)packSwitch_open;
    f.in[1].chargeLimit_mA        = 0u;

    step(&f, 2u, 250u);
    TEST_ASSERT(f.out.member[1].state == (uint8_t)cluMember_participating);
    TEST_ASSERT(f.out.pub.chargeVoltLimit_mV == 53500u);        /* MIN        */
    TEST_ASSERT(f.out.pub.dischargeVoltLimit_mV == 46000u);     /* MAX        */
    TEST_ASSERT(f.out.pub.fields & cluField_chargeVoltLimit);
}

static void test_voltage_is_the_mean_and_the_spread_is_reported(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].voltage_mV = 51206u;
    f.in[1].voltage_mV = 51212u;
    step(&f, 2u, 250u);

    TEST_ASSERT(f.out.pub.voltage_mV == 51209u);
    TEST_ASSERT(f.out.pub.voltSpread_mV == 6u);
    /* THE FIELD'S 6 mV MUST NOT TRIP the 500 mV bus-split threshold. */
    TEST_ASSERT((f.out.pub.clusterAlarms & cluAlarm_busSplit) == 0u);

    f.in[1].voltage_mV = 51806u;
    step(&f, 2u, 250u);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_busSplit);
}

static void test_temperature_is_max_and_alarms_are_ored(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].tempMax_dC = 227; f.in[0].tempMin_dC = 210;
    f.in[1].tempMax_dC = 310; f.in[1].tempMin_dC = 180;
    f.in[0].alarms = (uint32_t)packAlarm_cellImbalance;
    f.in[1].alarms = (uint32_t)packAlarm_overTemperature;
    step(&f, 2u, 250u);

    TEST_ASSERT(f.out.pub.tempMax_dC == 310);
    TEST_ASSERT(f.out.pub.tempMin_dC == 180);
    TEST_ASSERT(f.out.pub.fields & cluField_temperature);
    TEST_ASSERT(f.out.pub.alarms == ((uint32_t)packAlarm_cellImbalance |
                                     (uint32_t)packAlarm_overTemperature));
    /* An OFFLINE pack's alarms are NOT ored in: it is not on this bus. */
    f.in[1].cond = (uint8_t)packCond_absent;
    step(&f, 2u, 250u);
    TEST_ASSERT(f.out.pub.alarms == (uint32_t)packAlarm_cellImbalance);
}

static void test_confidence_is_the_worst_contributor_and_capped_while_searching(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].capacity_mAh = 100000u; f.in[0].remaining_mAh = 50000u;
    f.in[1].capacity_mAh = 100000u; f.in[1].remaining_mAh = 50000u;
    f.in[0].socConf_pm = 900u; f.in[1].socConf_pm = 400u;
    f.in[0].sohConf_pm = 800u; f.in[1].sohConf_pm = 200u;
    settle(&f, 2u);

    /* A cluster that has not yet measured its own capability should say so. */
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_searching);
    TEST_ASSERT(f.out.pub.socConf_pm == 300u);
    TEST_ASSERT(f.out.pub.sohConf_pm == 200u);
}

static void test_module_count_is_online_and_electrically_fresh(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    pack_online(&f.in[2], 2u, 100000u, 100000u);
    f.in[1].elecAge_ms = 400000u;               /* fresh enough to be online,
                                                   too old to do sums with   */
    f.in[2].cond = (uint8_t)packCond_absent;
    step(&f, 3u, 250u);

    TEST_ASSERT(f.out.pub.memberCnt == 3u);
    TEST_ASSERT(f.out.pub.onlineCnt == 1u);
    TEST_ASSERT(f.out.pub.cond == (uint8_t)cluCond_degraded);
}

/* ============================================================================
 * G — divergence, which is REPORTED and never a control input
 * ============================================================================ */

static void test_diverge_soc_flags_the_sodas_case_and_changes_no_limit(void)
{
    sFix     f;
    uint32_t before;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].capacity_mAh = 660000u; f.in[0].remaining_mAh = 138600u;
    f.in[1].capacity_mAh = 594700u; f.in[1].remaining_mAh = 454600u;
    f.in[0].soc_pm = 210u;
    f.in[1].soc_pm = 710u;
    settle(&f, 2u);
    before = f.out.pub.chargeLimit_mA;

    /* 710 - 210 = 500 pm, past the 200 pm threshold. */
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_socDiverge);
    TEST_ASSERT(f.out.member[0].flags & cluMemFlag_socOutlier);
    TEST_ASSERT(f.out.member[1].flags & cluMemFlag_socOutlier);

    /* And it changed NOTHING: the control response to a greedy pack is
     * already produced by the loop, and a second one would fight it. */
    f.in[0].soc_pm = 460u;
    f.in[1].soc_pm = 460u;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT((f.out.pub.clusterAlarms & cluAlarm_socDiverge) == 0u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == before);
}

static void test_share_is_an_observation_that_flags_the_greedy_pack(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].current_mA = -384;
    f.in[1].current_mA = -2738;
    step(&f, 2u, 250u);

    TEST_ASSERT(f.out.pub.current_mA == -3122);
    TEST_ASSERT(f.out.member[0].share_pm == 122u);
    TEST_ASSERT(f.out.member[1].share_pm == 877u);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_shareDiverge);
    TEST_ASSERT(f.out.member[1].flags & cluMemFlag_shareOutlier);
}

static void test_a_circulating_pack_is_flagged_and_the_loop_holds(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    settle(&f, 2u);

    /* Packs six millivolts apart at low bus current: one reads the opposite
     * sign outright.  A circulating bus SHRINKS |S| in each direction, so the
     * gate fails and the loop holds — which is the correct response. */
    f.in[0].current_mA =  40000;
    f.in[1].current_mA = -38000;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_circulating);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 100000u);
}

/* ============================================================================
 * H — degenerate paths and purity
 * ============================================================================ */

/** Open question 3, made enforceable.  n = 1 is the SAME code path — there is
 *  no `if (n == 1)` in cluster_calc.c — and the published limit is NOT L:
 *  at the limit loadMax = 1000 A/L, so the loop settles at loadTarget x L and
 *  the emitted value at loadTarget x derate x L. */
static void test_single_pack_is_the_pack(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    f.in[0].caps |= (uint32_t)packCap_voltageLimits;
    f.in[0].voltage_mV            = 51209u;
    f.in[0].capacity_mAh          = 660000u;
    f.in[0].remaining_mAh         = 330000u;
    f.in[0].nameplate_mAh         = 660000u;
    f.in[0].soc_pm                = 500u;
    f.in[0].tempMax_dC            = 227;
    f.in[0].tempMin_dC            = 210;
    f.in[0].chargeVoltLimit_mV    = 56000u;
    f.in[0].dischargeVoltLimit_mV = 44000u;
    settle(&f, 1u);

    /* Every published field is the pack's own... */
    TEST_ASSERT(f.out.pub.voltage_mV == 51209u);
    TEST_ASSERT(f.out.pub.voltSpread_mV == 0u);
    TEST_ASSERT(f.out.pub.soc_pm == 500u);
    TEST_ASSERT(f.out.pub.tempMax_dC == 227);
    TEST_ASSERT(f.out.pub.chargeVoltLimit_mV == 56000u);
    TEST_ASSERT(f.out.pub.dischargeVoltLimit_mV == 44000u);
    TEST_ASSERT(f.out.pub.onlineCnt == 1u);
    TEST_ASSERT(f.out.pub.cond == (uint8_t)cluCond_online);
    /* ...EXCEPT the two current limits, which carry the derate. */
    TEST_ASSERT(f.out.pub.chargeLimit_mA    == 240000u);    /* 300 A x 0.80  */
    TEST_ASSERT(f.out.pub.dischargeLimit_mA == 255000u);    /* 300 A x 0.85  */

    /* And after a binding measurement taken AT the limit, the loop settles at
     * loadTarget x L and the emitted value at loadTarget x derate x L.  The
     * published limit is NOT L, and a test that asserted it was could not hold
     * under a loop at all. */
    f.in[0].current_mA = 240000;                            /* == published  */
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 800u);
    TEST_ASSERT(f.out.pub.chargeLoop_mA == 270000u);        /* 0.9 x 300 A   */
    TEST_ASSERT(f.out.pub.chargeDerated_mA == 216000u);     /* x 0.80        */

    /* A single pack cannot diverge from itself. */
    TEST_ASSERT((f.out.pub.clusterAlarms & cluAlarm_socDiverge) == 0u);
    TEST_ASSERT((f.out.pub.clusterAlarms & cluAlarm_shareDiverge) == 0u);
}

static void test_zero_packs_is_a_valid_zero(void)
{
    sFix f;

    fix_init(&f);
    step(&f, 0u, 250u);
    TEST_ASSERT(f.out.pub.memberCnt == 0u);
    TEST_ASSERT(f.out.pub.onlineCnt == 0u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
    TEST_ASSERT(f.out.pub.dischargeLimit_mA == 0u);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_noMembers);
    TEST_ASSERT(f.out.pub.valid == 0u);
}

/** THE PURE-CORE PROPERTY, ASSERTED RATHER THAN CLAIMED: memcpy the state,
 *  re-run the identical input, get an identical output.  If cluster_calc.c
 *  ever grew a file static this fails. */
static void test_state_is_the_only_thing_carried(void)
{
    sFix              f;
    sClusterCalcState saved;
    sClusterResult    first;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u,  50000u,  50000u);
    settle(&f, 2u);
    f.in[0].current_mA = 60000;
    f.in[1].current_mA = 30000;
    step(&f, 2u, BIG_DT);

    saved = f.st;
    step(&f, 2u, BIG_DT);
    first = f.out;

    f.st = saved;
    f.now_ms -= BIG_DT;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT_MEM_EQ(&first, &f.out, sizeof(first));
}

static void test_solve_rejects_bad_arguments(void)
{
    sFix f;

    fix_init(&f);
    TEST_ASSERT(ClusterCalc_Solve(NULL, 1u, &f.tune, &f.st, &f.sc, 0u,
                                  &f.out) == cluErr_badArg);
    TEST_ASSERT(ClusterCalc_Solve(f.in, CLUSTER_PACK_MAX + 1u, &f.tune,
                                  &f.st, &f.sc, 0u, &f.out) == cluErr_badArg);
    TEST_ASSERT(ClusterCalc_Solve(f.in, 1u, NULL, &f.st, &f.sc, 0u,
                                  &f.out) == cluErr_badArg);
    TEST_ASSERT(ClusterCalc_Solve(f.in, 1u, &f.tune, &f.st, &f.sc, 0u,
                                  NULL) == cluErr_badArg);
}

/** No sequence of inputs inside the plausible domain may produce a limit
 *  above the arithmetic sum of the participants' own limits — the weakest
 *  property the module has, and the one a fuzz sweep can actually check. */
static void test_limit_never_exceeds_the_sum_of_limits(void)
{
    unsigned iter;
    unsigned violations = 0u;

    srand(7u);
    for (iter = 0u; iter < 4000u; iter++) {
        sFix     f;
        uint8_t  n = (uint8_t)(1 + (rand() % CLUSTER_PACK_MAX));
        uint8_t  i;
        unsigned t;

        fix_init(&f);
        for (i = 0u; i < n; i++) {
            pack_online(&f.in[i], i,
                        (uint32_t)(rand() % 400000),
                        (uint32_t)(rand() % 400000));
        }
        for (t = 0u; t < 6u; t++) {
            uint64_t sumChg = 0u;
            uint64_t sumDsg = 0u;

            for (i = 0u; i < n; i++) {
                f.in[i].current_mA = (int32_t)((rand() % 400000) - 200000);
            }
            step(&f, n, BIG_DT);
            for (i = 0u; i < n; i++) {
                if (f.out.member[i].state >= (uint8_t)cluMember_present) {
                    sumChg += f.out.member[i].chargeLimit_mA;
                    sumDsg += f.out.member[i].dischargeLimit_mA;
                }
            }
            if (((uint64_t)f.out.pub.chargeLimit_mA > sumChg) ||
                ((uint64_t)f.out.pub.dischargeLimit_mA > sumDsg)) {
                violations++;
            }
        }
    }
    TEST_ASSERT(violations == 0u);
}

/* ============================================================================ */

int main(void)
{
    printf("=== cluster_calc ===\n");

    RUN_TEST(test_struct_sizes_are_as_budgeted);
    RUN_TEST(test_pack_max_equals_pack_module_max);
    RUN_TEST(test_enum_bits_are_distinct);
    RUN_TEST(test_enum_values_are_not_renumbered);

    RUN_TEST(test_loop_converges_in_one_step_greedy);
    RUN_TEST(test_loop_converges_in_one_step_identical);
    RUN_TEST(test_loop_does_not_update_when_the_bus_is_not_binding);
    RUN_TEST(test_loop_never_exceeds_min_limit_over_share);
    RUN_TEST(test_loop_does_not_learn_while_the_slew_is_still_ramping_in);
    RUN_TEST(test_loop_still_learns_once_the_slew_has_settled);
    RUN_TEST(test_loop_load_rounds_up_so_a_small_bus_cannot_inflate_the_limit);
    RUN_TEST(test_loop_saturates_load_for_a_zero_limit_pack);
    RUN_TEST(test_loop_ignores_a_zero_loadmax);
    RUN_TEST(test_loop_load_does_not_wrap_at_an_absurd_current);
    RUN_TEST(test_abs_of_int32_min_does_not_sign_extend);
    RUN_TEST(test_loop_uses_only_the_controlled_direction);

    RUN_TEST(test_start_is_min_limit_not_max);
    RUN_TEST(test_restart_on_a_participant_limit_decrease);
    RUN_TEST(test_no_restart_on_a_participant_limit_increase);
    RUN_TEST(test_deadband_suppresses_noise_but_not_a_ramp);
    RUN_TEST(test_restart_on_member_leave_never_raises_the_limit);
    RUN_TEST(test_restart_when_a_pack_joins);
    RUN_TEST(test_restart_after_hold_expiry);
    RUN_TEST(test_charge_and_discharge_loops_are_independent);
    RUN_TEST(test_zero_limit_pack_is_out_of_the_participating_set);
    RUN_TEST(test_open_charge_switch_excludes_charge_only);
    RUN_TEST(test_all_packs_offline_publishes_zero);
    RUN_TEST(test_electrically_stale_pack_is_excluded);

    RUN_TEST(test_charge_limit_is_zero_when_charge_is_forbidden);
    RUN_TEST(test_zero_limit_does_not_imply_forbidden);
    RUN_TEST(test_overvoltage_pack_that_isolated_itself_does_not_veto);
    RUN_TEST(test_all_unknown_switches_fall_back_to_permitted);
    RUN_TEST(test_permitting_again_ramps_from_where_it_was);

    RUN_TEST(test_slew_decrease_is_immediate_increase_is_limited);
    RUN_TEST(test_slew_step_is_clamped_at_dt_max);
    RUN_TEST(test_slew_is_wrap_safe_across_the_millisecond_rollover);
    RUN_TEST(test_slew_rate_at_the_parse_bound_does_not_overflow);

    RUN_TEST(test_soc_is_charge_weighted_not_averaged);
    RUN_TEST(test_soc_does_not_overflow_at_eight_thousand_amp_hours);
    RUN_TEST(test_soc_and_soh_fields_clear_when_their_denominator_is_zero);
    RUN_TEST(test_voltage_limits_are_invalid_when_nobody_advertises_them);
    RUN_TEST(test_cvl_includes_a_pack_that_refuses_to_charge);
    RUN_TEST(test_voltage_is_the_mean_and_the_spread_is_reported);
    RUN_TEST(test_temperature_is_max_and_alarms_are_ored);
    RUN_TEST(test_confidence_is_the_worst_contributor_and_capped_while_searching);
    RUN_TEST(test_module_count_is_online_and_electrically_fresh);

    RUN_TEST(test_diverge_soc_flags_the_sodas_case_and_changes_no_limit);
    RUN_TEST(test_share_is_an_observation_that_flags_the_greedy_pack);
    RUN_TEST(test_a_circulating_pack_is_flagged_and_the_loop_holds);

    RUN_TEST(test_single_pack_is_the_pack);
    RUN_TEST(test_zero_packs_is_a_valid_zero);
    RUN_TEST(test_state_is_the_only_thing_carried);
    RUN_TEST(test_solve_rejects_bad_arguments);
    RUN_TEST(test_limit_never_exceeds_the_sum_of_limits);

    printf("%s: %d failure(s)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}

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

/* A TICK INTERVAL, AND NOTHING DEPENDS ON IT.  The rate limiter is gone
 * (design §14.8), so no value is carried and no interval is measured; this
 * exists only to advance the snapshot stamp the way the board does. */
#define BIG_DT      2000u

typedef struct {
    sClusterPackIn    in[CLUSTER_PACK_MAX];
    sClusterTune      tune;
    sClusterScratch   sc;
    sClusterResult    out;
    uint32_t          now_ms;
} sFix;

static void fix_init(sFix *f)
{
    (void)memset(f, 0, sizeof(*f));
    ClusterCfg_Defaults(&f->tune);
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
    TEST_ASSERT(ClusterCalc_Solve(f->in, n, &f->tune, &f->sc,
                                  f->now_ms, &f->out) == cluErr_ok);
}

/** ONE TICK IS SETTLED.  This used to iterate until the published value
 *  stopped moving, because a rate limiter took several ticks to reach it.  It
 *  is kept as a name rather than inlined, because "the answer is complete
 *  after exactly one tick" is a PROPERTY worth asserting: the second call must
 *  produce the same output as the first. */
static void settle(sFix *f, uint8_t n)
{
    sClusterResult first;

    step(f, n, BIG_DT);
    first = f->out;
    step(f, n, BIG_DT);
    first.pub.tick_ms = f->out.pub.tick_ms;     /* the stamp is allowed to
                                                   move; nothing else is    */
    TEST_ASSERT_MEM_EQ(&first, &f->out, sizeof(first));
}

/* ============================================================================
 * A — shapes and invariants
 * ============================================================================ */

static void test_struct_sizes_are_as_budgeted(void)
{
    /* R4.1: main SRAM is the binding constraint and these are what is spent.
     * A silent growth here is 16 bytes per pack per buffer. */
    /* 104, DOWN FROM 112: the two slewed_mA fields went with the rate
     * limiter and the causal chain is three numbers now, not four. */
    TEST_ASSERT(sizeof(sClusterOutput) == 104u);
    TEST_ASSERT(sizeof(sClusterMember) == 32u);
    TEST_ASSERT(sizeof(sClusterPackIn) == 64u);
    /* 16, DOWN FROM 96: three write-only arrays of the retired estimator
     * hierarchy went with the dead-code sweep. */
    TEST_ASSERT(sizeof(sClusterScratch) == 16u);
    TEST_ASSERT(sizeof(sClusterTune) == 24u);
    TEST_ASSERT(sizeof(sClusterCfg) == 168u);
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
        cluMemFlag_circulating, cluMemFlag_implausible,
        cluMemFlag_limitSaturated,
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
    TEST_ASSERT((int)cluLoop_predicted == 2);
    TEST_ASSERT((int)cluLimitWhy_noParticipant == 0);
    TEST_ASSERT((int)cluLimitWhy_ceiling       == 4);
    TEST_ASSERT((int)cluErr_ok    ==  0);
    TEST_ASSERT((int)cluErr_busy  == -6);
}

/* ============================================================================
 * B — the measured rule and its low-load prediction (design §3.2, rev 3)
 * ============================================================================ */

/** THE WORKED EXAMPLE, stated by the user and reproduced exactly.
 *
 *  Two equal 300 A packs drawing 100 A and 80 A.  The bus is at 180 A and the
 *  hardest-working pack is at 1/3 of its limit, so the bus could carry
 *  180 x 3 = 540 A and the module publishes 90 % of that = 486 A.
 *
 *  The assertion is 485 029 mA, not 486 000, and the 971 mA difference is the
 *  DELIBERATE conservatism of rounding load_pm UP: ceil(1000 x 100/300) = 334
 *  rather than 333.33, and load is the divisor.  Asserting the exact value
 *  rather than a band is what makes a change to that rounding visible here. */
static void test_measured_limit_reproduces_the_worked_example(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].current_mA = 100000;
    f.in[1].current_mA =  80000;
    settle(&f, 2u);

    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 334u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA  == 485029u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA  <  486000u);
    TEST_ASSERT(f.out.pub.chargeLoopState  == (uint8_t)cluLoop_measured);
    TEST_ASSERT(f.out.pub.chargeWhy        == (uint8_t)cluLimitWhy_measured);
    TEST_ASSERT(f.out.pub.chargeBindingIdx == 0u);      /* the 100 A pack   */
}

/** THE BINDING PACK IS THE HARDEST-WORKING ONE, NOT THE BIGGEST CURRENT, and
 *  with unequal packs those are different packs.
 *
 *  L = (300, 100) A, I = (100, 60) A.  Pack 0 draws more current but sits at
 *  33 % of its limit; pack 1 is at 60 % and is what the bus can be scaled
 *  against: 160 x (100/60) = 266.7 A, x 0.9 = 240 A.  Scaling from pack 0
 *  instead would give 432 A and put 162 A through a 100 A pack. */
static void test_measured_limit_binds_on_fractional_load_not_current(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].current_mA = 100000;
    f.in[1].current_mA =  60000;
    settle(&f, 2u);

    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 600u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA  == 240000u);
    TEST_ASSERT(f.out.pub.chargeBindingIdx == 1u);
    TEST_ASSERT(f.out.member[1].flags & cluMemFlag_bindingCharge);
    TEST_ASSERT((f.out.member[0].flags & cluMemFlag_bindingCharge) == 0u);
}

/** SCALE INVARIANCE IS THE WHOLE REASON THE GATE COULD GO.  The same shares
 *  at a tenth of the current give the same answer, so the rule needs no bus
 *  at the limit and no settled publish to measure against.  Currents chosen
 *  to divide exactly, so the assertion is equality rather than a band. */
static void test_measured_limit_is_scale_invariant(void)
{
    sFix     f;
    uint32_t big;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].current_mA = 150000;
    f.in[1].current_mA = 120000;
    settle(&f, 2u);
    big = f.out.pub.chargeTarget_mA;
    TEST_ASSERT(big == 486000u);

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    /* A FIFTH of the current, still above the floor (loadMax lands exactly
     * on 100), and the answer is bit-for-bit the same. */
    f.in[0].current_mA = 30000;
    f.in[1].current_mA = 24000;
    settle(&f, 2u);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 100u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA == big);
}

/** THE §13.2 RAMP-IN TRAP, WHICH CANNOT HAPPEN ANY MORE — the board-1
 *  scenario that cost a 36x throughput loss on Pd1.1.50, replayed.
 *
 *  A 150 A pack, a steady 1.007 A charge, the slew walking up from zero.
 *  Revision 2 measured the bus against its own emitted value, so the ramp
 *  crossing the load satisfied the gate and ratcheted the loop down to 5.1 A,
 *  where it LATCHED.  Revision 3 never looks at what it published: 1.007 A on
 *  a 150 A pack is a load of 7 pm, far below the floor, so it predicts
 *  0.9 x 150 A and the slew simply walks up to it. */
static void test_a_ramping_slew_can_no_longer_latch_the_limit_low(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 150000u, 150000u);
    f.in[0].current_mA = 1007;
    settle(&f, 1u);

    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_predicted);
    TEST_ASSERT(f.out.pub.chargeTarget_mA == 135000u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA  == 135000u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA  >  100000u);  /* not 4 112 mA     */
}

/** BELOW THE FLOOR, PREDICT.  Two 300 A packs at ~1 A: a load of 4 pm is
 *  noise, so the geometric sum runs — 0.9 x 300 + 0.81 x 300 = 513 A, which
 *  is conservative against the 600 A the packs add up to. */
static void test_below_the_floor_the_prediction_runs(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].current_mA = 1000;
    f.in[1].current_mA =  800;
    settle(&f, 2u);

    TEST_ASSERT(f.out.pub.chargeLoadMax_pm < f.tune.lowLoadFloor_pm);
    TEST_ASSERT(f.out.pub.chargeLoopState  == (uint8_t)cluLoop_predicted);
    TEST_ASSERT(f.out.pub.chargeWhy        == (uint8_t)cluLimitWhy_predicted);
    TEST_ASSERT(f.out.pub.chargeTarget_mA  == 513000u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA  <  600000u);
    /* NOBODY IS BINDING A PREDICTION, and saying so is what stops an operator
     * reading the last measurement's pack as the current one. */
    TEST_ASSERT(f.out.pub.chargeBindingIdx == CLUSTER_PACK_NONE);
}

/** SMALLEST FIRST, and the ordering is load-bearing.  L = (100, 300) A gives
 *  0.9 x 100 + 0.81 x 300 = 333 A.  Sorted the other way it would be
 *  0.9 x 300 + 0.81 x 100 = 351 A — higher, i.e. the unsafe direction, which
 *  is exactly why the sort exists. */
static void test_the_prediction_weights_the_weakest_pack_most(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    settle(&f, 2u);

    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_predicted);
    TEST_ASSERT(f.out.pub.chargeTarget_mA == 333000u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA != 351000u);
}

/** A PREDICTION NEVER REACHES sum(L_i) EITHER, at any member count.  With
 *  eight identical packs the geometric series is bounded by d/(1-d) x L,
 *  which at d = 0.9 is 9 L against the 8 L the packs actually add up to — so
 *  the bound is NOT free from the series alone at eight members, and the test
 *  is a real one rather than an algebraic tautology. */
static void test_the_prediction_never_exceeds_the_sum_of_limits(void)
{
    uint8_t n;

    for (n = 1u; n <= CLUSTER_PACK_MAX; n++) {
        sFix     f;
        uint8_t  i;
        uint64_t sum = 0u;

        fix_init(&f);
        fix_no_derate(&f);
        for (i = 0u; i < n; i++) {
            pack_online(&f.in[i], i, 300000u, 300000u);
            sum += 300000u;
        }
        settle(&f, n);
        TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_predicted);
        TEST_ASSERT((uint64_t)f.out.pub.chargeTarget_mA <= sum);
    }
}

/** THE ACCEPTED LIMITATION, made explicit so it cannot regress by accident.
 *  A one-pack cluster publishes margin x L — 135 A of a 150 A pack — and it
 *  does so IDENTICALLY on both sides of the floor, because |S| / (|S|/L) is
 *  L at every current.  A single-pack site therefore never steps when the
 *  load arrives, and the 10 % it gives up is the price of the module. */
static void test_a_single_pack_is_continuous_across_the_floor(void)
{
    sFix     f;
    uint32_t quiet;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 150000u, 150000u);
    settle(&f, 1u);
    quiet = f.out.pub.chargeTarget_mA;
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_predicted);
    TEST_ASSERT(quiet == 135000u);

    f.in[0].current_mA = 100000;                /* well above the floor      */
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_measured);
    /* Equal to within the ceiling on load_pm — 667 rather than 666.67 — which
     * is 68 mA of 135 A, and always in the conservative direction. */
    TEST_ASSERT(f.out.pub.chargeTarget_mA <= quiet);
    TEST_ASSERT((quiet - f.out.pub.chargeTarget_mA) < 200u);
}

/** THE SAFETY MARGIN IS A NUMBER, not a constant folded into the arithmetic.
 *  1000 is a legal setting and means "no margin"; the sum bound still holds
 *  there, which is why the parser permits it. */
static void test_the_safety_margin_is_configurable_end_to_end(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.safetyMargin_pm = 1000u;
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].current_mA = 150000;
    f.in[1].current_mA = 120000;
    settle(&f, 2u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA == 540000u);  /* 486 000 / 0.9    */

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.safetyMargin_pm = 500u;
    pack_online(&f.in[0], 0u, 300000u, 300000u);
    pack_online(&f.in[1], 1u, 300000u, 300000u);
    f.in[0].current_mA = 150000;
    f.in[1].current_mA = 120000;
    settle(&f, 2u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA == 270000u);
}

/** LOAD IS ROUNDED UP, AND THAT IS THE SAFE DIRECTION.  A 10 mA bus against a
 *  4.779 A pack has a true load of 2.09 pm; truncating to 2 would licence
 *  4.5 A, ceiling to 3 gives 3.0 A.  Measured with the floor lowered, since
 *  the shipped floor sends this case to the prediction instead. */
static void test_load_rounds_up_so_a_small_bus_cannot_inflate_the_limit(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.lowLoadFloor_pm = 0u;                /* force the measured path   */
    pack_online(&f.in[0], 0u, 4779u, 4779u);
    f.in[0].current_mA = 10;
    settle(&f, 1u);

    /* ceil(10 * 1000 / 4779) = 3, so 10 * 900 / 3 = 3000 mA.  Truncation
     * would have given 2 and 4500 mA — nearly the pack's whole limit off one
     * 10 mA reading. */
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 3u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA  == 3000u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA  <= 4779u);
}

/** A PACK ADVERTISING A ZERO LIMIT WHILE CURRENT FLOWS IS OVER ITS LIMIT BY
 *  DEFINITION.  It is excluded from the participating set but NOT from the
 *  measurement, and its load saturates at 1000 rather than dividing. */
static void test_measurement_saturates_load_for_a_zero_limit_pack(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u,      0u, 100000u);
    f.in[0].current_mA = 50000;
    f.in[1].current_mA = 50000;
    settle(&f, 2u);

    TEST_ASSERT(f.out.member[1].flags & cluMemFlag_limitSaturated);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 1000u);
    /* 100 A of bus at a load of 1.0 is 100 A of capability, x 0.9. */
    TEST_ASSERT(f.out.pub.chargeTarget_mA == 90000u);
}

/** `|I| * 1000` OVERFLOWS uint32 above 4 294 967 mA, and a wrap there
 *  produces a SMALL loadMax — which is a LARGE limit, since loadMax is the
 *  divisor.  The intermediate is uint64 for exactly this. */
static void test_load_does_not_wrap_at_an_absurd_current(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    f.tune.lowLoadFloor_pm = 0u;
    pack_online(&f.in[0], 0u, 1000u, 1000u);
    f.in[0].current_mA = 900000000;             /* beyond plausible          */
    settle(&f, 1u);

    /* Dropped by the plausibility pass before any of this, which is where a
     * garbage reading belongs. */
    TEST_ASSERT(f.out.member[0].flags & cluMemFlag_implausible);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
}

static void test_abs_of_int32_min_does_not_sign_extend(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.in[0].current_mA = INT32_MIN;
    step(&f, 1u, 250u);

    /* -(int64_t)v, never -v: negating INT32_MIN in int32 is undefined and
     * (uint64_t)(-v) sign-extends first.  Either way it is implausible. */
    TEST_ASSERT(f.out.member[0].flags & cluMemFlag_implausible);
}

/** ONLY THE PACKS FLOWING THIS WAY.  A discharging pack must not appear in
 *  the charge measurement, or the two directions would contaminate each
 *  other's divisor. */
static void test_measurement_uses_only_the_controlled_direction(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].current_mA =  50000;                /* charging                  */
    f.in[1].current_mA = -50000;                /* discharging               */
    settle(&f, 2u);

    /* Each direction sees one pack at half its limit: 50 A / 0.5 x 0.9. */
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm    == 500u);
    TEST_ASSERT(f.out.pub.dischargeLoadMax_pm == 500u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA     == 90000u);
    TEST_ASSERT(f.out.pub.dischargeTarget_mA  == 90000u);
}

/** THE TWO DIRECTIONS SHARE NO ARITHMETIC.  Charge and discharge sharing
 *  differ, so each is solved on its own currents and its own limits. */
static void test_charge_and_discharge_are_independent(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 200000u);
    settle(&f, 1u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA    ==  90000u);
    TEST_ASSERT(f.out.pub.dischargeTarget_mA == 180000u);

    f.in[0].current_mA = -190000;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeTarget_mA    ==  90000u);   /* untouched    */
    TEST_ASSERT(f.out.pub.dischargeTarget_mA == 180000u);
}

/* ============================================================================
 * C — the participating set
 * ============================================================================ */

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
        const uint32_t learned = f.out.pub.chargeTarget_mA;

        TEST_ASSERT(f.out.pub.chargeLimit_mA > 0u);

        /* A pack at cell over-voltage that has NOT isolated itself. */
        f.in[1].alarms = (uint32_t)packAlarm_cellOverVoltage;
        step(&f, 2u, BIG_DT);

        TEST_ASSERT(f.out.pub.chargeAllowed == 0u);
        TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
        TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_forbidden);
        TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_chargeForbidden);
        /* But the RULE is untouched, and chargeDerated_mA still shows what
         * would have gone out — which is what tells "not allowed" from
         * "nothing to give". */
        TEST_ASSERT(f.out.pub.chargeTarget_mA == learned);
        TEST_ASSERT(f.out.pub.chargeDerated_mA > 0u);
        /* Discharge is untouched — permission is per direction. */
        TEST_ASSERT(f.out.pub.dischargeAllowed == 1u);
        TEST_ASSERT(f.out.pub.dischargeLimit_mA > 0u);
    }
}

/** THE CONVERSE DOES NOT HOLD.  A tiny pack publishes a SMALL limit, not a
 *  refusal — permission and magnitude are different statements.  This used to
 *  be demonstrated with a slew starting at zero; with the rate limiter gone
 *  the honest way to make a small limit is a small battery. */
static void test_zero_limit_does_not_imply_forbidden(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100u, 100u);      /* a 0.1 A pack              */
    step(&f, 1u, 250u);

    TEST_ASSERT(f.out.pub.chargeLimit_mA < 100u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA > 0u);
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

/** A REFUSAL AND ITS WITHDRAWAL BOTH TAKE EFFECT IMMEDIATELY, and that is the
 *  point rather than an omission (design §14.8).  The cap is a cap: the
 *  battery being emulated switches `0x351`'s current fields between discrete
 *  states between 250 ms frames, and a sudden change of situation needs a
 *  sudden response.  Revision 2 ramped back up here, which required an
 *  asymmetry (fall fast, rise slow) that existed only to stop the rate limiter
 *  causing the cascade it was meant to prevent.  With no rate limiter there is
 *  no asymmetry to get wrong. */
static void test_permission_and_its_withdrawal_both_take_effect_at_once(void)
{
    sFix     f;
    uint32_t before;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    settle(&f, 1u);
    before = f.out.pub.chargeLimit_mA;
    TEST_ASSERT(before > 0u);

    f.in[0].alarms = (uint32_t)packAlarm_cellOverVoltage;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeAllowed == 0u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == 0u);
    TEST_ASSERT(f.out.pub.chargeWhy == (uint8_t)cluLimitWhy_forbidden);

    f.in[0].alarms = 0u;
    step(&f, 1u, 250u);
    TEST_ASSERT(f.out.pub.chargeAllowed == 1u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA == before);    /* the FULL cap, now */
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

/** THE SODAS DEFECT, 2026-09-09 — found by running the two-pack case for the
 *  first time and fixed the same day.
 *
 *  `sodas2` advertised packCap_voltageLimits and returned 0 mV / 0 mV for four
 *  minutes (its limits register group had not answered) while `sodas15`
 *  returned 55 200 / 43 200.  The min took the zero, and the snapshot went out
 *  with `chargeVoltLimit_mV: 0` and the field marked VALID — which by contract
 *  2 of cluster.h is not "no limit" but an instruction to STOP CHARGING.  It
 *  drove nothing only because no frame source exists yet. */
static void test_a_pack_reporting_zero_volt_limits_does_not_capture_the_min(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].caps |= (uint32_t)packCap_voltageLimits;
    f.in[1].caps |= (uint32_t)packCap_voltageLimits;
    f.in[0].chargeVoltLimit_mV    = 0u;         /* advertises, has not
                                                   answered yet             */
    f.in[0].dischargeVoltLimit_mV = 0u;
    f.in[1].chargeVoltLimit_mV    = 55200u;
    f.in[1].dischargeVoltLimit_mV = 43200u;
    step(&f, 2u, 250u);

    TEST_ASSERT(f.out.pub.chargeVoltLimit_mV    == 55200u);
    TEST_ASSERT(f.out.pub.dischargeVoltLimit_mV == 43200u);
    TEST_ASSERT(f.out.pub.fields & (uint16_t)cluField_chargeVoltLimit);
    TEST_ASSERT(f.out.pub.fields & (uint16_t)cluField_dischargeVoltLimit);
    /* Nobody is missing: one pack answered both fields. */
    TEST_ASSERT((f.out.pub.clusterAlarms &
                 (uint32_t)cluAlarm_voltLimitMissing) == 0u);
}

/** AND IF NOBODY ANSWERS, THE FIELD IS CLEARED RATHER THAN PUBLISHED AS ZERO —
 *  the §7.4 rule, now reachable through a pack that advertises as well as
 *  through one that does not. */
static void test_all_packs_reporting_zero_volt_limits_clears_the_field(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.in[0].caps |= (uint32_t)packCap_voltageLimits;
    f.in[0].chargeVoltLimit_mV    = 0u;
    f.in[0].dischargeVoltLimit_mV = 0u;
    step(&f, 1u, 250u);

    TEST_ASSERT(f.out.pub.chargeVoltLimit_mV == 0u);
    TEST_ASSERT((f.out.pub.fields & (uint16_t)cluField_chargeVoltLimit) == 0u);
    TEST_ASSERT((f.out.pub.fields &
                 (uint16_t)cluField_dischargeVoltLimit) == 0u);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_voltLimitMissing);
}

/** THE TWO FIELDS FAIL INDEPENDENTLY.  A pack may answer one register group
 *  and not the other, and the healthy field must not be held hostage. */
static void test_volt_limits_are_published_per_field(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    f.in[0].caps |= (uint32_t)packCap_voltageLimits;
    f.in[0].chargeVoltLimit_mV    = 56000u;
    f.in[0].dischargeVoltLimit_mV = 0u;         /* this half is unanswered   */
    step(&f, 1u, 250u);

    TEST_ASSERT(f.out.pub.chargeVoltLimit_mV == 56000u);
    TEST_ASSERT(f.out.pub.fields & (uint16_t)cluField_chargeVoltLimit);
    TEST_ASSERT((f.out.pub.fields &
                 (uint16_t)cluField_dischargeVoltLimit) == 0u);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_voltLimitMissing);
}

/** A DECODE ERROR YIELDS A HUGE NUMBER, and DVL is a max(), so the implausible
 *  domain has to bound it from above as well as at zero. */
static void test_an_implausible_volt_limit_is_not_taken(void)
{
    sFix f;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    f.in[0].caps |= (uint32_t)packCap_voltageLimits;
    f.in[1].caps |= (uint32_t)packCap_voltageLimits;
    f.in[0].chargeVoltLimit_mV    = 56000u;
    f.in[0].dischargeVoltLimit_mV = 43200u;
    f.in[1].chargeVoltLimit_mV    = 56000u;
    f.in[1].dischargeVoltLimit_mV = 0xFFFF0000u;    /* a wrong decodeType    */
    step(&f, 2u, 250u);

    TEST_ASSERT(f.out.pub.dischargeVoltLimit_mV == 43200u);
    TEST_ASSERT(f.out.pub.fields & (uint16_t)cluField_dischargeVoltLimit);
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

static void test_confidence_is_the_worst_contributor_and_capped_while_predicting(void)
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

    /* A cluster that has not measured its own capability should say so — and
     * with TWO packs the prediction genuinely is a guess, so the cap applies.
     * (A one-pack cluster is exempt: there both rules give the same answer.) */
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_predicted);
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

static void test_a_circulating_pack_is_flagged_and_each_direction_splits(void)
{
    sFix f;

    fix_init(&f);
    fix_no_derate(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u, 100000u, 100000u);
    settle(&f, 2u);

    /* Packs six millivolts apart at low bus current: one reads the opposite
     * sign outright.  A circulating bus is SPLIT — each direction sees only
     * its own pack — so the charge answer is that ONE pack's capability
     * (40 A against 100 A is a load of 400 pm, above the floor, so it
     * measures): 40 000 x 900 / 400 = 90 000. */
    f.in[0].current_mA =  40000;
    f.in[1].current_mA = -38000;
    step(&f, 2u, BIG_DT);
    TEST_ASSERT(f.out.pub.clusterAlarms & cluAlarm_circulating);
    TEST_ASSERT(f.out.pub.chargeTarget_mA == 90000u);
}

/* ============================================================================
 * H — degenerate paths and purity
 * ============================================================================ */

/** Open question 3, made enforceable.  n = 1 is the SAME code path — there is
 *  no `if (n == 1)` in cluster_calc.c — and the published limit is NOT L:
 *  loadMax = 1000 |I|/L, so the target is margin x L whatever the current,
 *  and the emitted value is margin x derate x L. */
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
    /* ...EXCEPT the two current limits, which carry BOTH the safety margin
     * and the derate: 300 A x 0.9 x 0.80 and x 0.9 x 0.85. */
    TEST_ASSERT(f.out.pub.chargeTarget_mA   == 270000u);
    TEST_ASSERT(f.out.pub.chargeLimit_mA    == 216000u);
    TEST_ASSERT(f.out.pub.dischargeLimit_mA == 229500u);

    /* And it is the SAME answer once current flows — one pack is the same
     * number whichever rule produced it. */
    f.in[0].current_mA = 216000;
    step(&f, 1u, BIG_DT);
    TEST_ASSERT(f.out.pub.chargeLoopState == (uint8_t)cluLoop_measured);
    TEST_ASSERT(f.out.pub.chargeLoadMax_pm == 720u);
    TEST_ASSERT(f.out.pub.chargeTarget_mA <= 270000u);
    TEST_ASSERT((270000u - f.out.pub.chargeTarget_mA) < 500u);

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
/** NOTHING IS CARRIED AT ALL — the strongest form this assertion has ever
 *  taken.  It used to save and restore sClusterCalcState to prove the state
 *  was the ONLY carrier; with the rate limiter deleted there is no state, so
 *  two independent fixtures fed the same packs must agree bit for bit even
 *  though one has a long history behind it and the other has none. */
static void test_nothing_at_all_is_carried_between_ticks(void)
{
    sFix f;
    sFix fresh;

    fix_init(&f);
    pack_online(&f.in[0], 0u, 100000u, 100000u);
    pack_online(&f.in[1], 1u,  50000u,  50000u);
    settle(&f, 2u);
    f.in[0].current_mA = 60000;                 /* a long, varied history    */
    f.in[1].current_mA = 30000;
    step(&f, 2u, BIG_DT);
    f.in[0].current_mA = -5000;
    step(&f, 2u, BIG_DT);
    f.in[0].current_mA = 60000;
    step(&f, 2u, BIG_DT);

    /* A fixture with NO history, given exactly the inputs of that last tick. */
    fix_init(&fresh);
    (void)memcpy(fresh.in, f.in, sizeof(fresh.in));
    step(&fresh, 2u, BIG_DT);

    fresh.out.pub.tick_ms = f.out.pub.tick_ms;  /* the stamp is the argument */
    TEST_ASSERT_MEM_EQ(&f.out, &fresh.out, sizeof(f.out));
}

static void test_solve_rejects_bad_arguments(void)
{
    sFix f;

    fix_init(&f);
    TEST_ASSERT(ClusterCalc_Solve(NULL, 1u, &f.tune, &f.sc, 0u,
                                  &f.out) == cluErr_badArg);
    TEST_ASSERT(ClusterCalc_Solve(f.in, CLUSTER_PACK_MAX + 1u, &f.tune,
                                  &f.sc, 0u, &f.out) == cluErr_badArg);
    TEST_ASSERT(ClusterCalc_Solve(f.in, 1u, NULL, &f.sc, 0u,
                                  &f.out) == cluErr_badArg);
    TEST_ASSERT(ClusterCalc_Solve(f.in, 1u, &f.tune, &f.sc, 0u,
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

    RUN_TEST(test_measured_limit_reproduces_the_worked_example);
    RUN_TEST(test_measured_limit_binds_on_fractional_load_not_current);
    RUN_TEST(test_measured_limit_is_scale_invariant);
    RUN_TEST(test_a_ramping_slew_can_no_longer_latch_the_limit_low);
    RUN_TEST(test_below_the_floor_the_prediction_runs);
    RUN_TEST(test_the_prediction_weights_the_weakest_pack_most);
    RUN_TEST(test_the_prediction_never_exceeds_the_sum_of_limits);
    RUN_TEST(test_a_single_pack_is_continuous_across_the_floor);
    RUN_TEST(test_the_safety_margin_is_configurable_end_to_end);
    RUN_TEST(test_load_rounds_up_so_a_small_bus_cannot_inflate_the_limit);
    RUN_TEST(test_measurement_saturates_load_for_a_zero_limit_pack);
    RUN_TEST(test_load_does_not_wrap_at_an_absurd_current);
    RUN_TEST(test_abs_of_int32_min_does_not_sign_extend);
    RUN_TEST(test_measurement_uses_only_the_controlled_direction);
    RUN_TEST(test_charge_and_discharge_are_independent);

    RUN_TEST(test_zero_limit_pack_is_out_of_the_participating_set);
    RUN_TEST(test_open_charge_switch_excludes_charge_only);
    RUN_TEST(test_all_packs_offline_publishes_zero);
    RUN_TEST(test_electrically_stale_pack_is_excluded);

    RUN_TEST(test_charge_limit_is_zero_when_charge_is_forbidden);
    RUN_TEST(test_zero_limit_does_not_imply_forbidden);
    RUN_TEST(test_overvoltage_pack_that_isolated_itself_does_not_veto);
    RUN_TEST(test_all_unknown_switches_fall_back_to_permitted);
    RUN_TEST(test_permission_and_its_withdrawal_both_take_effect_at_once);


    RUN_TEST(test_soc_is_charge_weighted_not_averaged);
    RUN_TEST(test_soc_does_not_overflow_at_eight_thousand_amp_hours);
    RUN_TEST(test_soc_and_soh_fields_clear_when_their_denominator_is_zero);
    RUN_TEST(test_voltage_limits_are_invalid_when_nobody_advertises_them);
    RUN_TEST(test_cvl_includes_a_pack_that_refuses_to_charge);
    RUN_TEST(test_a_pack_reporting_zero_volt_limits_does_not_capture_the_min);
    RUN_TEST(test_all_packs_reporting_zero_volt_limits_clears_the_field);
    RUN_TEST(test_volt_limits_are_published_per_field);
    RUN_TEST(test_an_implausible_volt_limit_is_not_taken);
    RUN_TEST(test_voltage_is_the_mean_and_the_spread_is_reported);
    RUN_TEST(test_temperature_is_max_and_alarms_are_ored);
    RUN_TEST(test_confidence_is_the_worst_contributor_and_capped_while_predicting);
    RUN_TEST(test_module_count_is_online_and_electrically_fresh);

    RUN_TEST(test_diverge_soc_flags_the_sodas_case_and_changes_no_limit);
    RUN_TEST(test_share_is_an_observation_that_flags_the_greedy_pack);
    RUN_TEST(test_a_circulating_pack_is_flagged_and_each_direction_splits);

    RUN_TEST(test_single_pack_is_the_pack);
    RUN_TEST(test_zero_packs_is_a_valid_zero);
    RUN_TEST(test_nothing_at_all_is_carried_between_ticks);
    RUN_TEST(test_solve_rejects_bad_arguments);
    RUN_TEST(test_limit_never_exceeds_the_sum_of_limits);

    printf("%s: %d failure(s)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}

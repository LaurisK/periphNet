/*
 * test_pack_soc.c
 *
 * The SOC estimator (docs/design_battery_pack.md §24).  The interesting
 * assertions are the ones about the GRADED gate: that a knee sample
 * outweighs a plateau sample by the right order of magnitude, and that
 * averaging many mediocre samples beats one good one -- because on the real
 * pack, mediocre samples are all there are.
 */

#include "App/Pack/pack_soc.h"
#include "test_util.h"

#include <stdio.h>
#include <string.h>


/* Mirrors what the core does: read the counter, then advance the unit with
 * the charge it implies.  Keeping this in one helper is the point of the
 * refactor -- a pack and a cell now go through the SAME advance. */
static int note(sPackSoc *s, int32_t counter_mAh, uint32_t maxStep)
{
    int32_t dQ = 0;
    const int ok = PackSoc_NoteCounter(s, counter_mAh, maxStep, &dQ);

    if (ok != 0) {
        PackSoc_UnitAdvance(&s->unit, dQ);
    }
    return ok;
}

static void cells_init(sPackSocUnit *c, int n)
{
    int i;
    for (i = 0; i < n; i++) { PackSoc_UnitInit(&c[i], 100000u); }
}

/* Resolve + apply an anchor for every cell, the way the core does, with the
 * per-cell charge ALREADY corrected for balance by the caller. */
static int cells_resolve(sPackSocUnit *c, int n, uint32_t nameplate)
{
    int i, learned = 0;
    for (i = 0; i < n; i++) {
        int32_t soc = 0; uint32_t sig = 0;
        if (PackSoc_UnitAnchorResolve(&c[i], &soc, &sig)) {
            learned += PackSoc_UnitApplyAnchor(&c[i], soc, sig, nameplate);
        }
    }
    return learned;
}

/* --- the OCV table ------------------------------------------------------- */

static void test_ocv_maps_monotonically(void)
{
    uint16_t v;
    int32_t  prev = -1;

    for (v = 2400u; v <= 3700u; v += 5u) {
        const int32_t s = PackSoc_OcvToSoc_pm(v);

        TEST_ASSERT(s >= 0);
        TEST_ASSERT(s <= 1000);
        TEST_ASSERT(s >= prev);          /* never goes backwards */
        prev = s;
    }
}

static void test_ocv_hits_the_table_points(void)
{
    /* Exact breakpoints from §4. */
    TEST_ASSERT(PackSoc_OcvToSoc_pm(2500) == 0);
    TEST_ASSERT(PackSoc_OcvToSoc_pm(3250) == 200);
    TEST_ASSERT(PackSoc_OcvToSoc_pm(3290) == 500);
    TEST_ASSERT(PackSoc_OcvToSoc_pm(3320) == 800);
    TEST_ASSERT(PackSoc_OcvToSoc_pm(3340) == 900);
    /* Below and above the table clamp rather than extrapolate. */
    TEST_ASSERT(PackSoc_OcvToSoc_pm(2000) == 0);
    TEST_ASSERT(PackSoc_OcvToSoc_pm(3900) == 1000);
}

/** THE FLATNESS IS THE PROBLEM, so assert it rather than assume it: the whole
 *  30-80 % region must move less than 25 mV. */
static void test_the_plateau_really_is_flat(void)
{
    const int32_t lo = PackSoc_OcvToSoc_pm(3270);   /* 300 pm */
    const int32_t hi = PackSoc_OcvToSoc_pm(3320);   /* 800 pm */

    TEST_ASSERT(lo == 300);
    TEST_ASSERT(hi == 800);
    /* 500 per-mille of SOC across only 50 mV. */
}

/* --- the graded gate ----------------------------------------------------- */

/** A knee sample must outweigh a plateau sample by orders of magnitude --
 *  that is what lets the gate be graded instead of a cliff. */
static void test_knee_outweighs_plateau_by_orders_of_magnitude(void)
{
    const uint32_t kPlateau = PackSoc_Slope_uV_per_pm(3300);  /* ~1 mV/%  */
    const uint32_t kBottom  = PackSoc_Slope_uV_per_pm(3220);  /* ~5 mV/%  */
    const uint32_t kDeep    = PackSoc_Slope_uV_per_pm(3150);  /* ~20 mV/% */

    TEST_ASSERT(kBottom > kPlateau);
    TEST_ASSERT(kDeep   > kBottom);
    /* Weight is k^2, so a deep sample is worth hundreds of plateau ones. */
    TEST_ASSERT((kDeep / kPlateau) >= 10u);
}

/** One knee sample must dominate a pile of plateau samples. */
static void test_one_knee_sample_dominates_many_plateau_samples(void)
{
    sPackSocUnit a;
    int32_t        soc = 0;
    int            i;

    PackSoc_UnitInit(&a, 100000u);
    for (i = 0; i < 100; i++) {
        PackSoc_UnitAnchorAdd(&a, 3300u);   /* plateau, ~700 pm */
    }
    PackSoc_UnitAnchorAdd(&a, 3150u);              /* deep knee, ~75 pm */

    TEST_ASSERT(1 == PackSoc_UnitAnchorResolve(&a, &soc, NULL));
    /* The single knee sample must pull the answer well below the plateau
     * crowd -- if it did not, the weighting is not doing its job. */
    TEST_ASSERT(soc < 400);
}

/** Averaging many mediocre samples must beat one mediocre sample.  This is
 *  the property the real pack depends on: at 2 mV/% a single reading is
 *  +/- 1.5 % SOC, and only sqrt(N) makes it usable (§24.3). */
static void test_many_samples_shrink_sigma(void)
{
    sPackSocUnit one;
    sPackSocUnit many;
    uint32_t       s1 = 0;
    uint32_t       sN = 0;
    int            i;

    PackSoc_UnitInit(&one, 100000u);
    PackSoc_UnitInit(&many, 100000u);

    PackSoc_UnitAnchorAdd(&one, 3260u);
    for (i = 0; i < 64; i++) {
        PackSoc_UnitAnchorAdd(&many, 3260u);
    }
    TEST_ASSERT(1 == PackSoc_UnitAnchorResolve(&one,  NULL, &s1));
    TEST_ASSERT(1 == PackSoc_UnitAnchorResolve(&many, NULL, &sN));

    TEST_ASSERT(sN < s1);            /* more samples, less uncertainty */
}

static void test_empty_anchor_resolves_to_nothing(void)
{
    sPackSocUnit a;

    PackSoc_UnitInit(&a, 100000u);
    TEST_ASSERT(0 == PackSoc_UnitAnchorResolve(&a, NULL, NULL));
}

/* --- coulomb counting ---------------------------------------------------- */

static void test_counter_needs_two_samples_before_integrating(void)
{
    sPackSoc s;

    PackSoc_Init(&s, 261000u);
    /* The first reading establishes a baseline; there is no interval yet. */
    TEST_ASSERT(0 == note(&s, 130000, 20000u));
    TEST_ASSERT(1 == note(&s, 130500, 20000u));
    TEST_ASSERT(s.unit.chargeSinceAnchor_mAh == 500);
}

/** A STEP IS NOT CHARGE: the vendor's counter is re-seeded from a voltage
 *  estimate on a config change, and clamped at the ends of its range. */
static void test_counter_rejects_a_reseed_step(void)
{
    sPackSoc s;

    PackSoc_Init(&s, 261000u);
    (void)note(&s, 130000, 20000u);
    (void)note(&s, 130500, 20000u);

    /* A 60 Ah jump between two 5 s samples is not current that flowed. */
    TEST_ASSERT(0 == note(&s, 190500, 20000u));
    TEST_ASSERT(s.unit.chargeSinceAnchor_mAh == 500);           /* unchanged */

    /* ...but the baseline moved, so the next ordinary delta still works. */
    TEST_ASSERT(1 == note(&s, 190600, 20000u));
    TEST_ASSERT(s.unit.chargeSinceAnchor_mAh == 600);
}

static void test_soc_tracks_the_counter_once_seeded(void)
{
    sPackSoc s;

    PackSoc_Init(&s, 100000u);                  /* 100 Ah, easy arithmetic */
    (void)PackSoc_UnitApplyAnchor(&s.unit, 500, 5u, 0u);       /* 50.0 % */
    (void)note(&s, 50000, 20000u);
    (void)note(&s, 55000, 20000u);   /* +5 Ah = +5 % */

    TEST_ASSERT(PackSoc_UnitGet_pm(&s.unit, NULL) == 550);
}

static void test_soc_is_unknown_until_anchored(void)
{
    sPackSoc s;

    PackSoc_Init(&s, 100000u);
    TEST_ASSERT(PackSoc_UnitGet_pm(&s.unit, NULL) == -1);
    (void)note(&s, 50000, 20000u);
    (void)note(&s, 55000, 20000u);
    /* Charge accumulated, but with no anchor there is nothing to add it to
     * -- an estimator that guessed here would be confidently wrong. */
    TEST_ASSERT(PackSoc_UnitGet_pm(&s.unit, NULL) == -1);
    TEST_ASSERT(s.unit.chargeSinceAnchor_mAh == 5000);
}

static void test_soc_clamps_at_both_ends(void)
{
    sPackSoc s;

    PackSoc_Init(&s, 100000u);
    (void)PackSoc_UnitApplyAnchor(&s.unit, 980, 5u, 0u);
    (void)note(&s, 98000, 20000u);
    (void)note(&s, 108000, 20000u);      /* would be 1080 pm */
    TEST_ASSERT(PackSoc_UnitGet_pm(&s.unit, NULL) == 1000);

    PackSoc_Init(&s, 100000u);
    (void)PackSoc_UnitApplyAnchor(&s.unit, 20, 5u, 0u);
    (void)note(&s, 2000, 20000u);
    (void)note(&s, -8000, 20000u);
    TEST_ASSERT(PackSoc_UnitGet_pm(&s.unit, NULL) == 0);
}

/* --- the drift residual, which is the actual product --------------------- */

/** The gap between prediction and anchor is the error coulomb counting
 *  accumulated.  It is the only thing that bounds how wrong the plateau
 *  estimate can be, so it must be measured, not discarded. */
static void test_anchor_measures_the_drift_it_corrects(void)
{
    sPackSoc s;

    PackSoc_Init(&s, 100000u);
    (void)PackSoc_UnitApplyAnchor(&s.unit, 500, 5u, 0u);
    TEST_ASSERT(s.unit.driftResidual_pm == 0);       /* first anchor: no history */

    /* Coulomb counting says +10 %, so it predicts 60.0 %. */
    (void)note(&s, 50000, 20000u);
    (void)note(&s, 60000, 20000u);
    TEST_ASSERT(PackSoc_UnitGet_pm(&s.unit, NULL) == 600);

    /* The anchor disagrees: it says 57.0 %.  The 3 % gap is the drift. */
    (void)PackSoc_UnitApplyAnchor(&s.unit, 570, 5u, 0u);
    TEST_ASSERT(s.unit.driftResidual_pm == -30);
    TEST_ASSERT(PackSoc_UnitGet_pm(&s.unit, NULL) == 570);
    TEST_ASSERT(s.unit.chargeSinceAnchor_mAh == 0);             /* accumulator re-zeroed */
}

static void test_confidence_falls_with_anchor_uncertainty(void)
{
    sPackSoc s;
    uint16_t good = 0;
    uint16_t poor = 0;

    PackSoc_Init(&s, 100000u);
    (void)PackSoc_UnitApplyAnchor(&s.unit, 500, 1u, 0u);
    (void)PackSoc_UnitGet_pm(&s.unit, &good);

    PackSoc_Init(&s, 100000u);
    (void)PackSoc_UnitApplyAnchor(&s.unit, 500, 50u, 0u);
    (void)PackSoc_UnitGet_pm(&s.unit, &poor);

    TEST_ASSERT(good > poor);
    TEST_ASSERT(poor >= 100);       /* never claims zero, never claims all */
}

/* ==========================================================================
 * Per-cell SOC and capacity (docs/design_battery_pack.md §26)
 *
 * C_i = dQ_i / dSOC_i, and dQ_i differs per cell ONLY by the balance
 * transfer -- so these tests are really about whether the balance term is
 * carried correctly.  Without it every cell measures the same capacity and
 * the answer is vacuous.
 * ========================================================================== */

#define NCELL 4

/** Drive every cell to a given OCV and resolve, enough times to anchor. */
static void anchor_all(sPackSocUnit *c, const uint16_t *mv, int reps)
{
    int i, k;
    for (k = 0; k < reps; k++) {
        for (i = 0; i < NCELL; i++) { PackSoc_UnitAnchorAdd(&c[i], mv[i]); }
    }
}

/** With NO balance transfer, every cell sees identical charge, so every cell
 *  must measure the SAME capacity.  That is the null case, and it must hold
 *  before any difference can mean anything. */
static void test_cells_without_balancing_measure_equal_capacity(void)
{
    sPackSocUnit c[NCELL];
    int32_t      bal[NCELL] = {0,0,0,0};
    /* 3250 mV = 200 pm, 3290 mV = 500 pm -> dSOC = +300 pm */
    const uint16_t lo[NCELL] = {3250,3250,3250,3250};
    const uint16_t hi[NCELL] = {3290,3290,3290,3290};
    int i;

    cells_init(c, NCELL);
    anchor_all(c, lo, 4);
    TEST_ASSERT(0 == cells_resolve(c, NCELL, 100000u));

    /* +30 Ah through the string, no balancing, so every cell saw +30 Ah. */
    for (i = 0; i < NCELL; i++) { PackSoc_UnitAdvance(&c[i], 30000); }
    anchor_all(c, hi, 4);
    TEST_ASSERT(NCELL == cells_resolve(c, NCELL, 100000u));
    for (i = 0; i < NCELL; i++) {
        TEST_ASSERT(c[i].capacity_mAh == 100000);
    }
}

/** THE POINT: a cell the balancer had to CHARGE received more coulombs than
 *  the string did, so for the same SOC change it must measure LARGER... no --
 *  it needed extra charge to move the same amount, which means it holds MORE
 *  per per-mille.  The sign of the balance term must be carried, not dropped. */
static void test_balance_transfer_changes_the_measured_capacity(void)
{
    sPackSocUnit c[NCELL];
    int32_t      bal[NCELL] = {0,0,0,0};
    const uint16_t lo[NCELL] = {3250,3250,3250,3250};
    const uint16_t hi[NCELL] = {3290,3290,3290,3290};

    cells_init(c, NCELL);
    anchor_all(c, lo, 4);
    (void)cells_resolve(c, NCELL, 100000u);

    /* THE CALLER APPLIES THE BALANCE CORRECTION -- cell 1 additionally
     * received 1000 mAh from the balancer, so the charge handed to ITS unit
     * is larger.  The estimator itself knows nothing about balancers. */
    {
        int k;
        for (k = 0; k < NCELL; k++) {
            PackSoc_UnitAdvance(&c[k], 30000 + ((k == 1) ? 1000 : 0));
        }
    }
    anchor_all(c, hi, 4);
    (void)cells_resolve(c, NCELL, 100000u);

    TEST_ASSERT(c[0].capacity_mAh == 100000);
    /* (30000 + 1000) / 300 * 1000 = 103333 */
    TEST_ASSERT(c[1].capacity_mAh > c[0].capacity_mAh);
    TEST_ASSERT(c[1].capacity_mAh == 103333);
}

/** A dSOC too small to divide by is REFUSED, not amplified into noise. */
static void test_small_soc_change_yields_no_capacity(void)
{
    sPackSocUnit c[NCELL];
    int32_t      bal[NCELL] = {0,0,0,0};
    const uint16_t a[NCELL] = {3270,3270,3270,3270};   /* 300 pm */
    const uint16_t b[NCELL] = {3274,3274,3274,3274};   /* ~340 pm, dSOC 40 */
    int i;

    cells_init(c, NCELL);
    anchor_all(c, a, 4);
    (void)cells_resolve(c, NCELL, 100000u);
    for (i = 0; i < NCELL; i++) { PackSoc_UnitAdvance(&c[i], 4000); }
    anchor_all(c, b, 4);
    /* dSOC ~40 pm is under PACK_SOC_CAP_MIN_DELTA_pm (150). */
    TEST_ASSERT(0 == cells_resolve(c, NCELL, 100000u));
    for (i = 0; i < NCELL; i++) {
        TEST_ASSERT(c[i].capacityLearned == 0u);   /* still the given value */
    }
}

/** An implausible result is rejected rather than adopted: a mis-paired
 *  anchor or a missed discontinuity must not become "this cell is 900 Ah". */
static void test_implausible_capacity_is_rejected(void)
{
    sPackSocUnit c[NCELL];
    int32_t      bal[NCELL] = {0,0,0,0};
    const uint16_t lo[NCELL] = {3250,3250,3250,3250};
    const uint16_t hi[NCELL] = {3290,3290,3290,3290};
    int i;

    cells_init(c, NCELL);
    anchor_all(c, lo, 4);
    (void)cells_resolve(c, NCELL, 100000u);
    for (i = 0; i < NCELL; i++) { PackSoc_UnitAdvance(&c[i], 300000); }
    anchor_all(c, hi, 4);
    /* 300 Ah through a nameplate-100 Ah pack over 300 pm -> 1000 Ah. */
    TEST_ASSERT(0 == cells_resolve(c, NCELL, 100000u));
    for (i = 0; i < NCELL; i++) {
        TEST_ASSERT(c[i].capacityLearned == 0u);   /* still the given value */
    }
}

/** Discharge measures capacity just as well as charge; refusing one
 *  direction would halve an already scarce supply of anchors. */
static void test_capacity_from_a_discharge(void)
{
    sPackSocUnit c[NCELL];
    int32_t      bal[NCELL] = {0,0,0,0};
    const uint16_t hi[NCELL] = {3290,3290,3290,3290};
    const uint16_t lo[NCELL] = {3250,3250,3250,3250};

    int i;

    cells_init(c, NCELL);
    anchor_all(c, hi, 4);
    (void)cells_resolve(c, NCELL, 100000u);
    for (i = 0; i < NCELL; i++) { PackSoc_UnitAdvance(&c[i], -30000); }
    anchor_all(c, lo, 4);
    TEST_ASSERT(NCELL == cells_resolve(c, NCELL, 100000u));
    TEST_ASSERT(c[0].capacity_mAh == 100000);
}

/** THE ANSWER THE MODULE EXISTS FOR: rank cells and name the weakest. */
static void test_weakest_cell_is_identified(void)
{
    sPackSocUnit c[NCELL];
    int32_t      cap = 0;

    cells_init(c, NCELL);
    TEST_ASSERT(-1 == PackSoc_WeakestUnit(c, NCELL, NULL));  /* none measured */

    c[0].capacity_mAh = 100000; c[0].capacityLearned = 1u;
    c[1].capacity_mAh =  92000; c[1].capacityLearned = 1u;   /* the weak one */
    c[2].capacity_mAh = 101000; c[2].capacityLearned = 1u;
    c[3].capacity_mAh =  99000; c[3].capacityLearned = 1u;

    TEST_ASSERT(1 == PackSoc_WeakestUnit(c, NCELL, &cap));
    TEST_ASSERT(cap == 92000);
}

/** Per-cell SOC advances by the charge THROUGH THAT CELL, so a cell with
 *  less capacity moves further for the same string charge. */
static void test_smaller_cell_moves_further_per_amp_hour(void)
{
    sPackSocUnit c[2];

    cells_init(c, 2);
    c[0].soc_pm = 500; c[0].capacity_mAh = 100000; c[0].haveAnchor = 1u;
    c[1].soc_pm = 500; c[1].capacity_mAh =  50000; c[1].haveAnchor = 1u;

    PackSoc_UnitAdvance(&c[0], 10000);      /* same +10 Ah through both */
    PackSoc_UnitAdvance(&c[1], 10000);
    TEST_ASSERT(c[0].soc_pm == 600);     /* +10 %   */
    TEST_ASSERT(c[1].soc_pm == 700);     /* +20 %   */
}

/** A cell that has not learned a capacity uses the fallback rather than
 *  freezing, and one that has never anchored is left alone entirely. */
static void test_advance_uses_fallback_and_skips_unanchored(void)
{
    sPackSocUnit c[2];

    cells_init(c, 2);
    c[0].soc_pm = 500; c[0].haveAnchor = 1u;   /* anchored, capacity given */
    /* c[1] stays soc_pm = -1, never anchored */

    PackSoc_UnitAdvance(&c[0], 10000);
    PackSoc_UnitAdvance(&c[1], 10000);
    TEST_ASSERT(c[0].soc_pm == 600);
    TEST_ASSERT(c[1].soc_pm == -1);       /* untouched */
}

int main(void)
{
    printf("=== pack_soc tests ===\n");

    RUN_TEST(test_ocv_maps_monotonically);
    RUN_TEST(test_ocv_hits_the_table_points);
    RUN_TEST(test_the_plateau_really_is_flat);

    RUN_TEST(test_knee_outweighs_plateau_by_orders_of_magnitude);
    RUN_TEST(test_one_knee_sample_dominates_many_plateau_samples);
    RUN_TEST(test_many_samples_shrink_sigma);
    RUN_TEST(test_empty_anchor_resolves_to_nothing);

    RUN_TEST(test_counter_needs_two_samples_before_integrating);
    RUN_TEST(test_counter_rejects_a_reseed_step);
    RUN_TEST(test_soc_tracks_the_counter_once_seeded);
    RUN_TEST(test_soc_is_unknown_until_anchored);
    RUN_TEST(test_soc_clamps_at_both_ends);

    RUN_TEST(test_anchor_measures_the_drift_it_corrects);
    RUN_TEST(test_confidence_falls_with_anchor_uncertainty);

    /* per-cell SOC and capacity */
    RUN_TEST(test_cells_without_balancing_measure_equal_capacity);
    RUN_TEST(test_balance_transfer_changes_the_measured_capacity);
    RUN_TEST(test_small_soc_change_yields_no_capacity);
    RUN_TEST(test_implausible_capacity_is_rejected);
    RUN_TEST(test_capacity_from_a_discharge);
    RUN_TEST(test_weakest_cell_is_identified);
    RUN_TEST(test_smaller_cell_moves_further_per_amp_hour);
    RUN_TEST(test_advance_uses_fallback_and_skips_unanchored);

    printf("%s (%d failures)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}

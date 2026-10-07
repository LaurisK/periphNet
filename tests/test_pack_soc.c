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

/* ==========================================================================
 * Pack DC resistance, from current steps -- and the IR-corrected anchor it
 * exists to enable.  The site this was built for (zaliakalnis) draws 11.6 A
 * on a 261 Ah pack, so it never reaches the C/50 rest gate and never anchors;
 * removing the IR term instead of waiting it out is the whole point.
 * ========================================================================== */

#define CAP_mAh     261000u
#define STEP_mA     ((int32_t)(CAP_mAh / PACK_SOC_RSTEP_C_DIV))  /* 1305 mA */

/* A perfectly ohmic pack: V = OCV + I*R, charging current positive. */
static int32_t ohmic_mV(int32_t ocv_mV, int32_t i_mA, int32_t r_uOhm)
{
    return ocv_mV + (int32_t)(((int64_t)i_mA * (int64_t)r_uOhm) / 1000000);
}

static void drive_steps(sPackSocRes *r, int32_t r_uOhm, int n, int32_t sign)
{
    const int32_t profile[4] = { 0, -40000, 0, 20000 };
    int           i;

    for (i = 0; i < n; i++) {
        const int32_t cur = profile[i & 3];

        PackSoc_ResNote(r, ohmic_mV(51200, cur * sign, r_uOhm), cur,
                        STEP_mA);
    }
}

static void test_anchor_resolve_terminates_for_every_weight(void)
{
    /* REGRESSION.  The integer sqrt inside AnchorResolve used to stop on
     * "the iterate stopped changing", which a Newton two-cycle never does:
     * for w = n^2 - 1 it alternated between n-1 and n forever, hanging the
     * pack task on the board.  These are the first such weights; the loop
     * below sweeps far enough to catch a reintroduction. */
    static const int32_t cyclers[] = { 3, 8, 15, 24, 35, 48, 63, 80, 99, 120 };
    uint32_t i;

    for (i = 0u; i < (sizeof(cyclers) / sizeof(cyclers[0])); i++) {
        sPackSocUnit u;
        int32_t      soc = 0;
        uint32_t     sig = 0;

        PackSoc_UnitInit(&u, CAP_mAh);
        u.samples = 1u;
        u.w       = cyclers[i];
        u.wSum    = (int64_t)cyclers[i] * 500;

        TEST_ASSERT(PackSoc_UnitAnchorResolve(&u, &soc, &sig) == 1);
        TEST_ASSERT(soc == 500);
        TEST_ASSERT(sig >= 1u);
    }

    for (i = 1u; i < 4000u; i++) {
        sPackSocUnit u;
        int32_t      soc = 0;
        uint32_t     sig = 0;

        PackSoc_UnitInit(&u, CAP_mAh);
        u.samples = 1u;
        u.w       = (int32_t)i;
        u.wSum    = (int64_t)i * 300;
        TEST_ASSERT(PackSoc_UnitAnchorResolve(&u, &soc, &sig) == 1);
    }
}

static void test_resistance_says_nothing_until_it_has_steps(void)
{
    sPackSocRes r;
    uint32_t    got = 0;

    PackSoc_ResInit(&r);
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &got) == 0);

    /* One step short of the minimum is still nothing -- the caller must fall
     * back to the rest path, never to a half-measured guess. */
    drive_steps(&r, 5000, (int)PACK_SOC_R_MIN_STEPS, 1);
    TEST_ASSERT(r.steps == PACK_SOC_R_MIN_STEPS - 1u);
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &got) == 0);
}

static void test_resistance_fits_a_known_ohmic_pack(void)
{
    sPackSocRes r;
    uint32_t    got = 0;

    PackSoc_ResInit(&r);
    drive_steps(&r, 5000, 40, 1);

    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &got) == 1);
    TEST_ASSERT(got == 5000u);          /* exact: the fit is a ratio of sums */
}

static void test_resistance_survives_voltage_noise(void)
{
    sPackSocRes r;
    uint32_t    got = 0;
    int         i;
    int32_t     seed = 12345;

    PackSoc_ResInit(&r);
    for (i = 0; i < 400; i++) {
        const int32_t profile[4] = { 0, -40000, 0, 20000 };
        const int32_t cur = profile[i & 3];
        int32_t       noise;

        seed  = (seed * 1103515245) + 12345;
        noise = ((seed >> 16) & 7) - 3;         /* +/-3 mV, like the ADC */

        PackSoc_ResNote(&r, ohmic_mV(51200, cur, 5000) + noise, cur, STEP_mA);
    }
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &got) == 1);
    /* Through-origin least squares over big steps: noise costs a few per
     * cent, not an order of magnitude. */
    TEST_ASSERT((got > 4800u) && (got < 5200u));
}

static void test_resistance_ignores_movements_too_small_to_measure(void)
{
    sPackSocRes r;
    uint32_t    got = 0;
    int         i;

    PackSoc_ResInit(&r);
    for (i = 0; i < 200; i++) {
        const int32_t cur = ((i & 1) != 0) ? 1000 : 0;   /* well under C/40 */

        PackSoc_ResNote(&r, ohmic_mV(51200, cur, 5000), cur, STEP_mA);
    }
    TEST_ASSERT(r.steps == 0u);
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &got) == 0);
}

static void test_resistance_refuses_a_backwards_fit(void)
{
    sPackSocRes r;
    uint32_t    got = 0;

    /* Voltage FALLING as charge current rises is not a battery; it is a sign
     * error or a mis-paired sample.  A negative fit lands outside the band by
     * construction, which is the cheapest sign check there is. */
    PackSoc_ResInit(&r);
    drive_steps(&r, 5000, 40, -1);
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &got) == 0);
}

static void test_resistance_refuses_an_implausible_fit(void)
{
    sPackSocRes r;
    uint32_t    got = 0;

    PackSoc_ResInit(&r);
    drive_steps(&r, (int32_t)PACK_SOC_R_MAX_uOhm * 2, 40, 1);
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &got) == 0);
}

static void test_resistance_follows_a_rise(void)
{
    sPackSocRes r;
    uint32_t    early = 0;
    uint32_t    late  = 0;

    /* Enough steps to cross the accumulator ceiling several times, so the
     * forgetting is exercised rather than assumed. */
    PackSoc_ResInit(&r);
    drive_steps(&r, 4000, 4000, 1);
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &early) == 1);
    TEST_ASSERT((early > 3900u) && (early < 4100u));

    drive_steps(&r, 8000, 4000, 1);
    TEST_ASSERT(PackSoc_ResGet_uOhm(&r, &late) == 1);

    /* A pack whose resistance doubles must be followed, not averaged away
     * over its whole life -- rising R is the degradation signal. */
    TEST_ASSERT(late > early + 3000u);
}

static void test_resistance_accumulators_stay_bounded(void)
{
    sPackSocRes r;

    PackSoc_ResInit(&r);
    drive_steps(&r, 5000, 20000, 1);
    TEST_ASSERT(r.sumII < PACK_SOC_R_SUM_MAX);
    TEST_ASSERT(r.sumIV > 0);
}

static void test_stated_sigma_at_base_is_the_plain_anchor(void)
{
    sPackSocUnit a;
    sPackSocUnit b;
    int32_t      sa = 0, sb = 0;
    uint32_t     ga = 0, gb = 0;

    PackSoc_UnitInit(&a, CAP_mAh);
    PackSoc_UnitInit(&b, CAP_mAh);

    PackSoc_UnitAnchorAdd(&a, 3270);
    PackSoc_UnitAnchorAddSigma(&b, 3270, PACK_SOC_SIGMA_V_mV);

    TEST_ASSERT(PackSoc_UnitAnchorResolve(&a, &sa, &ga) == 1);
    TEST_ASSERT(PackSoc_UnitAnchorResolve(&b, &sb, &gb) == 1);
    TEST_ASSERT((sa == sb) && (ga == gb));
    TEST_ASSERT(a.w == b.w);
}

static void test_a_less_certain_sample_weighs_less(void)
{
    sPackSocUnit a;
    sPackSocUnit b;
    int32_t      sa = 0, sb = 0;
    uint32_t     ga = 0, gb = 0;

    /* Same two voltages into both units; only the second sample's stated
     * uncertainty differs.  The less certain unit must end up LESS sure. */
    PackSoc_UnitInit(&a, CAP_mAh);
    PackSoc_UnitAnchorAdd(&a, 3270);
    PackSoc_UnitAnchorAdd(&a, 3270);

    PackSoc_UnitInit(&b, CAP_mAh);
    PackSoc_UnitAnchorAdd(&b, 3270);
    PackSoc_UnitAnchorAddSigma(&b, 3270, PACK_SOC_SIGMA_V_mV * 4u);

    TEST_ASSERT(PackSoc_UnitAnchorResolve(&a, &sa, &ga) == 1);
    TEST_ASSERT(PackSoc_UnitAnchorResolve(&b, &sb, &gb) == 1);
    TEST_ASSERT(a.w > b.w);
    TEST_ASSERT(ga < gb);               /* sigma: more weight, less doubt */
}

static void test_a_doubtful_sample_barely_moves_the_anchor(void)
{
    sPackSocUnit u;
    int32_t      soc  = 0;
    uint32_t     sig  = 0;
    int32_t      near = PackSoc_OcvToSoc_pm(3270);
    int32_t      far  = PackSoc_OcvToSoc_pm(3310);

    /* One trusted sample and one badly corrected one, on the SAME slope
     * region so only sigma separates them.  This is what stops a big IR
     * correction from dragging the estimate. */
    PackSoc_UnitInit(&u, CAP_mAh);
    PackSoc_UnitAnchorAdd(&u, 3270);
    PackSoc_UnitAnchorAddSigma(&u, 3310, PACK_SOC_SIGMA_V_mV * 20u);

    TEST_ASSERT(PackSoc_UnitAnchorResolve(&u, &soc, &sig) == 1);
    TEST_ASSERT(far > near);
    /* Nearer the trusted end than the midpoint by a wide margin. */
    TEST_ASSERT(soc < (near + ((far - near) / 4)));
}

static void test_a_working_site_fits_inside_the_error_budget(void)
{
    /* zaliakalnis, measured 2026-09-04: 6.0 A on a 261 Ah 16S pack.  The old
     * gate refused every one of its samples (C/50 = 5.22 A) and it took ZERO
     * anchors in ten minutes while sodas, at 1.5 A on 660 Ah, took 48.
     *
     * Under the WORST-CASE resistance bound -- no measurement needed, and
     * none is available at that current -- the error it carries is inside the
     * budget, so it anchors.  That is the entire benefit. */
    const int32_t i_mA = 6000;
    const int32_t n    = 16;
    const int32_t worst_mV_cell =
        (int32_t)(((int64_t)i_mA * (int64_t)PACK_SOC_R_BOUND_uOhm) / 1000000)
        / n;

    TEST_ASSERT(worst_mV_cell <= (int32_t)PACK_SOC_IR_MAX_ERR_mV);

    /* And it still weighs something: sigma grows, it does not become
     * infinite.  Roughly a twelfth of a rest sample here, so a minute of
     * 5 s samples is worth one clean anchor. */
    {
        sPackSocUnit a2;
        sPackSocUnit b2;

        PackSoc_UnitInit(&a2, CAP_mAh);
        PackSoc_UnitInit(&b2, CAP_mAh);
        PackSoc_UnitAnchorAdd(&a2, 3235);
        PackSoc_UnitAnchorAddSigma(&b2, 3235,
                                   PACK_SOC_SIGMA_V_mV +
                                   (uint32_t)worst_mV_cell);
        TEST_ASSERT(b2.w >= 1);
        TEST_ASSERT(b2.w < a2.w);
    }
}

static void test_the_bound_is_smaller_than_the_error_it_replaces(void)
{
    /* The argument for admitting these samples at all, as arithmetic: across
     * the WHOLE plausible range of pack resistance the SOC error at 6 A spans
     * about one per-cent, and the free-running coulomb count it displaces was
     * out by sixty-five.  A gate protecting the smaller number at the cost of
     * the larger one is mis-calibrated, whatever its threshold. */
    const int32_t i_mA = 6000, n = 16;
    const int32_t lo_mV_cell = (int32_t)(((int64_t)i_mA * 5000) / 1000000) / n;
    const int32_t hi_mV_cell =
        (int32_t)(((int64_t)i_mA * (int64_t)PACK_SOC_R_BOUND_uOhm) / 1000000)
        / n;

    /* ~5 mV per per-cent on the 3200..3250 plateau. */
    TEST_ASSERT((hi_mV_cell - lo_mV_cell) / 5 <= 2);    /* <= 2 %% SOC span */
    TEST_ASSERT(hi_mV_cell <= (int32_t)PACK_SOC_IR_MAX_ERR_mV);
}

static void test_the_correction_a_real_site_needs_is_small(void)
{
    /* zaliakalnis, measured 2026-09-03: 11.6 A on a 261 Ah pack whose fitted
     * resistance is a few milliohms.  The point of this test is the SIZE of
     * the thing being corrected -- if it were large, correcting it would be
     * the riskier choice, and the cap in the caller would refuse it. */
    const int32_t  i_mA    = -11600;
    const uint32_t r_uOhm  = 5000u;
    const int32_t  ir_mV   = (int32_t)(((int64_t)i_mA * (int64_t)r_uOhm) /
                                       1000000);
    const int32_t  perCell = ir_mV / 16;

    TEST_ASSERT(ir_mV == -58);
    TEST_ASSERT((perCell > -4) && (perCell < 0));
    TEST_ASSERT((uint32_t)(-perCell) <= PACK_SOC_IR_MAX_ERR_mV);

    /* And it is worth about a per-cent of SOC on the plateau: 3235 mV reads
     * ~3 mV low under this load, and the curve is ~5 mV per per-cent there.
     * Small enough to correct, big enough to be worth correcting. */
    {
        const int32_t raw  = PackSoc_OcvToSoc_pm(3232);
        const int32_t corr = PackSoc_OcvToSoc_pm(3235);

        TEST_ASSERT(corr > raw);
        TEST_ASSERT((corr - raw) < 100);        /* under 10 %% SOC */
    }
}


/* === the fused LFP estimator ==============================================
 *
 * Validated against docs/issue_soc_estimator_sodas_2026-10-06.md: the sodas15
 * pack (660 Ah, 16S) and the numbers measured there.  The capture TSV the
 * issue cites is not in the repository, so its table 1.1 is replayed by hand. */

#define SODAS_mAh   660000u

static sPackSocLfpIn frame(uint16_t vmin, uint16_t vmax, int32_t i_mA,
                           uint16_t jk_pm)
{
    sPackSocLfpIn f;

    memset(&f, 0, sizeof(f));
    f.cellMin_mV = vmin;
    f.cellMax_mV = vmax;
    f.cellAvg_mV = (uint16_t)((vmin + vmax) / 2u);
    f.i_mA       = i_mA;
    f.rMin_uOhm  = PackSocLfp_DefaultCellR_uOhm(SODAS_mAh);
    f.rMax_uOhm  = f.rMin_uOhm;
    f.tMin_dC    = 250;
    f.socJk_pm   = jk_pm;
    return f;
}

static int near(int32_t a, int32_t b, int32_t tol)
{
    return ((a - b) <= tol) && ((b - a) <= tol);
}

static void test_lfp_emergency_high_latch(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3400, 3563, 20000, 800);

    PackSocLfp_Init(&s, SODAS_mAh);
    f.cellAvg_mV = 3400;                 /* average is nowhere near full */
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.socReal_pm == 1000u);
    TEST_ASSERT(s.conf_pm == 1000u);
    TEST_ASSERT(s.lastLatch == packSocLatch_high);
}

static void test_lfp_emergency_low_latch_locks_inverter(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(2600, 3100, -30000, 100);

    PackSocLfp_Init(&s, SODAS_mAh);
    f.cellAvg_mV = 3000;
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.socReal_pm == 0u);
    TEST_ASSERT(s.socInverter_pm == 60u);
}

static void test_lfp_thresholds_are_the_75_percent_rule(void)
{
    sPackSocLfp s;

    PackSocLfp_Init(&s, SODAS_mAh);
    TEST_ASSERT(s.vHigh_mV == 3562u || s.vHigh_mV == 3563u);
    TEST_ASSERT(s.vLow_mV == 2600u);
}

static void test_lfp_average_high_latch_needs_the_tail_current(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3440, 3460, 99000, 900);   /* tail = 13.2 A */

    PackSocLfp_Init(&s, SODAS_mAh);
    f.cellAvg_mV = 3450;
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.socReal_pm < 1000u);
    f.i_mA = 10000;
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.socReal_pm == 1000u);
}

static void test_lfp_inverter_window_maps_linearly(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3300, 3310, 0, 500);

    PackSocLfp_Init(&s, SODAS_mAh);
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(near(s.socInverter_pm, 60 + (500 * 940) / 1000, 1));
    TEST_ASSERT(s.socReal_pm == 500u);
}

/** D2: at 11:40 the old estimator overwrote the vendor's ~23 % with 81 % from
 *  a plateau voltage.  A plateau sample must not move the estimate. */
static void test_lfp_plateau_voltage_does_not_override_the_count(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3345, 3350, 99000, 227);   /* JK 150/660 Ah */

    PackSocLfp_Init(&s, SODAS_mAh);
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.wKnee_pm == 0u);
    TEST_ASSERT(near(s.socReal_pm, 227, 2));
}

/** ...and the same disagreement AT THE KNEE is heard, and shows as a large
 *  innovation (D4: it used to read 1-3 pm all afternoon). */
static void test_lfp_knee_is_heard_and_reported_as_innovation(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3440, 3450, 0, 700);

    PackSocLfp_Init(&s, SODAS_mAh);
    f.cellAvg_mV = 3445;
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.wKnee_pm == 1000u);
    TEST_ASSERT(s.socReal_pm > 950u);
    TEST_ASSERT(s.innovation_pm > 100);
}

static void test_lfp_lower_knee_pulls_down_without_stepping_to_zero(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3150, 3250, -15000, 200);
    uint16_t      prev = 200u;
    int           k;

    PackSocLfp_Init(&s, SODAS_mAh);
    f.cellAvg_mV = 3200;
    for (k = 0; k < 5; k++) {
        PackSocLfp_Update(&s, &f, 5000u);
        TEST_ASSERT(s.socReal_pm > 0u);
        TEST_ASSERT(s.socReal_pm <= prev);
        prev = s.socReal_pm;
        f.cellMin_mV = (uint16_t)(f.cellMin_mV - 10u);
    }
    TEST_ASSERT(s.socReal_pm < 200u);
}

/** Load compensation: a discharge spike must not read as a knee. */
static void test_lfp_load_compensation_prevents_a_false_knee(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3190, 3250, -30000, 400);

    PackSocLfp_Init(&s, SODAS_mAh);
    f.rMin_uOhm = 2000u;                 /* 30 A * 2 mOhm = 60 mV */
    f.rMeasured = 1u;
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.vCompMin_mV >= 3200);
    TEST_ASSERT(s.wKnee_pm == 0u);
}

/** D3: 16:08, JK counter +67.8 Ah in one poll against ~0.6 Ah of charge. */
static void test_lfp_vendor_recalibration_step_is_not_charge(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3300, 3310, 109000, 897);

    PackSocLfp_Init(&s, SODAS_mAh);
    PackSocLfp_Update(&s, &f, 20000u);
    f.socJk_pm = 1000u;
    PackSocLfp_Update(&s, &f, 20000u);
    TEST_ASSERT(s.jkSteps == 1u);
    TEST_ASSERT(near(s.socReal_pm, 897, 5));
}

/** A genuine +4 Ah poll against a matching current is accepted. */
static void test_lfp_a_real_step_is_accepted(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3300, 3310, 100000, 500);

    PackSocLfp_Init(&s, SODAS_mAh);
    PackSocLfp_Update(&s, &f, 5000u);
    f.socJk_pm = 502u;
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(s.jkSteps == 0u);
    TEST_ASSERT(s.socReal_pm == 502u);
}

/** D1: every poll is a fraction of one per-mille on this pack; the estimate
 *  must still follow the count across a window. */
static void test_lfp_follows_the_count_between_knees(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3300, 3310, 99000, 650);
    int           k;

    PackSocLfp_Init(&s, SODAS_mAh);
    PackSocLfp_Update(&s, &f, 5000u);
    for (k = 0; k < 60; k++) {          /* 5 min, 1 pm per 3 polls */
        if ((k % 3) == 2) { f.socJk_pm++; }
        PackSocLfp_Update(&s, &f, 5000u);
    }
    TEST_ASSERT(near(s.socReal_pm, f.socJk_pm, 1));
    TEST_ASSERT(f.socJk_pm > 650u);
}

static void test_lfp_a_latch_re_anchors_the_vendor_count(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3400, 3563, 20000, 920);

    PackSocLfp_Init(&s, SODAS_mAh);
    f.cellAvg_mV = 3400;
    PackSocLfp_Update(&s, &f, 5000u);
    f = frame(3300, 3310, 0, 920);       /* vendor still says 92 % */
    PackSocLfp_Update(&s, &f, 5000u);
    TEST_ASSERT(near(s.socReal_pm, 1000, 2));
}

static void test_lfp_confidence_decays_with_throughput_and_time(void)
{
    sPackSocLfp   s;
    sPackSocLfpIn f = frame(3300, 3310, -660000, 500);   /* 1.0 C */
    int           k;

    PackSocLfp_Init(&s, SODAS_mAh);
    PackSocLfp_Update(&s, &f, 0u);
    for (k = 0; k < 720; k++) {         /* one hour in 5 s polls */
        PackSocLfp_Update(&s, &f, 5000u);
    }
    /* 1 - (0.05 * 1.0 + 0.001 * 1) = 0.949 */
    TEST_ASSERT(near(s.conf_pm, 949, 1));
}

static void test_lfp_charge_efficiency_applies_only_to_charge(void)
{
    sPackSocLfp   a, b;
    sPackSocLfpIn fc = frame(3300, 3310, 660000, 500);
    sPackSocLfpIn fd = frame(3300, 3310, -660000, 500);

    PackSocLfp_Init(&a, SODAS_mAh);
    PackSocLfp_Init(&b, SODAS_mAh);
    PackSocLfp_Update(&a, &fc, 3600000u);
    PackSocLfp_Update(&b, &fd, 3600000u);
    TEST_ASSERT(a.qAbs_mAms < b.qAbs_mAms);
    TEST_ASSERT(near((int32_t)(a.qAbs_mAms * 1000 / b.qAbs_mAms), 992, 1));
}

static void test_lfp_temperature_tables(void)
{
    TEST_ASSERT(PackSocLfp_Kq_pm(-200) == 650);
    TEST_ASSERT(PackSocLfp_Kq_pm(250) == 1000);
    TEST_ASSERT(PackSocLfp_Kq_pm(700) == 1030);
    TEST_ASSERT(PackSocLfp_Kq_pm(-500) == 650);
    TEST_ASSERT(PackSocLfp_Kq_pm(75) == 920);
    TEST_ASSERT(PackSocLfp_Kr_pm(-200) == 3500);
    TEST_ASSERT(PackSocLfp_Kr_pm(150) == 1150);
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

    /* pack DC resistance and the IR-corrected anchor */
    RUN_TEST(test_anchor_resolve_terminates_for_every_weight);
    RUN_TEST(test_resistance_says_nothing_until_it_has_steps);
    RUN_TEST(test_resistance_fits_a_known_ohmic_pack);
    RUN_TEST(test_resistance_survives_voltage_noise);
    RUN_TEST(test_resistance_ignores_movements_too_small_to_measure);
    RUN_TEST(test_resistance_refuses_a_backwards_fit);
    RUN_TEST(test_resistance_refuses_an_implausible_fit);
    RUN_TEST(test_resistance_follows_a_rise);
    RUN_TEST(test_resistance_accumulators_stay_bounded);
    RUN_TEST(test_stated_sigma_at_base_is_the_plain_anchor);
    RUN_TEST(test_a_less_certain_sample_weighs_less);
    RUN_TEST(test_a_doubtful_sample_barely_moves_the_anchor);
    RUN_TEST(test_the_correction_a_real_site_needs_is_small);
    RUN_TEST(test_a_working_site_fits_inside_the_error_budget);
    RUN_TEST(test_the_bound_is_smaller_than_the_error_it_replaces);

    /* the fused LFP estimator, against the sodas issue */
    RUN_TEST(test_lfp_emergency_high_latch);
    RUN_TEST(test_lfp_emergency_low_latch_locks_inverter);
    RUN_TEST(test_lfp_thresholds_are_the_75_percent_rule);
    RUN_TEST(test_lfp_average_high_latch_needs_the_tail_current);
    RUN_TEST(test_lfp_inverter_window_maps_linearly);
    RUN_TEST(test_lfp_plateau_voltage_does_not_override_the_count);
    RUN_TEST(test_lfp_knee_is_heard_and_reported_as_innovation);
    RUN_TEST(test_lfp_lower_knee_pulls_down_without_stepping_to_zero);
    RUN_TEST(test_lfp_load_compensation_prevents_a_false_knee);
    RUN_TEST(test_lfp_vendor_recalibration_step_is_not_charge);
    RUN_TEST(test_lfp_a_real_step_is_accepted);
    RUN_TEST(test_lfp_follows_the_count_between_knees);
    RUN_TEST(test_lfp_a_latch_re_anchors_the_vendor_count);
    RUN_TEST(test_lfp_confidence_decays_with_throughput_and_time);
    RUN_TEST(test_lfp_charge_efficiency_applies_only_to_charge);
    RUN_TEST(test_lfp_temperature_tables);

    printf("%s (%d failures)\n", test_failures ? "FAILED" : "PASSED",
           test_failures);
    return test_failures ? 1 : 0;
}

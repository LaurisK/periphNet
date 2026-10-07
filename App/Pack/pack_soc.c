/*
 * pack_soc.c
 *
 * See pack_soc.h.  Pure, libc only, host-tested.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Pack/pack_soc.h"

#include <string.h>

/* Private types ------------------------------------------------------------*/

typedef struct {
    uint16_t ocv_mV;
    int16_t  soc_pm;
} sOcvPoint;

/* Private variables --------------------------------------------------------*/

/**
 * Rest OCV for a prismatic LFP cell at 25 degC, from
 * design_bms_cell_health_estimation.md §4.
 *
 * THE FLATNESS IS THE POINT.  From 300 to 800 per-mille the whole curve moves
 * 20 mV -- 1 mV per per-cent -- which is why coulomb counting has to carry
 * the middle and why an estimator that admits plateau samples at full weight
 * converges confidently to a wrong answer.
 */
static const sOcvPoint s_ocv[] = {
    { 2500,    0 },
    { 3100,   50 },
    { 3200,  100 },
    { 3250,  200 },
    { 3270,  300 },
    { 3290,  500 },
    { 3310,  700 },
    { 3320,  800 },
    { 3340,  900 },
    { 3370,  950 },
    { 3450,  990 },
    { 3650, 1000 },
};

#define OCV_N   (sizeof(s_ocv) / sizeof(s_ocv[0]))

/* Exported functions -------------------------------------------------------*/

int32_t PackSoc_OcvToSoc_pm(uint16_t ocv_mV)
{
    uint32_t i;

    if (ocv_mV <= s_ocv[0].ocv_mV) {
        return 0;
    }
    if (ocv_mV >= s_ocv[OCV_N - 1u].ocv_mV) {
        return (int32_t)PACK_SOC_FULL_pm;
    }

    for (i = 1u; i < OCV_N; i++) {
        if (ocv_mV <= s_ocv[i].ocv_mV) {
            const int32_t dV = (int32_t)s_ocv[i].ocv_mV -
                               (int32_t)s_ocv[i - 1u].ocv_mV;
            const int32_t dS = (int32_t)s_ocv[i].soc_pm -
                               (int32_t)s_ocv[i - 1u].soc_pm;
            const int32_t off = (int32_t)ocv_mV -
                                (int32_t)s_ocv[i - 1u].ocv_mV;

            if (dV <= 0) {
                return s_ocv[i - 1u].soc_pm;
            }
            return (int32_t)s_ocv[i - 1u].soc_pm + ((off * dS) / dV);
        }
    }
    return (int32_t)PACK_SOC_FULL_pm;
}

uint32_t PackSoc_Slope_uV_per_pm(uint16_t ocv_mV)
{
    uint32_t i;

    for (i = 1u; i < OCV_N; i++) {
        if ((ocv_mV <= s_ocv[i].ocv_mV) || (i == (OCV_N - 1u))) {
            const int32_t dV = (int32_t)s_ocv[i].ocv_mV -
                               (int32_t)s_ocv[i - 1u].ocv_mV;
            const int32_t dS = (int32_t)s_ocv[i].soc_pm -
                               (int32_t)s_ocv[i - 1u].soc_pm;

            if (dS <= 0) {
                break;
            }
            /* microvolts per per-mille: mV/pm * 1000 */
            return (uint32_t)((dV * 1000) / dS);
        }
    }
    return 1u;                  /* never zero: a weight must be computable */
}

/* --- the unit: one implementation, used for a pack AND for a cell -------- */

void PackSoc_UnitInit(sPackSocUnit *u, uint32_t capacity_mAh)
{
    if (u == NULL) {
        return;
    }
    (void)memset(u, 0, sizeof(*u));
    u->capacity_mAh = (int32_t)capacity_mAh;
    u->soc_pm       = -1;               /* unknown until anchored */
}

void PackSoc_UnitAnchorAdd(sPackSocUnit *u, uint16_t ocv_mV)
{
    PackSoc_UnitAnchorAddSigma(u, ocv_mV, PACK_SOC_SIGMA_V_mV);
}

void PackSoc_UnitAnchorAddSigma(sPackSocUnit *u, uint16_t ocv_mV,
                                uint32_t sigmaV_mV)
{
    uint32_t k;
    int32_t  w;

    if (u == NULL) {
        return;
    }
    if (sigmaV_mV < PACK_SOC_SIGMA_V_mV) {
        sigmaV_mV = PACK_SOC_SIGMA_V_mV;   /* nothing beats the ADC */
    }

    /* WEIGHT IS k SQUARED: a sample's SOC uncertainty is sigma_V / k, so
     * inverse-variance weighting is k^2.  This is what makes the gate graded
     * rather than a cliff -- and a cliff, measured, admits nothing. */
    k = PackSoc_Slope_uV_per_pm(ocv_mV);
    w = (int32_t)(((int64_t)k * (int64_t)k) / 1000);

    /* A SAMPLE KNOWN LESS WELL WEIGHS LESS, by the same inverse-variance
     * rule: scaling by (base/sigma)^2 leaves the rest path arithmetically
     * untouched at sigma == base, and lets an IR-corrected sample pay for its
     * correction in weight rather than be admitted or refused outright. */
    if (sigmaV_mV > PACK_SOC_SIGMA_V_mV) {
        const int64_t base = (int64_t)PACK_SOC_SIGMA_V_mV;

        w = (int32_t)(((int64_t)w * base * base) /
                      ((int64_t)sigmaV_mV * (int64_t)sigmaV_mV));
    }
    if (w < 1) {
        w = 1;
    }
    u->wSum += (int64_t)w * (int64_t)PackSoc_OcvToSoc_pm(ocv_mV);
    u->w    += w;
    u->samples++;
}

int PackSoc_UnitAnchorResolve(const sPackSocUnit *u, int32_t *soc_pm_out,
                              uint32_t *sigma_pm_out)
{
    if ((u == NULL) || (u->samples == 0u) || (u->w <= 0)) {
        return 0;
    }
    if (soc_pm_out != NULL) {
        *soc_pm_out = (int32_t)(u->wSum / (int64_t)u->w);
    }
    if (sigma_pm_out != NULL) {
        /* sigma = sigma_V / sqrt(sum w).  Integer sqrt by Newton; this runs
         * once per anchor, not per sample.
         *
         * TERMINATION IS THE SUBTLE PART.  Newton from above descends to
         * floor(sqrt(x)) and then, for x = n^2 - 1, ENTERS A TWO-CYCLE
         * between n-1 and n: an earlier form stopped on `r != prev`, which a
         * two-cycle never satisfies, so this loop ran forever for x = 3, 8,
         * 15, 24, 35 ... -- 1731 values below three million, and w is a sum
         * of weights that lands wherever it lands.  Stopping when the iterate
         * stops DECREASING is what makes it total: the sequence is monotone
         * until the cycle, so the last decrease is the answer. */
        int64_t  x = u->w;
        int64_t  r;
        int64_t  prev;
        uint32_t sigma;

        if (x < 1) {
            x = 1;
        }
        r = x;
        for (;;) {
            prev = r;
            r = (r + (x / r)) / 2;
            if (r >= prev) {
                break;
            }
        }
        r = prev;
        if (r < 1) {
            r = 1;
        }
        sigma = (uint32_t)(((int64_t)PACK_SOC_SIGMA_V_mV * 1000) / r);
        if (sigma == 0u) {
            sigma = 1u;
        }
        *sigma_pm_out = sigma;
    }
    return 1;
}

void PackSoc_UnitAdvance(sPackSocUnit *u, int32_t dQ_mAh)
{
    int32_t moved;

    if ((u == NULL) || (u->capacity_mAh <= 0)) {
        return;
    }

    /* Accumulated even before the first anchor: it is the charge that will
     * measure capacity once two anchors exist. */
    u->chargeSinceAnchor_mAh += dQ_mAh;

    if (u->soc_pm < 0) {
        return;                         /* not anchored: nothing to move */
    }

    moved = (int32_t)(((int64_t)dQ_mAh * (int64_t)PACK_SOC_FULL_pm) /
                      (int64_t)u->capacity_mAh);
    {
        int32_t v = (int32_t)u->soc_pm + moved;

        if (v < 0) {
            v = 0;
        }
        if (v > (int32_t)PACK_SOC_FULL_pm) {
            v = (int32_t)PACK_SOC_FULL_pm;
        }
        u->soc_pm = (int16_t)v;
    }
}

int PackSoc_UnitApplyAnchor(sPackSocUnit *u, int32_t anchorSoc_pm,
                            uint32_t sigma_pm, uint32_t nameplate_mAh)
{
    int learned = 0;

    if (u == NULL) {
        return 0;
    }

    if (u->haveAnchor != 0u) {
        const int32_t dSoc = anchorSoc_pm - (int32_t)u->anchorSoc_pm;

        /* THE DRIFT: what coulomb counting predicted versus what the anchor
         * found.  The only measurement that bounds how wrong the plateau
         * estimate can be. */
        if (u->soc_pm >= 0) {
            u->driftResidual_pm = (int16_t)(anchorSoc_pm - u->soc_pm);
        }

        /* CAPACITY, from the same two anchors: C = dQ / dSOC.  Identical for
         * a pack and for a cell -- the caller has already made dQ mean "the
         * charge through THIS unit". */
        if ((dSoc >= PACK_SOC_CAP_MIN_DELTA_pm) ||
            (dSoc <= -PACK_SOC_CAP_MIN_DELTA_pm)) {
            int32_t cap = (int32_t)(((int64_t)u->chargeSinceAnchor_mAh *
                                     (int64_t)PACK_SOC_FULL_pm) /
                                    (int64_t)dSoc);

            if (cap < 0) {
                cap = -cap;
            }
            if (nameplate_mAh > 0u) {
                const int32_t lo = (int32_t)((nameplate_mAh *
                                              PACK_SOC_CAP_MIN_NUM) /
                                             PACK_SOC_CAP_MIN_DEN);
                const int32_t hi = (int32_t)(nameplate_mAh *
                                             PACK_SOC_CAP_MAX_NUM);

                /* Outside the band this is arithmetic gone wrong -- a
                 * mis-paired anchor, a missed discontinuity -- not a
                 * discovery about the battery. */
                if ((cap < lo) || (cap > hi)) {
                    cap = 0;
                }
            }
            if (cap > 0) {
                if (u->capacityLearned == 0u) {
                    u->capacity_mAh   = cap;
                    u->capConf_pm     = 300u;
                    u->capacityLearned = 1u;
                } else {
                    /* Blend: one pair of anchors is noisy, and a capacity
                     * that jumps around cannot decide a replacement. */
                    u->capacity_mAh = ((u->capacity_mAh * 3) + cap) / 4;
                    if (u->capConf_pm < 900u) {
                        u->capConf_pm = (uint16_t)(u->capConf_pm + 100u);
                    }
                }
                learned = 1;
            }
        }
    }

    u->soc_pm                = (int16_t)anchorSoc_pm;
    u->anchorSoc_pm          = (int16_t)anchorSoc_pm;
    u->chargeSinceAnchor_mAh = 0;
    u->haveAnchor            = 1u;
    u->wSum                  = 0;
    u->w                     = 0;
    u->samples               = 0u;

    {
        int32_t c = (int32_t)PACK_SOC_FULL_pm - ((int32_t)sigma_pm * 10);

        if (c < 100) {
            c = 100;
        }
        if (c > (int32_t)PACK_SOC_FULL_pm) {
            c = (int32_t)PACK_SOC_FULL_pm;
        }
        u->conf_pm = (uint16_t)c;
    }
    return learned;
}

int32_t PackSoc_UnitGet_pm(const sPackSocUnit *u, uint16_t *conf_pm_out)
{
    if ((u == NULL) || (u->haveAnchor == 0u)) {
        return -1;
    }
    if (conf_pm_out != NULL) {
        *conf_pm_out = u->conf_pm;
    }
    return u->soc_pm;
}

int PackSoc_WeakestUnit(const sPackSocUnit *u, uint8_t n, int32_t *cap_mAh_out)
{
    uint8_t i;
    int     worst = -1;

    if (u == NULL) {
        return -1;
    }
    for (i = 0u; i < n; i++) {
        if (u[i].capacityLearned == 0u) {
            continue;                   /* never measured: not a candidate */
        }
        if ((worst < 0) || (u[i].capacity_mAh < u[worst].capacity_mAh)) {
            worst = (int)i;
        }
    }
    if ((worst >= 0) && (cap_mAh_out != NULL)) {
        *cap_mAh_out = u[worst].capacity_mAh;
    }
    return worst;
}

/* --- the pack's vendor-counter bookkeeping ------------------------------ */

void PackSoc_Init(sPackSoc *s, uint32_t capacity_mAh)
{
    if (s == NULL) {
        return;
    }
    (void)memset(s, 0, sizeof(*s));
    PackSoc_UnitInit(&s->unit, capacity_mAh);
}

/* --- pack DC resistance, from current steps ------------------------------ */

void PackSoc_ResInit(sPackSocRes *r)
{
    if (r != NULL) {
        (void)memset(r, 0, sizeof(*r));
    }
}

void PackSoc_ResNote(sPackSocRes *r, int32_t v_mV, int32_t i_mA,
                     int32_t minStep_mA)
{
    if (r == NULL) {
        return;
    }
    if (minStep_mA < 1) {
        minStep_mA = 1;
    }

    if (r->haveLast != 0u) {
        const int32_t dI = i_mA - r->lastI_mA;
        const int32_t dV = v_mV - r->lastV_mV;

        /* ONLY STEPS.  Between two samples the pack's OCV moves by the charge
         * that passed -- at 45 A on a 261 Ah pack over 5 s that is 0.024 %%
         * SOC, which on this curve is microvolts.  So across a step dV is the
         * IR term and essentially nothing else, and no OCV model is needed to
         * separate them.  Small dI is the case where that stops being true,
         * and it is also where dV is pure noise; both argue for the same
         * threshold. */
        if ((dI >= minStep_mA) || (dI <= -minStep_mA)) {
            r->sumIV += (int64_t)dI * (int64_t)dV;
            r->sumII += (int64_t)dI * (int64_t)dI;
            if (r->steps < 0xFFFFFFFFu) {
                r->steps++;
            }

            /* Halve on reaching the ceiling: bounds the arithmetic, and makes
             * the fit forget at a rate that lets a slowly rising resistance
             * be followed instead of averaged away over the pack's life. */
            if (r->sumII >= PACK_SOC_R_SUM_MAX) {
                r->sumIV /= 2;
                r->sumII /= 2;
            }
        }
    }

    r->lastI_mA  = i_mA;
    r->lastV_mV  = v_mV;
    r->haveLast  = 1u;
}

int PackSoc_ResGet_uOhm(const sPackSocRes *r, uint32_t *r_uOhm_out)
{
    int64_t v;

    if ((r == NULL) || (r->steps < PACK_SOC_R_MIN_STEPS) ||
        (r->sumII <= 0)) {
        return 0;
    }

    /* dV in mV over dI in mA is ohms; the 1e6 carries it to micro-ohms, and
     * is applied AFTER the division's numerator so the ratio keeps its
     * resolution on a pack whose resistance is a few milliohms. */
    v = (r->sumIV * 1000000) / r->sumII;

    /* A negative fit fails this test by construction, which is the sign check
     * -- charging harder must raise the terminal voltage, and a fit that says
     * otherwise has been fed something that was not a load step. */
    if ((v < (int64_t)PACK_SOC_R_MIN_uOhm) ||
        (v > (int64_t)PACK_SOC_R_MAX_uOhm)) {
        return 0;
    }
    if (r_uOhm_out != NULL) {
        *r_uOhm_out = (uint32_t)v;
    }
    return 1;
}

int PackSoc_NoteCounter(sPackSoc *s, int32_t counter_mAh, uint32_t maxStep_mAh,
                        int32_t *dQ_mAh_out)
{
    int32_t d;

    if (dQ_mAh_out != NULL) {
        *dQ_mAh_out = 0;
    }
    if (s == NULL) {
        return 0;
    }
    if (s->haveCounter == 0u) {
        s->lastCounter_mAh = counter_mAh;
        s->haveCounter     = 1u;
        return 0;                       /* no interval yet */
    }

    d = counter_mAh - s->lastCounter_mAh;
    s->lastCounter_mAh = counter_mAh;

    /* A STEP IS NOT CHARGE.  The counter is fenced at the ends of its range
     * and re-seeded from a voltage estimate on any configuration change;
     * both look like a huge delta and neither is current that flowed. */
    if ((d > (int32_t)maxStep_mAh) || (d < -(int32_t)maxStep_mAh)) {
        return 0;
    }

    if (dQ_mAh_out != NULL) {
        *dQ_mAh_out = d;
    }
    return 1;
}

/* --- the fused LFP estimator --------------------------------------------- */

#define LFP_TEMP_N  7

static const int16_t s_lfpT[LFP_TEMP_N]  = { -200, -100, 0, 150, 250, 450, 700 };
static const int16_t s_lfpKq[LFP_TEMP_N] = { 650, 780, 880, 960, 1000, 1020, 1030 };
static const int16_t s_lfpKr[LFP_TEMP_N] = { 3500, 2300, 1600, 1150, 1000, 920, 900 };

static int32_t Clamp32(int32_t x, int32_t lo, int32_t hi)
{
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* Piecewise-linear over the temperature axis, saturating at both ends. */
static int32_t LfpInterp(const int16_t *ys, int16_t t_dC)
{
    uint32_t i;

    if (t_dC <= s_lfpT[0]) {
        return ys[0];
    }
    if (t_dC >= s_lfpT[LFP_TEMP_N - 1u]) {
        return ys[LFP_TEMP_N - 1u];
    }
    for (i = 1u; i < LFP_TEMP_N; i++) {
        if (t_dC <= s_lfpT[i]) {
            const int32_t dT = (int32_t)s_lfpT[i] - (int32_t)s_lfpT[i - 1u];
            const int32_t dY = (int32_t)ys[i] - (int32_t)ys[i - 1u];

            return (int32_t)ys[i - 1u] +
                   (((int32_t)t_dC - (int32_t)s_lfpT[i - 1u]) * dY) / dT;
        }
    }
    return ys[LFP_TEMP_N - 1u];
}

int32_t PackSocLfp_Kq_pm(int16_t t_dC) { return LfpInterp(s_lfpKq, t_dC); }
int32_t PackSocLfp_Kr_pm(int16_t t_dC) { return LfpInterp(s_lfpKr, t_dC); }

uint32_t PackSocLfp_DefaultCellR_uOhm(uint32_t qNominal_mAh)
{
    const uint32_t ah = (qNominal_mAh >= 1000u) ? (qNominal_mAh / 1000u) : 1u;

    return PACK_SOC_LFP_R_UOHM_AH / ah;
}

static void LfpThresholds(sPackSocLfp *s)
{
    s->vHigh_mV = (uint16_t)(PACK_SOC_LFP_V_FULL_mV +
        (3u * (PACK_SOC_LFP_V_OVERCHG_mV - PACK_SOC_LFP_V_FULL_mV)) / 4u);
    s->vLow_mV  = (uint16_t)(PACK_SOC_LFP_V_EMPTY_mV -
        (3u * (PACK_SOC_LFP_V_EMPTY_mV - PACK_SOC_LFP_V_UNDERDIS_mV)) / 4u);
}

static void LfpMapInverter(sPackSocLfp *s)
{
    s->socInverter_pm = (uint16_t)((uint32_t)s->dispMin_pm +
        ((uint32_t)s->socReal_pm * (uint32_t)(s->dispMax_pm - s->dispMin_pm)) /
        PACK_SOC_FULL_pm);
}

void PackSocLfp_Init(sPackSocLfp *s, uint32_t qNominal_mAh)
{
    if (s == NULL) {
        return;
    }
    (void)memset(s, 0, sizeof(*s));
    s->qNominal_mAh = qNominal_mAh;
    s->qEff_mAh     = qNominal_mAh;
    s->dispMin_pm   = PACK_SOC_LFP_DISP_MIN_pm;
    s->dispMax_pm   = PACK_SOC_LFP_DISP_MAX_pm;
    s->conf_pm      = (uint16_t)PACK_SOC_FULL_pm;
    LfpThresholds(s);
    s->socInverter_pm = s->dispMin_pm;
}

static void LfpLatch(sPackSocLfp *s, const sPackSocLfpIn *in, int32_t target)
{
    s->socReal_pm   = (uint16_t)target;
    s->jkOffset_pm  = target - (int32_t)in->socJk_pm;  /* re-anchor the vendor */
    s->lastJk_pm    = (int32_t)in->socJk_pm;
    s->haveJk       = 1u;
    s->conf_pm      = (uint16_t)PACK_SOC_FULL_pm;
    s->qAbs_mAms    = 0;
    s->tLatch_ms    = 0;
    s->wKnee_pm     = 0u;
    s->innovation_pm = 0;
    s->haveEst      = 1u;
    if (target >= (int32_t)(PACK_SOC_FULL_pm / 2u)) {
        s->lastLatch = packSocLatch_high;
        s->latchHigh++;
    } else {
        s->lastLatch = packSocLatch_low;
        s->latchLow++;
    }
    LfpMapInverter(s);
}

void PackSocLfp_Update(sPackSocLfp *s, const sPackSocLfpIn *in, uint32_t dt_ms)
{
    int32_t  tailCur_mA;
    int32_t  i;
    int32_t  kr;
    int32_t  rMin;
    int32_t  rMax;
    int32_t  vCompMin;
    int32_t  vCompMax;
    int32_t  wLower;
    int32_t  wUpper;
    int32_t  wKnee;
    int32_t  vKnee;
    int32_t  jkRaw;
    int32_t  jkCorr;
    int32_t  soc;
    int64_t  wCoul;
    int64_t  denom;
    int64_t  decay;
    uint32_t dtInt_ms;

    if ((s == NULL) || (in == NULL) || (s->qNominal_mAh == 0u)) {
        return;
    }
    i = in->i_mA;
    s->lastLatch = packSocLatch_none;

    /* Temperature scaling. */
    s->qEff_mAh = (uint32_t)(((uint64_t)s->qNominal_mAh *
                              (uint32_t)PackSocLfp_Kq_pm(in->tMin_dC)) / 1000u);
    kr   = (in->rMeasured != 0u) ? 1000 : PackSocLfp_Kr_pm(in->tMin_dC);
    rMin = (int32_t)(((int64_t)in->rMin_uOhm * kr) / 1000);
    rMax = (int32_t)(((int64_t)in->rMax_uOhm * kr) / 1000);

    /* LATCHES, before anything integrates: they win the frame. */
    tailCur_mA = (int32_t)(((uint64_t)s->qNominal_mAh *
                            PACK_SOC_LFP_TAIL_C_pm) / 1000u);
    if (((in->cellAvg_mV >= PACK_SOC_LFP_V_FULL_mV) &&
         (i >= 0) && (i <= tailCur_mA)) ||
        (in->cellMax_mV >= s->vHigh_mV)) {
        LfpLatch(s, in, (int32_t)PACK_SOC_FULL_pm);
        return;
    }
    if ((in->cellAvg_mV <= PACK_SOC_LFP_V_EMPTY_mV) ||
        (in->cellMin_mV <= s->vLow_mV)) {
        LfpLatch(s, in, 0);
        return;
    }

    /* Vendor SOC, re-anchored by the last latch.  A step the current does
     * not explain is a vendor re-calibration: absorb it into the offset so
     * the estimate stays continuous (D3: +67.8 Ah in one poll was believed
     * as charge and drove the pack to 100 %). */
    jkRaw = (int32_t)in->socJk_pm;
    dtInt_ms = (dt_ms > PACK_SOC_LFP_DT_MAX_ms) ? PACK_SOC_LFP_DT_MAX_ms : dt_ms;
    if (s->haveJk != 0u) {
        const int32_t expect = (int32_t)(((int64_t)i * (int64_t)dtInt_ms) /
                                         (3600 * (int64_t)s->qNominal_mAh));
        const int32_t step   = (jkRaw - s->lastJk_pm) - expect;

        if ((step > PACK_SOC_LFP_JK_STEP_pm) ||
            (step < -PACK_SOC_LFP_JK_STEP_pm)) {
            s->jkOffset_pm -= step;
            s->jkSteps++;
        }
    }
    s->lastJk_pm = jkRaw;
    s->haveJk    = 1u;
    jkCorr = Clamp32(jkRaw + s->jkOffset_pm, 0, (int32_t)PACK_SOC_FULL_pm);

    /* Confidence decay: throughput (with coulombic efficiency) and time. */
    {
        const int32_t absI = (i < 0) ? -i : i;
        const int32_t eta  = (i > 0) ? (int32_t)PACK_SOC_LFP_ETA_CHG_pm
                                     : (int32_t)PACK_SOC_LFP_ETA_DIS_pm;

        s->qAbs_mAms += ((int64_t)absI * eta * (int64_t)dt_ms) / 1000;
        s->tLatch_ms += (int64_t)dt_ms;
    }
    decay = (s->qAbs_mAms * (int64_t)PACK_SOC_LFP_ALPHA_pm) /
            (3600000 * (int64_t)s->qNominal_mAh);
    decay += (s->tLatch_ms * (int64_t)PACK_SOC_LFP_BETA_ppm_h) / 3600000000LL;
    if (decay > 1000) {
        decay = 1000;
    }
    s->conf_pm = (uint16_t)Clamp32((int32_t)(1000 - decay),
                                   (int32_t)PACK_SOC_LFP_CONF_MIN_pm, 1000);

    /* Load compensation: V - I*R, charge positive. */
    vCompMin = (int32_t)in->cellMin_mV - (int32_t)(((int64_t)i * rMin) / 1000000);
    vCompMax = (int32_t)in->cellMax_mV - (int32_t)(((int64_t)i * rMax) / 1000000);
    s->vCompMin_mV = vCompMin;
    s->vCompMax_mV = vCompMax;

    wLower = Clamp32(((PACK_SOC_LFP_KNEE_LOW_mV - vCompMin) * 1000) /
                     (PACK_SOC_LFP_KNEE_LOW_mV - (int32_t)PACK_SOC_LFP_V_EMPTY_mV),
                     0, 1000);
    wUpper = Clamp32(((vCompMax - PACK_SOC_LFP_KNEE_HIGH_mV) * 1000) /
                     ((int32_t)PACK_SOC_LFP_V_FULL_mV - PACK_SOC_LFP_KNEE_HIGH_mV),
                     0, 1000);
    if (wLower >= wUpper) {
        wKnee = wLower;
        vKnee = vCompMin;
    } else {
        wKnee = wUpper;
        vKnee = vCompMax;
    }
    s->wLower_pm = (uint16_t)wLower;
    s->wUpper_pm = (uint16_t)wUpper;
    s->wKnee_pm  = (uint16_t)wKnee;
    s->socOcv_pm = (uint16_t)PackSoc_OcvToSoc_pm(
                       (uint16_t)Clamp32(vKnee, 0, 65535));

    /* Fusion, in ppm weights: Wc = Cc (1 - Wk), floor 1e-4 on the sum. */
    wCoul = (int64_t)s->conf_pm * (int64_t)(1000 - wKnee);
    denom = wCoul + (int64_t)wKnee * 1000;
    if (denom < 100) {
        denom = 100;
    }
    soc = (int32_t)((wCoul * (int64_t)jkCorr +
                     (int64_t)wKnee * 1000 * (int64_t)s->socOcv_pm) / denom);
    soc = Clamp32(soc, 0, (int32_t)PACK_SOC_FULL_pm);

    s->innovation_pm = (int16_t)(soc - jkCorr);
    s->socReal_pm    = (uint16_t)soc;
    s->haveEst       = 1u;
    LfpMapInverter(s);
}

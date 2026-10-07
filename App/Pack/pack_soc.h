/*
 * pack_soc.h
 *
 * The SOC estimator: coulomb counting across the plateau, OCV re-anchoring in
 * the knees, and a GRADED gate rather than a hard one
 * (docs/design_battery_pack.md §24).
 *
 * PURE, libc only, host-tested -- like pack_fsm.c and for the same reason:
 * this is the arithmetic that decides what the board reports about a
 * customer's battery, and it should be provable on a laptop rather than only
 * observable on a roof.
 *
 * WHY THIS EXISTS AT ALL.  The JK's own SOC is coulomb counting too, and its
 * arithmetic is fine.  What is wrong with it is WHERE IT RE-ANCHORS: it stops
 * integrating only outside [full/100, full x 99/100], so it corrects itself
 * at 1 % and 99 % -- and this pack, measured over a full solar day, reached
 * neither.  The informative part of the LFP curve starts at 20 % and 90 %,
 * which is 20x and 10x wider, and a graded gate widens it further still.
 */

#ifndef PACK_SOC_H_
#define PACK_SOC_H_

#include "App/Pack/pack.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants -------------------------------------------------------*/

/** Assumed cell-voltage noise, per sample.  Sets how fast an anchor's
 *  confidence improves with N; §10 item 7 of the estimation design measures
 *  it at about 3 mV. */
#define PACK_SOC_SIGMA_V_mV     3u

/** Current below which terminal voltage is treated as OCV, as a divisor of
 *  the pack's capacity: |I| < C/50.  Above it a sample needs IR correction --
 *  which the pack path now has (sPackSocRes), and the per-cell path still
 *  does not. */
#define PACK_SOC_REST_C_DIV     50u

/* --- IR-corrected anchoring (estimation design 5.3, second gate row) ------
 *
 * "Current small, OR IR-corrected."  A site whose baseline load never falls
 * below C/50 never anchors at all: zaliakalnis draws 11.6 A on a 261 Ah pack,
 * so its rest limit of 5.2 A is unreachable and its coulomb count free-ran to
 * 82 %% against cells sitting at 3235 mV, which the OCV table calls 17 %%.
 * Removing the IR term is what turns that site from never-anchoring into
 * anchoring continuously. */

/** Smallest |dI| between consecutive samples that carries resistance
 *  information, as a divisor of capacity, with an absolute floor applied by
 *  the caller.
 *
 *  SET FROM THE INSTRUMENTATION, not from taste.  A JK quantises its current
 *  at ~115 mA and its pack voltage at 1 mV, so one LSB over one LSB is
 *  ~8.7 mOhm: a step of 115 mA can only resolve R in 8.7 mOhm granules,
 *  which is not a measurement.  A step of ~1.3 A puts dV 15 LSBs clear of
 *  quantisation, which is.  (The first version of this used C/40 = 6.5 A on
 *  a 261 Ah pack -- larger than that site's entire load current, so it could
 *  never fit anything at all.) */
#define PACK_SOC_RSTEP_C_DIV    200u
#define PACK_SOC_RSTEP_MIN_mA   1000

/** Steps required before a fitted resistance is used for anything. */
#define PACK_SOC_R_MIN_STEPS    8u

/** Plausibility band on a fitted PACK resistance, micro-ohms.  Outside it the
 *  fit is arithmetic gone wrong -- a sign error, a re-seed step mistaken for
 *  a load step -- not a discovery about the battery.  A NEGATIVE fit lands
 *  outside by construction, which is the cheapest sign check available. */
#define PACK_SOC_R_MIN_uOhm     50u
#define PACK_SOC_R_MAX_uOhm     200000u

/** Accumulator ceiling, mA^2.  On reaching it both sums halve, which bounds
 *  the arithmetic AND makes the fit exponentially forgetful -- resistance
 *  rises over months and the estimate has to be able to follow it. */
#define PACK_SOC_R_SUM_MAX      1000000000000LL

/** Worst-case pack DC resistance, used ONLY as a bound when nothing has been
 *  measured yet.  Generous for a 16S pack of any size this board serves.
 *
 *  IT IS A BOUND, NEVER A CORRECTION.  Correcting by a guess would move the
 *  estimate by an amount nobody measured; charging the guess as UNCERTAINTY
 *  moves nothing and only decides how much a sample is allowed to weigh. */
#define PACK_SOC_R_BOUND_uOhm   20000u

/** Largest per-cell IR error, in mV, that a sample may carry and still
 *  anchor -- whether that error has been corrected or merely bounded.
 *
 *  THE BUDGET IS ON THE ERROR, NOT ON THE CURRENT, and the difference is the
 *  whole point.  C/50 is a proxy for "the IR error is negligible"; on a
 *  261 Ah pack it works out to ~1.6 mV/cell.  A site sitting at 6 A carries
 *  ~1.9 mV/cell -- 0.3 mV/cell worse -- and was refused every sample for it,
 *  while its coulomb count free-ran 65 PERCENTAGE POINTS away from the truth.
 *  Across the whole plausible range of R, from 5 to 20 mOhm, that site's IR
 *  error spans 0.38 %% to 1.50 %% SOC: the entire quantity being protected
 *  against is smaller than one part in forty of the error being tolerated.
 *
 *  15 mV/cell is about 3 %% SOC on the plateau -- large enough to admit any
 *  ordinary operating point, small enough that a mis-estimated R cannot move
 *  the anchor by more than the drift it is there to correct. */
#define PACK_SOC_IR_MAX_ERR_mV  15u

/** A CORRECTED sample carries a quarter of the correction as extra
 *  uncertainty: what a step fit measures is the ohmic part, and the slow
 *  polarisation it cannot see scales with the same current.  An UNCORRECTED
 *  sample carries the whole worst-case error instead -- it has not been
 *  corrected, so all of it is still there.  Either way big errors weigh
 *  almost nothing and small ones weigh nearly full, with no cliff between --
 *  the same graded treatment the OCV slope already gets. */
#define PACK_SOC_IR_SIGMA_DIV   4u

/** Current above which the cell is considered DISTURBED, as a divisor of
 *  capacity: |I| > C/20.  After that, terminal voltage needs time to relax
 *  back to OCV before it means anything. */
#define PACK_SOC_BUSY_C_DIV     20u

/** How long after a disturbance before a reading is OCV again.  LFP
 *  relaxation is slow, and a sample taken one second after a 30 A charge
 *  stops still carries most of the polarisation -- it reads HIGH, and an
 *  anchor built from it is confidently wrong in a fixed direction. */
#define PACK_SOC_RELAX_MS       600000u   /* 10 min */

/** Cell temperature band in decidegrees.  Both capacity and the OCV curve
 *  move with temperature; outside this the table is not the right table. */
#define PACK_SOC_TEMP_MIN_dC    100
#define PACK_SOC_TEMP_MAX_dC    350

/** SOC is carried in PER-MILLE, like every other ratio in this module. */
#define PACK_SOC_FULL_pm        1000u

/** Smallest SOC change between two anchors that yields a usable capacity.
 *
 *  C_i = dQ_i / dSOC_i, so the relative error is sigma(dSOC)/dSOC and with two
 *  anchors sigma(dSOC) = sigma*sqrt(2).  At sigma ~ 1 %% SOC, 10 %% capacity
 *  accuracy needs dSOC >= ~14 %%.  Below this the division amplifies noise
 *  instead of measuring anything. */
#define PACK_SOC_CAP_MIN_DELTA_pm   150

/** A measured capacity outside [nameplate/4, nameplate*2] is rejected as
 *  arithmetic gone wrong rather than adopted as a discovery. */
#define PACK_SOC_CAP_MIN_NUM        1u
#define PACK_SOC_CAP_MIN_DEN        4u
#define PACK_SOC_CAP_MAX_NUM        2u

/* Exported types -----------------------------------------------------------*/

/**
 * ONE ESTIMATOR UNIT -- a whole pack, or a single cell.  THE ARITHMETIC DOES
 * NOT CARE WHICH.
 *
 * There used to be two of these: a pack path and a cell path, with their own
 * anchor accumulators and their own SOC advance.  That is two copies of the
 * same equations, and two copies drift.  A cell and a pack differ only in
 * three arguments -- the voltage that anchors them, the capacity the charge
 * is a fraction of, and how much charge went through them -- so they are one
 * type with one set of functions.
 *
 * BALANCING IS NOT THIS TYPE'S BUSINESS.  The caller hands over the charge
 * that actually passed through THIS unit; for a pack that is the string
 * charge, for a cell it is the string charge plus whatever the balancer moved
 * into or out of it.  Correcting for the balancer before the hand-off is what
 * lets the same code serve both, and keeps a battery concept out of what is
 * otherwise pure arithmetic.
 */
typedef struct {
    /* the accumulating anchor: inverse-variance weighted, w = k^2 */
    int64_t  wSum;
    int32_t  w;
    uint16_t samples;

    /* the running estimate.  ORDERED WIDEST FIRST and narrowed where the
     * range allows: this struct exists once per cell per instance -- 16 x 8
     * of them -- so eight bytes of padding here is a kilobyte of main SRAM,
     * which on this part is the scarce region. */
    int32_t  capacity_mAh;      /* what soc_pm is a fraction of             */
    int32_t  chargeSinceAnchor_mAh;

    int16_t  soc_pm;            /* -1 = never anchored; else 0..1000        */
    int16_t  anchorSoc_pm;      /* SOC at the previous anchor               */
    int16_t  driftResidual_pm;  /* anchor MINUS prediction: the drift the
                                   coulomb count accumulated, and the only
                                   thing that bounds it.  A difference of
                                   two per-mille SOCs, so +/-1000           */
    uint16_t conf_pm;
    uint16_t capConf_pm;
    uint8_t  haveAnchor;
    uint8_t  capacityLearned;   /* 1 = measured, 0 = supplied at init       */
} sPackSocUnit;

/**
 * THE PACK'S DC RESISTANCE, fitted through the origin from current STEPS:
 *
 *      R = sum(dI * dV) / sum(dI^2)
 *
 * THIS IS THE POWER PATH -- busbars, terminals, cell internals -- the circuit
 * the pack current actually flows through, and therefore the only resistance
 * that can explain a load IR drop.
 *
 * IT IS NOT THE JK'S `CellWireRes`.  That array measures the sense/balance
 * HARNESS, which carries balance current and nothing else (estimation design
 * 5.4: "these are different circuits").  Its role in this estimator is a
 * VETO -- a cell being balanced through a degraded lead reads high and must
 * not supply an anchor -- never a correction term.  Subtracting I*R_wire from
 * a load IR drop would apply a wrong number with confidence, which is the one
 * failure mode the whole gate exists to prevent.
 *
 * No anchors are needed and none are consumed: a solar ESS supplies inverter
 * load steps for free, so this converges on ordinary operation within
 * minutes of a board starting.  Deliberately NOT persisted -- it re-learns
 * far faster than the learned capacities that share the state record, and
 * bumping that record's version to carry it would discard those.
 */
typedef struct {
    int64_t  sumIV;             /* sum(dI_mA * dV_mV)                       */
    int64_t  sumII;             /* sum(dI_mA^2)                             */
    int32_t  lastI_mA;
    int32_t  lastV_mV;
    uint32_t steps;             /* qualifying steps folded in               */
    uint8_t  haveLast;
} sPackSocRes;

/** The pack's estimator: one unit, plus the bookkeeping for reading the
 *  vendor's mAh counter -- which is a transport concern, not a SOC one --
 *  plus the resistance that lets it anchor while the pack is working. */
typedef struct {
    sPackSocUnit unit;
    sPackSocRes  res;
    int32_t      lastCounter_mAh;
    uint8_t      haveCounter;
} sPackSoc;

/* Exported functions -------------------------------------------------------*/

/**
 * @brief  Rest OCV -> SOC for one LFP cell.
 * @param  ocv_mV - open-circuit voltage of ONE cell
 * @retval SOC in per-mille, clamped to [0, 1000]
 * @note   Pure.  A pack anchors on its MEAN cell, which is the same curve.
 */
int32_t PackSoc_OcvToSoc_pm(uint16_t ocv_mV);

/**
 * @brief  Local slope of the OCV curve, in microvolts per per-mille SOC.
 * @retval slope; never zero, so a weight is always computable
 * @note   Pure.
 */
uint32_t PackSoc_Slope_uV_per_pm(uint16_t ocv_mV);

/* --- the unit: used identically for a pack and for a cell --------------- */

/**
 * @brief  Start a unit.
 * @param  u - the unit
 * @param  capacity_mAh - starting capacity; refined once measured
 * @note   Pure.
 */
void PackSoc_UnitInit(sPackSocUnit *u, uint32_t capacity_mAh);

/**
 * @brief  Fold one near-rest voltage into this unit's anchor.
 *
 * NO GATE.  The weight is k^2, so a plateau sample contributes ~1/400 of a
 * knee sample and the arithmetic decides.  A hard cut at 4 mV/% produced
 * ZERO anchors against 690 real samples from this pack (§24.3).
 *
 * @param  u - the unit
 * @param  ocv_mV - a cell's voltage, or a pack's mean cell voltage
 * @note   Pure.
 */
void PackSoc_UnitAnchorAdd(sPackSocUnit *u, uint16_t ocv_mV);

/**
 * @brief  Fold one voltage into the anchor at a STATED per-sample
 *         uncertainty, so a corrected sample can weigh less than a rest one.
 *
 * Weight is inverse-variance, w = (k/sigma)^2, so passing
 * PACK_SOC_SIGMA_V_mV reproduces PackSoc_UnitAnchorAdd exactly.
 *
 * @param  sigmaV_mV - this sample's voltage uncertainty; values below the
 *                     base are clamped up to it, since no sample is better
 *                     known than the ADC
 * @note   Pure.
 */
void PackSoc_UnitAnchorAddSigma(sPackSocUnit *u, uint16_t ocv_mV,
                                uint32_t sigmaV_mV);

/**
 * @brief  Resolve the accumulated anchor without consuming it.
 * @param  soc_pm_out - the weighted SOC estimate
 * @param  sigma_pm_out - its 1-sigma uncertainty
 * @retval 1 when an estimate could be formed, 0 when nothing accumulated
 * @note   Pure.
 */
int PackSoc_UnitAnchorResolve(const sPackSocUnit *u, int32_t *soc_pm_out,
                              uint32_t *sigma_pm_out);

/**
 * @brief  Advance this unit's SOC by the charge that passed THROUGH IT.
 *
 * @param  u - the unit
 * @param  dQ_mAh - charge through this unit.  FOR A CELL THE CALLER HAS
 *                  ALREADY ADDED THE BALANCE TRANSFER; this function knows
 *                  nothing about balancers.
 * @note   Pure.
 */
void PackSoc_UnitAdvance(sPackSocUnit *u, int32_t dQ_mAh);

/**
 * @brief  Apply a resolved anchor: measure capacity if possible, re-seed SOC,
 *         record the drift, and clear the accumulator.
 *
 * Capacity is measured as C = dQ / dSOC over the interval since the previous
 * anchor, subject to PACK_SOC_CAP_MIN_DELTA_pm and the plausibility band.
 * Identical for a pack and for a cell -- only the arguments differ.
 *
 * @param  u - the unit
 * @param  anchorSoc_pm - the weighted anchor
 * @param  sigma_pm - its uncertainty
 * @param  nameplate_mAh - plausibility reference; 0 disables the band
 * @retval 1 when a NEW capacity measurement was adopted, 0 otherwise
 * @note   Pure.
 */
int PackSoc_UnitApplyAnchor(sPackSocUnit *u, int32_t anchorSoc_pm,
                            uint32_t sigma_pm, uint32_t nameplate_mAh);

/**
 * @brief  This unit's SOC.
 * @param  conf_pm_out - optional confidence
 * @retval SOC per-mille, or -1 when never anchored
 * @note   Pure.
 */
int32_t PackSoc_UnitGet_pm(const sPackSocUnit *u, uint16_t *conf_pm_out);

/* --- the pack's vendor-counter bookkeeping ------------------------------ */

/**
 * @brief  Start the pack estimator.
 * @note   Pure.
 */
void PackSoc_Init(sPackSoc *s, uint32_t capacity_mAh);

/**
 * @brief  Take a reading of the vendor's mAh counter and report the charge
 *         it implies.
 *
 * A TRANSPORT CONCERN, NOT A SOC ONE, which is why it is separate from the
 * unit: it exists to turn a vendor register into a delta, and to REJECT the
 * steps that are not charge -- the counter is fenced at the ends of its range
 * and re-seeded from a voltage estimate whenever the BMS's configuration
 * changes.
 *
 * @param  s - pack state
 * @param  counter_mAh - the vendor's remaining-capacity counter
 * @param  maxStep_mAh - largest believable change between two samples
 * @param  dQ_mAh_out - the charge this reading implies; 0 when rejected
 * @retval 1 if the delta is real charge, 0 if it was a step or the first read
 * @note   Pure.
 */
int PackSoc_NoteCounter(sPackSoc *s, int32_t counter_mAh, uint32_t maxStep_mAh,
                        int32_t *dQ_mAh_out);

/* --- pack DC resistance, from current steps ------------------------------ */

/**
 * @brief  Start the resistance fit.
 * @note   Pure.
 */
void PackSoc_ResInit(sPackSocRes *r);

/**
 * @brief  Offer one (voltage, current) sample to the resistance fit.
 *
 * CALLED FOR EVERY SAMPLE, INCLUDING THE ONES THE ANCHOR GATE THROWS AWAY --
 * the steps live precisely where the pack is being worked, so a fit fed only
 * from near-rest samples would never see one.
 *
 * @param  v_mV       - pack terminal voltage
 * @param  i_mA       - pack current, POSITIVE INTO the pack
 * @param  minStep_mA - smallest |dI| that counts as a step
 * @note   Pure.
 */
void PackSoc_ResNote(sPackSocRes *r, int32_t v_mV, int32_t i_mA,
                     int32_t minStep_mA);

/**
 * @brief  The fitted resistance, if it is trustworthy yet.
 * @param  r_uOhm_out - the fit, micro-ohms
 * @retval 1 when enough steps have been seen AND the fit is inside the
 *         plausibility band, 0 otherwise -- callers must fall back to the
 *         rest path on 0, never to a guess
 * @note   Pure.
 */
int PackSoc_ResGet_uOhm(const sPackSocRes *r, uint32_t *r_uOhm_out);

/**
 * @brief  The weakest unit of @p n by measured capacity.
 * @param  cap_mAh_out - optional, that unit's capacity
 * @retval index, or -1 when none has a measured capacity yet
 * @note   Pure.  THE ANSWER THE WHOLE MODULE EXISTS FOR.
 */
int PackSoc_WeakestUnit(const sPackSocUnit *u, uint8_t n, int32_t *cap_mAh_out);


/* === The fused LFP estimator (the pack's published SOC) ====================
 *
 * Coulomb count (the JK's own SOC) fused with an OCV opinion, a latch engine
 * that re-anchors at the true ends, a confidence that DECAYS with throughput
 * and time, and temperature / load compensation.  It replaces "OCV anchor
 * overwrites the estimate" -- defect D2 of
 * docs/issue_soc_estimator_sodas_2026-10-06.md -- with a blend whose weights
 * the arithmetic decides, and it never integrates charge itself, so the
 * truncating integrator (D1) cannot reach it.  A vendor re-calibration step
 * (D3) is absorbed into an offset instead of being believed.
 *
 * All integer: per-mille, mV, mA, uOhm, decidegrees, milliseconds. */

/** Latch thresholds are derived from these, with a 3/4 headroom rule:
 *  high = full + 3/4 (overcharge - full), low = empty - 3/4 (empty - under). */
#define PACK_SOC_LFP_V_FULL_mV          3450u
#define PACK_SOC_LFP_V_EMPTY_mV         2900u
#define PACK_SOC_LFP_V_OVERCHG_mV       3600u
#define PACK_SOC_LFP_V_UNDERDIS_mV      2500u
/** Tail current for the average-cell high latch, per-mille of capacity (C). */
#define PACK_SOC_LFP_TAIL_C_pm          20u
/** Knee voltages, mV per cell (compensated). */
#define PACK_SOC_LFP_KNEE_LOW_mV        3200
#define PACK_SOC_LFP_KNEE_HIGH_mV       3380
/** Confidence decay: per-mille per 1.0C of throughput, ppm per hour, floor. */
#define PACK_SOC_LFP_ALPHA_pm           50u
#define PACK_SOC_LFP_BETA_ppm_h         1000u
#define PACK_SOC_LFP_CONF_MIN_pm        100u
/** Coulombic efficiency, per-mille. */
#define PACK_SOC_LFP_ETA_CHG_pm         992u
#define PACK_SOC_LFP_ETA_DIS_pm         1000u
/** Inverter display window, per-mille. */
#define PACK_SOC_LFP_DISP_MIN_pm        60u
#define PACK_SOC_LFP_DISP_MAX_pm        1000u
/** Per-cell resistance used ONLY when no pack resistance has been fitted yet:
 *  uOhm * Ah, so R_cell = this / capacity_Ah (~250 uOhm for a 280 Ah cell). */
#define PACK_SOC_LFP_R_UOHM_AH          70000u
/** A JK SOC step bigger than this beyond what the current explains is a
 *  vendor re-calibration, not charge (D3). */
#define PACK_SOC_LFP_JK_STEP_pm         50
/** Longest interval whose current is integrated as if it were constant. */
#define PACK_SOC_LFP_DT_MAX_ms          60000u

/** Which latch fired on the last update. */
typedef enum {
    packSocLatch_none = 0,
    packSocLatch_high,
    packSocLatch_low,
} ePackSocLatch;

/** One telemetry frame. */
typedef struct {
    uint16_t cellMin_mV;
    uint16_t cellMax_mV;
    uint16_t cellAvg_mV;
    int32_t  i_mA;              /* POSITIVE = charge                        */
    uint32_t rMin_uOhm;         /* per-cell resistance, lowest cell         */
    uint32_t rMax_uOhm;         /* per-cell resistance, highest cell        */
    uint8_t  rMeasured;         /* 1 = already at present temperature       */
    int16_t  tMin_dC;
    uint16_t socJk_pm;          /* the vendor's SOC                         */
} sPackSocLfpIn;

typedef struct {
    uint32_t qNominal_mAh;
    uint16_t vHigh_mV;          /* derived single-cell emergency latch      */
    uint16_t vLow_mV;
    uint16_t dispMin_pm;
    uint16_t dispMax_pm;

    /* persistent state */
    uint16_t conf_pm;           /* coulomb-count confidence, 100..1000      */
    int64_t  qAbs_mAms;         /* |I| * eta * dt since the last latch      */
    int64_t  tLatch_ms;
    int32_t  jkOffset_pm;       /* re-anchors the vendor SOC at a latch     */
    int32_t  lastJk_pm;
    uint8_t  haveJk;
    uint8_t  haveEst;

    /* results */
    uint16_t socReal_pm;
    uint16_t socInverter_pm;
    uint16_t socOcv_pm;
    uint16_t wKnee_pm;
    uint16_t wLower_pm;
    uint16_t wUpper_pm;
    int16_t  innovation_pm;     /* fused MINUS corrected vendor SOC (D4)    */
    int32_t  vCompMin_mV;
    int32_t  vCompMax_mV;
    uint32_t qEff_mAh;
    ePackSocLatch lastLatch;
    uint32_t latchHigh;
    uint32_t latchLow;
    uint32_t jkSteps;           /* vendor re-calibrations absorbed          */
} sPackSocLfp;

/** @brief Start the estimator.  @note Pure. */
void PackSocLfp_Init(sPackSocLfp *s, uint32_t qNominal_mAh);

/**
 * @brief  Fold one telemetry frame in.
 * @param  dt_ms - time since the previous frame (0 on the first)
 * @note   Pure.  Latches win the frame in which they fire.
 */
void PackSocLfp_Update(sPackSocLfp *s, const sPackSocLfpIn *in, uint32_t dt_ms);

/** @brief Per-cell resistance to use when none has been fitted, uOhm. */
uint32_t PackSocLfp_DefaultCellR_uOhm(uint32_t qNominal_mAh);

/** @brief Temperature multipliers, per-mille, by piecewise-linear lookup. */
int32_t PackSocLfp_Kq_pm(int16_t t_dC);
int32_t PackSocLfp_Kr_pm(int16_t t_dC);

#ifdef __cplusplus
}
#endif

#endif /* PACK_SOC_H_ */

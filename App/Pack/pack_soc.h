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
 *  the pack's capacity: |I| < C/50.  Above it a sample needs IR correction,
 *  which needs a per-cell resistance this board does not always have. */
#define PACK_SOC_REST_C_DIV     50u

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

/** The pack's estimator: one unit, plus the bookkeeping for reading the
 *  vendor's mAh counter -- which is a transport concern, not a SOC one. */
typedef struct {
    sPackSocUnit unit;
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

/**
 * @brief  The weakest unit of @p n by measured capacity.
 * @param  cap_mAh_out - optional, that unit's capacity
 * @retval index, or -1 when none has a measured capacity yet
 * @note   Pure.  THE ANSWER THE WHOLE MODULE EXISTS FOR.
 */
int PackSoc_WeakestUnit(const sPackSocUnit *u, uint8_t n, int32_t *cap_mAh_out);

#ifdef __cplusplus
}
#endif

#endif /* PACK_SOC_H_ */

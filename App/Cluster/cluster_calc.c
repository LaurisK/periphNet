/*
 * cluster_calc.c
 *
 * THE PURE CORE of the battery cluster module: the measured limit rule, its
 * low-load prediction, the input sanitiser, all aggregation and the divergence
 * detectors (docs/design_battery_cluster.md §3, revision 3).
 *
 * LIBC ONLY, ZERO FILE STATICS, NO Pack_ CALL, NO FLOAT.  Every working array
 * belongs to the caller, and SINCE THE RATE LIMITER WAS DELETED (§14.8)
 * NOTHING AT ALL IS CARRIED BETWEEN TICKS: `ClusterCalc_Solve` is a pure
 * function of (in, n, tune), with now_ms used only to stamp the snapshot.  A
 * host test asserts it by running the same inputs twice and comparing.
 *
 * FOUR ARITHMETIC RULES THIS FILE EXISTS TO GET RIGHT (§3.5):
 *
 *   1. NO DIVISION IS REACHABLE WITH A ZERO DENOMINATOR.  The board traps a
 *      divide by zero into a UsageFault, and `L_i == 0` is a value a real
 *      pack publishes.  Every denominator here is either proven non-zero by
 *      the branch above it or saturates instead of dividing.
 *   2. abs(INT32_MIN) IS UNDEFINED and sign-extends through a (uint64_t)
 *      cast.  AbsMa() is the one place a magnitude is taken.
 *   3. THE BUS SUM IS SIGNED and eight garbage int32 values overflow it.  It
 *      accumulates in int64_t.
 *   4. `|I| * 1000` OVERFLOWS uint32 above 4 294 967 mA, and a wrap there
 *      produces a SMALL number, and a small loadMax is a LARGE limit — it is
 *      the divisor.  The intermediate is uint64 and the result saturates at
 *      65535 only where it is narrowed for report.
 */

/* Includes -----------------------------------------------------------------*/

#include "App/Cluster/cluster_calc.h"

#include <stddef.h>
#include <string.h>

/* Private types ------------------------------------------------------------*/

/** One direction's slice of the carried state, so the charge and discharge
 *  passes are ONE piece of code seen twice.  Two copies would be two places
 *  for the gate to be got wrong. */
typedef struct {
    uint16_t  derate_pm;
    uint8_t   charge;           /* 1 = charge, 0 = discharge                 */
    uint8_t   bindFlag;         /* eClusterMemberFlag                        */
} sDirCtx;

/** What one direction pass produces.  Copied into the two symmetric halves of
 *  sClusterOutput by the caller, so neither half can quietly grow a field the
 *  other lacks. */
typedef struct {
    uint32_t target_mA;
    uint32_t derated_mA;
    uint32_t published_mA;
    uint32_t loadMax_pm;
    uint8_t  loopState;         /* eClusterLoopState                         */
    uint8_t  why;               /* eClusterLimitWhy                          */
    uint8_t  bindingIdx;
    uint8_t  allowed;
    uint8_t  partCount;
    uint8_t  predicted;
    uint8_t  ceiling;
} sDirOut;

/* Private function prototypes ----------------------------------------------*/

static uint32_t AbsMa(int32_t v);
static uint16_t Sat16(uint32_t v);
static uint32_t MulDiv1000(uint32_t v, uint32_t num);
static uint32_t PredictQuiet(const uint32_t *limit_mA, uint8_t cnt,
                             uint16_t decay_pm);
static void SolveDirection(const sClusterPackIn *in, uint8_t n,
                           const sClusterTune *tune, sClusterScratch *sc,
                           sClusterResult *out,
                           const sDirCtx *d, sDirOut *o);

/* Private functions --------------------------------------------------------*/

/**
 * @brief  Magnitude of a signed milliamp reading.
 * @note   -(int64_t)v, never -v: negating INT32_MIN in int32 is undefined
 *         behaviour, and (uint64_t)(-v) on a negative int sign-extends first.
 */
static uint32_t AbsMa(int32_t v)
{
    return (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
}

/**
 * @brief  Narrow for report, saturating.
 * @note   A PLAIN NARROWING SENDS 65536 TO EXACTLY 0, and zero is the value
 *         rule reads as "nothing is flowing" — so the most extreme overload
 *         the system can have would be indistinguishable from no current at
 *         all (defect L4).  load_pm exceeds 65535 above 65.5x a pack's own
 *         limit, which is inside the plausible domain.
 */
static uint16_t Sat16(uint32_t v)
{
    return (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;
}

/** @brief (v * num) / 1000 with a uint64 intermediate. */
static uint32_t MulDiv1000(uint32_t v, uint32_t num)
{
    return (uint32_t)(((uint64_t)v * (uint64_t)num) / 1000u);
}

/**
 * @brief  THE LOW-LOAD PREDICTION: d*L(1) + d^2*L(2) + ... over the
 *         participants' own limits, SMALLEST FIRST (design §3.2.2).
 *
 * A measurement needs current; below lowLoadFloor_pm there is none worth
 * dividing by, and the honest answer is a conservative guess rather than a
 * ratio of two noise figures.  SMALLEST FIRST is what makes it conservative:
 * the weakest pack is trusted most, and each further pack is discounted
 * again, so the sum approaches but never reaches sum(L_i) — this never assumes
 * N packs share evenly, which is the assumption that has no evidence at rest.
 *
 * A SINGLE PACK GETS d x L, WHICH IS EXACTLY WHAT THE MEASURED RULE GIVES IT
 * at any current (|S|/(|S|/L) == L).  So the one-pack site is continuous
 * across the floor and never steps when the sun comes out.
 *
 * @note  Two 32-byte stack arrays, not sClusterScratch: they die with the
 *        call, so they are not carried state and the "zero file statics"
 *        property is untouched.
 */
static uint32_t PredictQuiet(const uint32_t *limit_mA, uint8_t cnt,
                             uint16_t decay_pm)
{
    uint32_t sorted[CLUSTER_PACK_MAX];
    uint64_t acc = 0u;
    uint64_t w   = (uint64_t)decay_pm;
    uint8_t  i;
    uint8_t  j;

    if ((limit_mA == NULL) || (cnt == 0u) || (cnt > CLUSTER_PACK_MAX)) {
        return 0u;
    }
    for (i = 0u; i < cnt; i++) {
        sorted[i] = limit_mA[i];
    }
    /* Insertion sort, ASCENDING.  At most eight elements, so the simplest
     * correct sort is also the right one. */
    for (i = 1u; i < cnt; i++) {
        const uint32_t v = sorted[i];

        for (j = i; (j > 0u) && (sorted[j - 1u] > v); j--) {
            sorted[j] = sorted[j - 1u];
        }
        sorted[j] = v;
    }
    for (i = 0u; i < cnt; i++) {
        acc += ((uint64_t)sorted[i] * w) / 1000u;
        w    = (w * (uint64_t)decay_pm) / 1000u;
    }
    return (acc > (uint64_t)CLUSTER_LIMIT_MAX_MA) ? CLUSTER_LIMIT_MAX_MA
                                                  : (uint32_t)acc;
}

/* ==========================================================================
 * One direction — §3.1 through §3.7
 * ========================================================================== */

/* NO STATE AND NO CLOCK REACH HERE.  Revision 2 needed the loop variable, the
 * binding stamps and a time to age them; revision 3 needed the slew's carried
 * value and an interval.  Neither remains — this is a pure function of one
 * tick's packs and the tune, and there is nothing left for a future change to
 * quietly make stateful. */
static void SolveDirection(const sClusterPackIn *in, uint8_t n,
                           const sClusterTune *tune, sClusterScratch *sc,
                           sClusterResult *out,
                           const sDirCtx *d, sDirOut *o)
{
    uint8_t *const part       = d->charge ? sc->partChg : sc->partDsg;
    uint32_t       partL_mA[CLUSTER_PACK_MAX];
    uint64_t       absS_mA    = 0u;
    uint32_t       loadMax_pm = 0u;
    uint32_t       target;
    uint8_t        bindIdx    = CLUSTER_PACK_NONE;
    uint8_t        partCount  = 0u;
    uint8_t        anyClosed  = 0u;
    uint8_t        anyKnown   = 0u;
    uint8_t        alarmVeto  = 0u;
    uint8_t        i;

    (void)memset(o, 0, sizeof(*o));
    o->bindingIdx = CLUSTER_PACK_NONE;

    /* --- §3.1 the participating set ------------------------------------ */
    for (i = 0u; i < n; i++) {
        const sClusterMember *m = &out->member[i];
        const uint32_t L  = d->charge ? m->chargeLimit_mA
                                      : m->dischargeLimit_mA;
        const uint8_t  sw = d->charge ? in[i].chargeSwitch
                                      : in[i].dischargeSwitch;

        part[i] = 0u;
        if (m->state < (uint8_t)cluMember_present) {
            continue;                       /* not online, or not fresh      */
        }
        /* A switch a pack CAN report and reports as open is evidence; an
         * unknown switch counts as CLOSED for participation, or every pack of
         * a type that cannot report switches would vanish. */
        if (sw == (uint8_t)packSwitch_open) {
            continue;
        }
        if ((in[i].caps & (uint32_t)packCap_currentLimits) == 0u) {
            continue;                       /* its limits mean nothing       */
        }
        if (L == 0u) {
            continue;                       /* a zero limit IS an open path  */
        }
        part[i]             = 1u;
        partL_mA[partCount] = L;    /* PACKED, not slot-indexed: the
                                       prediction sorts them and does not care
                                       which slot each came from             */
        partCount++;
    }

    /* --- permission (§7.1) --------------------------------------------- */
    for (i = 0u; i < n; i++) {
        const sClusterMember *m = &out->member[i];
        const uint8_t  sw   = d->charge ? in[i].chargeSwitch
                                        : in[i].dischargeSwitch;
        const uint32_t veto = d->charge ? (uint32_t)packAlarm_cellOverVoltage
                                        : (uint32_t)packAlarm_cellUnderVoltage;

        if (m->state < (uint8_t)cluMember_present) {
            continue;
        }
        if (sw != (uint8_t)packSwitch_unknown) {
            anyKnown = 1u;
        }
        if ((sw == (uint8_t)packSwitch_closed) && (part[i] != 0u)) {
            anyClosed = 1u;
        }
        /* A SWITCH IS WEAKER EVIDENCE THAN AN ALARM.  A pack shouting cell
         * over-voltage stops the whole bus charging — unless it has ALREADY
         * isolated itself, in which case it does not need to stop the others.
         * packAlarm_protectionOpen forces nothing: removing the pack from
         * participation is the whole response to it. */
        if (((in[i].alarms & veto) != 0u) && (sw != (uint8_t)packSwitch_open)) {
            alarmVeto = 1u;
        }
    }
    if (alarmVeto != 0u) {
        o->allowed = 0u;
    } else if (anyClosed != 0u) {
        o->allowed = 1u;
    } else if (anyKnown != 0u) {
        o->allowed = 0u;
    } else {
        /* NOBODY CAN REPORT A SWITCH.  Granted iff a non-zero limit will be
         * published, which is exactly "there is a participant" — and phrasing
         * it that way rather than "the previously published limit is
         * non-zero" is what phrases the grant in terms of the bus rather than
         * of the last thing published. */
        o->allowed = (uint8_t)((partCount > 0u) ? 1u : 0u);
    }

    /* --- the measurement (§3.2), over packs FLOWING IN THIS DIRECTION ---
     *
     * A WIDER SET THAN THE PARTICIPANTS, deliberately: a pack that is online,
     * fresh and advertising limits but whose own limit has collapsed to zero
     * is OVER its limit by definition, and it is still bolted to the busbar.
     * Saturating its load at 1000 makes the rule reduce, which is correct;
     * excluding it would leave the rule blind to the one pack that most needs
     * it.  A pack WITHOUT packCap_currentLimits is excluded, because its
     * limit is unknown rather than zero. */
    for (i = 0u; i < n; i++) {
        sClusterMember *m = &out->member[i];
        const int32_t   I = m->current_mA;
        uint32_t        a;
        uint32_t        L;
        uint32_t        load_pm;

        if (m->state < (uint8_t)cluMember_present) {
            continue;
        }
        if ((in[i].caps & (uint32_t)packCap_currentLimits) == 0u) {
            continue;
        }
        if (d->charge ? (I <= 0) : (I >= 0)) {
            continue;                       /* not flowing this way          */
        }
        a = AbsMa(I);
        absS_mA += (uint64_t)a;
        L = d->charge ? m->chargeLimit_mA : m->dischargeLimit_mA;
        if (L == 0u) {
            load_pm   = 1000u;              /* SATURATE, never divide        */
            m->flags |= (uint32_t)cluMemFlag_limitSaturated;
        } else {
            /* ROUNDED UP, AND THIS IS THE SAFE DIRECTION, NOT A ROUNDING
             * PREFERENCE.  The rule DIVIDES BY loadMax, so a load truncated
             * DOWN yields a limit rounded UP — and the error is unbounded in
             * ratio exactly where load_pm is small.  Measured: a bus of 10 mA
             * against a 4.779 A pack gives a true load of 1.4 pm, truncates to
             * 1, and the answer leaps to 9.0 A against a 6.8 A ceiling — 32 %
             * over, with every other rule satisfied.  Rounding up is what
             * makes |I_i| <= (loadMax/1000) x L_i true for EVERY pack, and
             * that inequality summed over the bus is the proof that the
             * measured limit can never exceed sum(L_i). */
            load_pm = (uint32_t)((((uint64_t)a * 1000u) + (uint64_t)L - 1u) /
                                 (uint64_t)L);
        }
        m->load_pm     = Sat16(load_pm);
        if (load_pm > loadMax_pm) {
            loadMax_pm = load_pm;
            bindIdx    = i;
        }
    }
    o->loadMax_pm = loadMax_pm;

    /* --- §3.2 THE RULE — a pure function of THIS tick -------------------
     *
     * SCALE THE BUS UNTIL THE HARDEST-WORKING PACK REACHES ITS OWN LIMIT.
     * Every pack satisfies |I_i| <= f x L_i for f = loadMax/1000, so |S| / f
     * is the bus current at which the binding pack sits exactly at L_b — and
     * summing that inequality proves |S| / f <= sum(L_i), so the answer can
     * NEVER exceed the packs' combined rating.  That is a proof, not a clamp:
     * revision 2's upward step clamp bounded a recursion, and the recursion
     * is gone.
     *
     * NOTHING IS CARRIED AND THERE IS NO GATE.  Revision 2 measured the bus
     * against ITS OWN PUBLISHED VALUE, which made the rule self-referential:
     * the measurement was meaningful only at the limit, so it needed a gate,
     * the gate needed a settled publish, and a slew still ramping in
     * satisfied it for a reason that had nothing to do with the battery —
     * latching the loop 36x low on real hardware (§13.2).  A ratio of two
     * MEASURED currents is scale-invariant, so none of that arises: it is as
     * correct at 2 A as at 200 A, provided the shares are real.
     *
     * THE ONE ASSUMPTION: that the shares hold as the bus scales.  The closed
     * loop did not need it — it re-measured at the new limit — and the margin
     * is what pays for it.  This is the whole trade of revision 3, and it
     * buys an answer on a site that never saturates its battery, which is
     * every site we have (bindingSample stayed 0 for 12 h on board 1).
     *
     * BELOW lowLoadFloor_pm THE SHARES ARE NOT REAL.  The floor is compared
     * against loadMax — "is any pack working hard enough for its share to
     * mean anything" — and deliberately NOT against the published value,
     * because that comparison is exactly the self-reference §13.2 came
     * through. */
    if (partCount == 0u) {
        target        = 0u;
        o->loopState  = (uint8_t)cluLoop_idle;
    } else if ((loadMax_pm > 0u) &&
               (loadMax_pm >= (uint32_t)tune->lowLoadFloor_pm)) {
        /* loadMax_pm is rounded UP above, which is the safe direction here:
         * it is the DIVISOR, so a load rounded up yields a limit rounded
         * down. */
        target       = (uint32_t)((absS_mA * (uint64_t)tune->safetyMargin_pm) /
                                  (uint64_t)loadMax_pm);
        o->loopState = (uint8_t)cluLoop_measured;
        if (bindIdx < n) {
            o->bindingIdx               = bindIdx;
            out->member[bindIdx].flags |= (uint32_t)d->bindFlag;
        }
    } else {
        target       = PredictQuiet(partL_mA, partCount,
                                    tune->predictDecay_pm);
        o->predicted = 1u;
        o->loopState = (uint8_t)cluLoop_predicted;
    }

    /* --- the derate and the configured ceiling.  THAT IS THE WHOLE CHAIN ---
     *
     * THERE IS NO RATE LIMITER.  R2.5 required one until 2026-09-09, on three
     * arguments of which none survived scrutiny (requirements R2.5, design
     * §14.8).  The decisive one:
     *
     *   THIS IS A CAP, AND THE BATTERY WE EMULATE SWITCHES IT INSTANTLY.  The
     *   Dyness capture has `0x351`'s current fields jumping between discrete
     *   states between 250 ms frames — 39.76 A discharge to 11.20 A is a 3.5x
     *   step, shipped by a real battery into a working site.  Ramping a field
     *   the emulated device steps is a DEVIATION from the thing being
     *   emulated, not a safety margin over it.
     *
     *   AND A SUDDEN CHANGE OF SITUATION NEEDS A SUDDEN RESPONSE.  A pack
     *   dropping off the bus leaves the survivors carrying its share; if the
     *   cap does not collapse with it they are driven past their own limits
     *   and open one after another.  The old asymmetry (fall instantly, rise
     *   slowly) existed precisely to keep that edge safe — which is to say the
     *   rate limiter needed a special case to avoid causing the accident it
     *   was supposed to prevent.  With no rate limiter there is no asymmetry
     *   to get wrong: the cap is the present answer in both directions.
     *
     * Nothing protected by the slew is unprotected now: each pack's own BMS
     * limit and contactor bound its current, and §3.2.1 proves this value
     * never exceeds margin x sum(L_i). */
    o->target_mA  = target;
    o->derated_mA = MulDiv1000(target, (uint32_t)d->derate_pm);
    if (o->derated_mA > tune->limitMax_mA) {
        o->derated_mA = tune->limitMax_mA;
        o->ceiling    = 1u;
    }

    /* PERMISSION AND LIMIT AGREE, ALWAYS.  A refused direction publishes a
     * zero cap as well as a cleared permission flag; `derated_mA` still shows
     * what would have gone out, which is what an operator needs to tell "not
     * allowed" from "nothing to give". */
    o->published_mA = (o->allowed != 0u) ? o->derated_mA : 0u;
    o->partCount    = partCount;

    /* --- why, in the stated priority order ----------------------------- */
    {
        uint8_t why = (o->predicted != 0u) ? (uint8_t)cluLimitWhy_predicted
                                           : (uint8_t)cluLimitWhy_measured;

        if (o->ceiling != 0u) {
            why = (uint8_t)cluLimitWhy_ceiling;
        }
        /* THE TWO ZERO-LIMIT REASONS, AND WHICH ONE WINS.  §3.1 says an empty
         * P_d reports noParticipant; the priority list says forbidden
         * outranks it.  They meet when there is nobody to permit anything,
         * and there `forbidden` is a TAUTOLOGY rather than information: the
         * emitted limit is zero either way, so the reason that survives
         * should be the one that says WHY.  Hence forbidden outranks
         * noParticipant only when a participant actually exists to be
         * refused. */
        if (o->allowed == 0u) {
            why = (uint8_t)cluLimitWhy_forbidden;
        }
        if (partCount == 0u) {
            why = (uint8_t)cluLimitWhy_noParticipant;
        }
        o->why = why;
    }
}

/* Exported functions -------------------------------------------------------*/

int ClusterCalc_Solve(const sClusterPackIn *in, uint8_t n,
                      const sClusterTune *tune, sClusterScratch *sc,
                      uint32_t now_ms, sClusterResult *out)
{
    sDirCtx  dChg;
    sDirCtx  dDsg;
    sDirOut  oChg;
    sDirOut  oDsg;
    uint8_t  i;

    if ((tune == NULL) || (sc == NULL) || (out == NULL)) {
        return cluErr_badArg;
    }
    if ((n > CLUSTER_PACK_MAX) || ((in == NULL) && (n > 0u))) {
        return cluErr_badArg;
    }

    (void)memset(out, 0, sizeof(*out));
    (void)memset(sc, 0, sizeof(*sc));
    out->pub.tick_ms             = now_ms;
    out->pub.memberCnt           = n;
    out->pub.chargeBindingIdx    = CLUSTER_PACK_NONE;
    out->pub.dischargeBindingIdx = CLUSTER_PACK_NONE;
    for (i = 0u; i < CLUSTER_PACK_MAX; i++) {
        out->member[i].packIdx = CLUSTER_PACK_NONE;
    }

    /* ======================================================================
     * Pass 1 — sanitise and classify.  ONE PASS WITH PER-FIELD POLICY:
     * limits are CLAMPED DOWN, currents are DROPPED (clamping a current down
     * under-reports load, which is the anti-safe direction), and a capacity,
     * voltage or temperature outside the plausible domain is excluded from
     * its own aggregate with the field bit left clear.
     * ====================================================================== */
    for (i = 0u; i < n; i++) {
        const sClusterPackIn *p = &in[i];
        sClusterMember       *m = &out->member[i];

        m->packIdx    = p->packIdx;
        m->soc_pm     = p->soc_pm;
        m->elecAge_ms = p->elecAge_ms;

        if ((p->present == 0u) || (p->packIdx == CLUSTER_PACK_NONE)) {
            m->state = (uint8_t)cluMember_unresolved;
            m->why   = (uint8_t)cluWhy_nameUnresolved;
            out->pub.clusterAlarms |= (uint32_t)cluAlarm_nameUnresolved;
            continue;
        }
        if (p->cond != (uint8_t)packCond_online) {
            /* MEMBER LOST IS NOW A STATE, NOT AN EDGE.  Revision 2 raised it
             * from a participation transition, which needed a carried mask;
             * "a pack this cluster is configured for resolved to a real pack
             * that is not usable" says the same thing without carrying
             * anything, and keeps saying it while the condition lasts rather
             * than for the single tick it began. */
            m->state = (uint8_t)cluMember_absent;
            m->why   = (uint8_t)cluWhy_notOnline;
            out->pub.clusterAlarms |= (uint32_t)cluAlarm_memberLost;
            continue;
        }
        if (p->elecAge_ms > (uint32_t)tune->elecMaxAge_ms) {
            /* R3.3: judged on packGrp_electrical, the group the arithmetic
             * consumes.  A pack whose overall condition reads online on a
             * 15 s budget but whose electrical group is 362 s old is not one
             * this module may do arithmetic with. */
            m->state = (uint8_t)cluMember_stale;
            m->why   = (uint8_t)cluWhy_electricalStale;
            out->pub.clusterAlarms |= (uint32_t)cluAlarm_memberLost;
            continue;
        }
        if (AbsMa(p->current_mA) > (uint32_t)CLUSTER_PLAUS_CURRENT_MA) {
            m->state  = (uint8_t)cluMember_absent;
            m->why    = (uint8_t)cluWhy_implausible;
            m->flags |= (uint32_t)cluMemFlag_implausible;
            out->pub.clusterAlarms |= (uint32_t)cluAlarm_implausible;
            out->sanitised++;
            continue;
        }

        m->current_mA        = p->current_mA;
        m->chargeLimit_mA    = p->chargeLimit_mA;
        m->dischargeLimit_mA = p->dischargeLimit_mA;
        if (m->chargeLimit_mA > CLUSTER_PLAUS_LIMIT_MA) {
            m->chargeLimit_mA = CLUSTER_PLAUS_LIMIT_MA;
            out->sanitised++;
        }
        if (m->dischargeLimit_mA > CLUSTER_PLAUS_LIMIT_MA) {
            m->dischargeLimit_mA = CLUSTER_PLAUS_LIMIT_MA;
            out->sanitised++;
        }

        m->state = (uint8_t)cluMember_present;
        m->why   = (uint8_t)cluWhy_none;
        out->pub.onlineCnt++;
    }

    /* ======================================================================
     * Pass 2 — the two directions.  Independent by construction: charge and
     * discharge sharing differ, so each is solved on its own currents.
     * NEITHER CARRIES ANYTHING between ticks.
     * ====================================================================== */
    dChg.derate_pm = tune->chargeDerate_pm;
    dChg.charge    = 1u;
    dChg.bindFlag  = (uint8_t)cluMemFlag_bindingCharge;

    dDsg.derate_pm = tune->dischargeDerate_pm;
    dDsg.charge    = 0u;
    dDsg.bindFlag  = (uint8_t)cluMemFlag_bindingDischarge;

    SolveDirection(in, n, tune, sc, out, &dChg, &oChg);
    SolveDirection(in, n, tune, sc, out, &dDsg, &oDsg);

    /* Refine the member state now both directions have run: a pack that
     * participates in neither gets the REASON it did not. */
    for (i = 0u; i < n; i++) {
        sClusterMember *m = &out->member[i];

        if (m->state != (uint8_t)cluMember_present) {
            continue;
        }
        if ((sc->partChg[i] != 0u) || (sc->partDsg[i] != 0u)) {
            m->state = (uint8_t)cluMember_participating;
            continue;
        }
        if ((in[i].caps & (uint32_t)packCap_currentLimits) == 0u) {
            m->why = (uint8_t)cluWhy_noLimitCap;
        } else if ((in[i].chargeSwitch == (uint8_t)packSwitch_open) &&
                   (in[i].dischargeSwitch == (uint8_t)packSwitch_open)) {
            m->why = (uint8_t)cluWhy_switchesOpen;
        } else {
            m->why = (uint8_t)cluWhy_zeroLimits;
        }
    }

    out->pub.chargeTarget_mA     = oChg.target_mA;
    out->pub.chargeDerated_mA    = oChg.derated_mA;
    out->pub.chargeLimit_mA      = oChg.published_mA;
    out->pub.chargeLoadMax_pm    = Sat16(oChg.loadMax_pm);
    out->pub.chargeLoopState     = oChg.loopState;
    out->pub.chargeWhy           = oChg.why;
    out->pub.chargeBindingIdx    = oChg.bindingIdx;
    out->pub.chargeAllowed       = oChg.allowed;

    out->pub.dischargeTarget_mA  = oDsg.target_mA;
    out->pub.dischargeDerated_mA = oDsg.derated_mA;
    out->pub.dischargeLimit_mA   = oDsg.published_mA;
    out->pub.dischargeLoadMax_pm = Sat16(oDsg.loadMax_pm);
    out->pub.dischargeLoopState  = oDsg.loopState;
    out->pub.dischargeWhy        = oDsg.why;
    out->pub.dischargeBindingIdx = oDsg.bindingIdx;
    out->pub.dischargeAllowed    = oDsg.allowed;

    if (oChg.predicted != 0u)   { out->ev |= (uint32_t)cluCalcEv_predictedChg; }
    if (oDsg.predicted != 0u)   { out->ev |= (uint32_t)cluCalcEv_predictedDsg; }
    /* A GENUINE REFUSAL, not an empty bus: with no participant there is
     * nothing to refuse, and cluAlarm_allOffline / cluAlarm_noMembers already
     * say what is wrong.  Raising both would make "charging is forbidden" the
     * permanent state of every unprovisioned board. */
    if ((oChg.allowed == 0u) && (oChg.partCount > 0u)) {
        out->pub.clusterAlarms |= (uint32_t)cluAlarm_chargeForbidden;
    }
    if ((oDsg.allowed == 0u) && (oDsg.partCount > 0u)) {
        out->pub.clusterAlarms |= (uint32_t)cluAlarm_dischargeForbid;
    }
    if (oChg.partCount > 0u) {
        out->pub.fields |= (uint16_t)cluField_chargeLimit;
    }
    if (oDsg.partCount > 0u) {
        out->pub.fields |= (uint16_t)cluField_dischargeLimit;
    }

    /* ======================================================================
     * Pass 3 — everything else (§3.8).  EVERY DENOMINATOR IS TESTED, and on
     * zero the corresponding eClusterField bit is CLEARED and the value left
     * at 0: one mechanism, not four special cases.
     * ====================================================================== */
    {
        uint64_t sumRemaining = 0u, sumCapacity = 0u, sumNameplate = 0u;
        uint64_t sumVolt      = 0u;
        int64_t  sumCurrent   = 0;
        uint64_t sumAbsI      = 0u;
        uint32_t vMin = 0u, vMax = 0u;
        uint32_t cvl  = 0u, dvl  = 0u;
        int16_t  tMax = 0, tMin = 0;
        uint16_t socConf = 1000u, sohConf = 1000u;
        uint16_t socMin  = 0u, socMax = 0u;
        uint8_t  nVolt = 0u, nCap = 0u, nName = 0u;
        uint8_t  nCvl = 0u, nDvl = 0u;
        uint8_t  nTemp = 0u, nSwitch = 0u;

        for (i = 0u; i < n; i++) {
            const sClusterPackIn *p = &in[i];
            const sClusterMember *m = &out->member[i];

            if (m->state < (uint8_t)cluMember_present) {
                continue;
            }

            sumCurrent += (int64_t)m->current_mA;
            sumAbsI    += (uint64_t)AbsMa(m->current_mA);
            out->pub.alarms |= p->alarms;

            if ((p->voltage_mV > 0u) &&
                (p->voltage_mV <= CLUSTER_PLAUS_VOLTAGE_MV)) {
                sumVolt += (uint64_t)p->voltage_mV;
                if ((nVolt == 0u) || (p->voltage_mV < vMin)) {
                    vMin = p->voltage_mV;
                }
                if ((nVolt == 0u) || (p->voltage_mV > vMax)) {
                    vMax = p->voltage_mV;
                }
                nVolt++;
            }

            if (((p->caps & (uint32_t)packCap_capacityAh) != 0u) &&
                (p->capacity_mAh  <= CLUSTER_PLAUS_CAPACITY_MAH) &&
                (p->remaining_mAh <= CLUSTER_PLAUS_CAPACITY_MAH)) {
                sumRemaining += (uint64_t)p->remaining_mAh;
                sumCapacity  += (uint64_t)p->capacity_mAh;
                sumNameplate += (uint64_t)p->nameplate_mAh;
                if (p->nameplate_mAh > 0u) {
                    nName++;
                }
                if ((nCap == 0u) || (p->soc_pm < socMin)) { socMin = p->soc_pm; }
                if ((nCap == 0u) || (p->soc_pm > socMax)) { socMax = p->soc_pm; }
                nCap++;
                if (p->socConf_pm < socConf) { socConf = p->socConf_pm; }
                if (p->sohConf_pm < sohConf) { sohConf = p->sohConf_pm; }
            }

            /* CVL/DVL COVER EVERY ONLINE PACK, INCLUDING ONE THAT REFUSES TO
             * CHARGE.  A contactor opens to CURRENT, not to voltage — and a
             * pack at cell over-voltage drops out of P_chg at exactly the
             * moment its low ceiling matters most.  Taking these over
             * participants only would delete it there (§7.1). */
            /* A ZERO IS "NOT REPORTED", NOT "ZERO VOLTS", AND THE TWO ARE
             * COUNTED SEPARATELY.  The capability bit says a pack CAN report
             * these; it does not say it HAS.  MEASURED ON SODAS 2026-09-09:
             * `sodas2` advertised packCap_voltageLimits and returned
             * 0 mV / 0 mV for four minutes while `sodas15` returned
             * 55 200 / 43 200, so the min put a CVL of ZERO on the published
             * snapshot with the field marked VALID — which by contract 2 of
             * cluster.h is not "no limit" but an instruction to STOP
             * CHARGING.  §3.8 already refuses to INVENT this field when
             * nobody advertises it; this is the same scepticism applied to a
             * pack that advertises and then does not answer.
             *
             * SEPARATE COUNTERS because the two fields fail independently: a
             * pack may answer one register group and not the other, and a
             * zero DVL is harmless to a max() while a zero CVL captures a
             * min().  Folding them into one count made the healthy field
             * hostage to the broken one.
             *
             * The upper bound is the same plausibility domain the voltage
             * itself uses: a decode error yields a huge number, which a
             * max() would take. */
            if ((p->caps & (uint32_t)packCap_voltageLimits) != 0u) {
                if ((p->chargeVoltLimit_mV > 0u) &&
                    (p->chargeVoltLimit_mV <= CLUSTER_PLAUS_VOLTAGE_MV)) {
                    if ((nCvl == 0u) || (p->chargeVoltLimit_mV < cvl)) {
                        cvl = p->chargeVoltLimit_mV;
                    }
                    nCvl++;
                }
                if ((p->dischargeVoltLimit_mV > 0u) &&
                    (p->dischargeVoltLimit_mV <= CLUSTER_PLAUS_VOLTAGE_MV)) {
                    if ((nDvl == 0u) || (p->dischargeVoltLimit_mV > dvl)) {
                        dvl = p->dischargeVoltLimit_mV;
                    }
                    nDvl++;
                }
            }
            if (((p->caps & (uint32_t)packCap_temperatures) != 0u) &&
                (p->tempMax_dC <= CLUSTER_PLAUS_TEMP_MAX_DC) &&
                (p->tempMin_dC >= CLUSTER_PLAUS_TEMP_MIN_DC)) {
                if ((nTemp == 0u) || (p->tempMax_dC > tMax)) {
                    tMax = p->tempMax_dC;
                }
                if ((nTemp == 0u) || (p->tempMin_dC < tMin)) {
                    tMin = p->tempMin_dC;
                }
                nTemp++;
            }
            if ((p->caps & (uint32_t)packCap_switchState) != 0u) {
                nSwitch++;
            }
        }

        out->pub.current_mA = (int32_t)sumCurrent;
        out->pub.fields    |= (uint16_t)cluField_current;

        if (nVolt > 0u) {
            /* THE MEAN.  They are in parallel and the true bus voltage is one
             * number each pack measures with its own offset and lead drop;
             * min or max would bias every reading.  The disagreement is
             * REPORTED rather than hidden. */
            out->pub.voltage_mV    = (uint32_t)(sumVolt / (uint64_t)nVolt);
            out->pub.voltSpread_mV = Sat16(vMax - vMin);
            out->pub.fields       |= (uint16_t)cluField_voltage;
        }

        out->pub.remaining_mAh = (uint32_t)sumRemaining;
        out->pub.capacity_mAh  = (uint32_t)sumCapacity;
        out->pub.nameplate_mAh = (uint32_t)sumNameplate;

        /* THE MEAN OF SOCs IS WRONG whenever packs differ in size: 21 % and
         * 71 % of unequal packs is 47.3 %, not 46 %.  uint64, because
         * 8 000 000 mAh x 1000 is 8e9. */
        if (sumCapacity > 0u) {
            out->pub.soc_pm  = Sat16((uint32_t)((sumRemaining * 1000u) /
                                                sumCapacity));
            out->pub.fields |= (uint16_t)cluField_soc;
        }
        if ((sumNameplate > 0u) && (nName > 0u)) {
            uint32_t soh = (uint32_t)((sumCapacity * 1000u) / sumNameplate);

            if (soh > CLUSTER_SOH_MAX_PM) {
                soh = CLUSTER_SOH_MAX_PM;
            }
            out->pub.soh_pm  = (uint16_t)soh;
            out->pub.fields |= (uint16_t)cluField_soh;
        }
        if (nCap > 0u) {
            out->pub.socConf_pm = socConf;
            out->pub.sohConf_pm = sohConf;
            /* A CLUSTER THAT HAS NOT YET MEASURED ITS OWN CAPABILITY SHOULD
             * SAY SO — but ONLY WHERE THE PREDICTION IS ACTUALLY A GUESS.
             * With one online pack both rules give margin x L, so the
             * prediction is exact and capping would report a one-pack site as
             * permanently unsure for no reason.  With two or more the
             * prediction assumes a sharing it has not seen, and that IS
             * worth saying. */
            if ((out->pub.onlineCnt > 1u) &&
                ((out->pub.chargeLoopState == (uint8_t)cluLoop_predicted) ||
                 (out->pub.dischargeLoopState ==
                  (uint8_t)cluLoop_predicted))) {
                if (out->pub.socConf_pm > 300u) {
                    out->pub.socConf_pm = 300u;
                }
            }
        }

        /* §7.4: the cluster DECLARES the dependency and refuses to publish.
         * It does not invent the field and it does not publish zero — a zero
         * CVL is a valid, meaningful and catastrophic instruction.
         *
         * PER FIELD, not per pack.  Publishing the half that IS known while
         * declaring the half that is not is strictly better than withholding
         * both, and the alarm fires if EITHER is missing. */
        if (nCvl > 0u) {
            out->pub.chargeVoltLimit_mV = cvl;
            out->pub.fields |= (uint16_t)cluField_chargeVoltLimit;
        }
        if (nDvl > 0u) {
            out->pub.dischargeVoltLimit_mV = dvl;
            out->pub.fields |= (uint16_t)cluField_dischargeVoltLimit;
        }
        if (((nCvl == 0u) || (nDvl == 0u)) && (out->pub.onlineCnt > 0u)) {
            out->pub.clusterAlarms |= (uint32_t)cluAlarm_voltLimitMissing;
        } else {
            /* nothing online: a missing voltage limit is not news */
        }
        if (nTemp > 0u) {
            out->pub.tempMax_dC = tMax;
            out->pub.tempMin_dC = tMin;
            out->pub.fields    |= (uint16_t)cluField_temperature;
        }
        if (nSwitch > 0u) {
            out->pub.fields |= (uint16_t)cluField_switches;
        }

        /* --- divergence (R3.2): REPORTED, NEVER A CONTROL INPUT ---------
         * The control response to a greedy pack is ALREADY produced by the
         * limit rule; a second one would fight it. */
        if ((nCap >= 2u) &&
            ((uint32_t)(socMax - socMin) > (uint32_t)tune->socDiverge_pm)) {
            out->pub.clusterAlarms |= (uint32_t)cluAlarm_socDiverge;
            for (i = 0u; i < n; i++) {
                sClusterMember *m = &out->member[i];

                if (m->state < (uint8_t)cluMember_present) { continue; }
                if ((in[i].caps & (uint32_t)packCap_capacityAh) == 0u) {
                    continue;
                }
                if ((m->soc_pm == socMin) || (m->soc_pm == socMax)) {
                    m->flags |= (uint32_t)cluMemFlag_socOutlier;
                }
            }
        }
        if ((uint32_t)out->pub.voltSpread_mV > tune->voltDiverge_mV) {
            out->pub.clusterAlarms |= (uint32_t)cluAlarm_busSplit;
        }

        /* share_pm survives revision 2 as an OBSERVATION: the estimators are
         * gone, R3.2's requirement to REPORT share divergence is not. */
        if (sumAbsI > 0u) {
            const int busPositive = (sumCurrent > 0);
            uint16_t  worst    = 0u;
            uint8_t   worstIdx = CLUSTER_PACK_NONE;

            for (i = 0u; i < n; i++) {
                sClusterMember *m = &out->member[i];
                uint32_t        s;

                if (m->state < (uint8_t)cluMember_present) { continue; }
                s = (uint32_t)(((uint64_t)AbsMa(m->current_mA) * 1000u) /
                               sumAbsI);
                m->share_pm     = Sat16(s);
                if (m->share_pm > worst) {
                    worst    = m->share_pm;
                    worstIdx = i;
                }
                /* A pack pushing against the bus.  The field data makes this
                 * the NORMAL case at low current: -0.384 A against -2.738 A
                 * from packs six millivolts apart. */
                if ((m->current_mA != 0) && (sumCurrent != 0) &&
                    (((m->current_mA > 0) ? 1 : 0) !=
                     ((busPositive != 0) ? 1 : 0))) {
                    m->flags |= (uint32_t)cluMemFlag_circulating;
                    out->pub.clusterAlarms |= (uint32_t)cluAlarm_circulating;
                }
            }
            if ((out->pub.onlineCnt >= 2u) &&
                ((uint32_t)worst > (uint32_t)tune->shareDiverge_pm) &&
                (worstIdx < n)) {
                out->pub.clusterAlarms |= (uint32_t)cluAlarm_shareDiverge;
                out->member[worstIdx].flags |=
                    (uint32_t)cluMemFlag_shareOutlier;
            }
        }
    }

    /* ======================================================================
     * Pass 4 — the verdict
     * ====================================================================== */
    if (n == 0u) {
        out->pub.clusterAlarms |= (uint32_t)cluAlarm_noMembers;
        out->pub.cond = (uint8_t)cluCond_absent;
    } else if (out->pub.onlineCnt == 0u) {
        out->pub.clusterAlarms |= (uint32_t)cluAlarm_allOffline;
        out->pub.cond = (uint8_t)cluCond_absent;
    } else if (out->pub.onlineCnt < out->pub.memberCnt) {
        /* THE STATE HALF THE SITE'S ENERGY IS INVISIBLE IN. */
        out->pub.cond = (uint8_t)cluCond_degraded;
    } else {
        out->pub.cond = (uint8_t)cluCond_online;
    }
    out->pub.valid = (uint8_t)((out->pub.onlineCnt > 0u) ? 1u : 0u);

    return cluErr_ok;
}

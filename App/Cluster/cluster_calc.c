/*
 * cluster_calc.c
 *
 * THE PURE CORE of the battery cluster module: the closed-loop limit search
 * and its gate, the restart triggers, the input sanitiser, the slew, all
 * aggregation and the divergence detectors
 * (docs/design_battery_cluster.md §3).
 *
 * LIBC ONLY, ZERO FILE STATICS, NO Pack_ CALL, NO FLOAT.  Every working array
 * belongs to the caller; the only thing carried between ticks is
 * sClusterCalcState, and a host test asserts exactly that by memcpy'ing the
 * state, re-running and comparing.
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
 *      produces a SMALL number, which DISARMS the loop.  The intermediate is
 *      uint64 and the result saturates at 65535 only where it is narrowed for
 *      report.
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
    uint32_t *loop;             /* pre-derate, NEVER written to zero         */
    uint32_t *slewed;
    uint32_t *bindTick_ms;
    uint32_t *prevLimit_mA;     /* CLUSTER_PACK_MAX entries                  */
    uint32_t *prevPart;
    uint16_t *lastLoadMax_pm;
    uint8_t  *forbidden;
    uint8_t  *bindSeen;
    uint8_t  *slewing;
    uint16_t  derate_pm;
    uint8_t   charge;           /* 1 = charge, 0 = discharge                 */
    uint8_t   bindFlag;         /* eClusterMemberFlag                        */
} sDirCtx;

/** What one direction pass produces.  Copied into the two symmetric halves of
 *  sClusterOutput by the caller, so neither half can quietly grow a field the
 *  other lacks. */
typedef struct {
    uint32_t loop_mA;
    uint32_t derated_mA;
    uint32_t slewed_mA;
    uint32_t published_mA;
    uint32_t loadMax_pm;
    uint8_t  loopState;         /* eClusterLoopState                         */
    uint8_t  why;               /* eClusterLimitWhy                          */
    uint8_t  bindingIdx;
    uint8_t  allowed;
    uint8_t  restart;           /* eClusterRestart                           */
    uint8_t  partCount;
    uint8_t  binding;
    uint8_t  clamped;
    uint8_t  slewLimited;
    uint8_t  left;
    uint8_t  ceiling;
} sDirOut;

/* Private function prototypes ----------------------------------------------*/

static uint32_t AbsMa(int32_t v);
static uint16_t Sat16(uint32_t v);
static uint32_t MulDiv1000(uint32_t v, uint32_t num);
static void SolveDirection(const sClusterPackIn *in, uint8_t n,
                           const sClusterTune *tune, sClusterCalcState *st,
                           sClusterScratch *sc, sClusterResult *out,
                           uint32_t now_ms, uint32_t dt_ms,
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
 *         the loop reads as "no current, do not update" — so the most extreme
 *         overload the system can have would be indistinguishable from no
 *         current at all (defect L4).  load_pm exceeds 65535 above 65.5x a
 *         pack's own limit, which is inside the plausible domain.
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

/* ==========================================================================
 * One direction — §3.1 through §3.7
 * ========================================================================== */

static void SolveDirection(const sClusterPackIn *in, uint8_t n,
                           const sClusterTune *tune, sClusterCalcState *st,
                           sClusterScratch *sc, sClusterResult *out,
                           uint32_t now_ms, uint32_t dt_ms,
                           const sDirCtx *d, sDirOut *o)
{
    uint8_t *const part       = d->charge ? sc->partChg : sc->partDsg;
    uint32_t       partMask   = 0u;
    uint32_t       minL_mA    = 0u;
    uint64_t       absS_mA    = 0u;
    uint32_t       loadMax_pm = 0u;
    uint32_t       loopBefore;
    uint8_t        haveMin    = 0u;
    uint8_t        bindIdx    = CLUSTER_PACK_NONE;
    uint8_t        partCount  = 0u;
    uint8_t        anyClosed  = 0u;
    uint8_t        anyKnown   = 0u;
    uint8_t        alarmVeto  = 0u;
    uint8_t        fell       = 0u;
    uint8_t        join       = 0u;
    uint8_t        leave      = 0u;
    uint8_t        restart    = (uint8_t)cluRestart_none;
    uint8_t        i;

    (void)memset(o, 0, sizeof(*o));
    o->bindingIdx = CLUSTER_PACK_NONE;
    loopBefore    = *d->loop;

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
        part[i] = 1u;
        partMask |= (1u << i);
        partCount++;
        if ((haveMin == 0u) || (L < minL_mA)) {
            minL_mA = L;
            haveMin = 1u;
        }
    }

    /* --- permission (§7.1), decided BEFORE the loop so the forbidden ->
     *     allowed edge is available as a restart trigger ----------------- */
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
         * non-zero" is what stops a board whose slew starts at zero from
         * forbidding itself forever. */
        o->allowed = (uint8_t)((partCount > 0u) ? 1u : 0u);
    }

    /* --- the measurement (§3.2), over packs FLOWING IN THIS DIRECTION ---
     *
     * A WIDER SET THAN THE PARTICIPANTS, deliberately: a pack that is online,
     * fresh and advertising limits but whose own limit has collapsed to zero
     * is OVER its limit by definition, and it is still bolted to the busbar.
     * Saturating its load at 1000 makes the loop reduce, which is correct;
     * excluding it would leave the loop blind to the one pack that most needs
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
             * PREFERENCE.  The update DIVIDES BY loadMax, so a load truncated
             * DOWN yields a limit rounded UP — and the error is unbounded in
             * ratio exactly where load_pm is small.  Measured: a bus of 10 mA
             * against a 4.779 A pack gives a true load of 1.4 pm, truncates to
             * 1, and the loop leaps to 9.0 A against a 6.8 A ceiling — 32 %
             * over, with the gate open and every other rule satisfied.
             * Rounding up restores the proof exactly: loadMax >= 1000 I_b/L_b
             * gives loop' <= (loadTarget/bindFrac) x min(L_i/f_i), which at
             * the parse rule bindFrac >= loadTarget is min(L_i/f_i) itself. */
            load_pm = (uint32_t)((((uint64_t)a * 1000u) + (uint64_t)L - 1u) /
                                 (uint64_t)L);
        }
        sc->load_pm[i] = load_pm;
        m->load_pm     = Sat16(load_pm);
        if (load_pm > loadMax_pm) {
            loadMax_pm = load_pm;
            bindIdx    = i;
        }
    }
    o->loadMax_pm = loadMax_pm;

    /* --- §3.3 what restarts the search --------------------------------- */
    leave = (uint8_t)(((*d->prevPart & ~partMask) != 0u) ? 1u : 0u);
    join  = (uint8_t)(((partMask & ~*d->prevPart) != 0u) ? 1u : 0u);

    for (i = 0u; i < n; i++) {
        const uint32_t prev = d->prevLimit_mA[i];
        uint32_t       L;
        uint32_t       thr;

        if ((part[i] == 0u) || (prev == 0u)) {
            continue;
        }
        L   = d->charge ? out->member[i].chargeLimit_mA
                        : out->member[i].dischargeLimit_mA;
        thr = prev - MulDiv1000(prev, (uint32_t)tune->limitDeadband_pm);
        if (L < thr) {
            fell = 1u;
            out->member[i].flags |= (uint32_t)cluMemFlag_limitFell;
        }
    }
    for (i = 0u; i < n; i++) {
        if (((partMask & (1u << i)) != 0u) &&
            ((*d->prevPart & (1u << i)) == 0u)) {
            out->member[i].flags |= (uint32_t)cluMemFlag_joined;
        }
        if (((partMask & (1u << i)) == 0u) &&
            ((*d->prevPart & (1u << i)) != 0u)) {
            out->member[i].flags |= (uint32_t)cluMemFlag_left;
        }
    }

    if (st->started == 0u) {
        restart = (uint8_t)cluRestart_init;
    } else if (st->pendingRestart != (uint8_t)cluRestart_none) {
        restart = st->pendingRestart;
    } else if (leave != 0u) {
        restart = (uint8_t)cluRestart_memberLeft;
    } else if (fell != 0u) {
        restart = (uint8_t)cluRestart_limitFell;
    } else if (join != 0u) {
        restart = (uint8_t)cluRestart_memberJoined;
    } else if ((*d->forbidden != 0u) && (o->allowed != 0u)) {
        restart = (uint8_t)cluRestart_permitted;
    } else if ((*d->bindSeen != 0u) &&
               ((uint32_t)(now_ms - *d->bindTick_ms) > tune->holdMaxAge_ms)) {
        restart = (uint8_t)cluRestart_holdExpired;
    }

    if (restart != (uint8_t)cluRestart_none) {
        if (haveMin != 0u) {
            uint32_t seed = minL_mA;

            /* A LEAVE MAY LOWER THE LOOP AND MUST NEVER RAISE IT.  Removing a
             * pack can only RAISE min(L_i), and the departed pack is still
             * bolted to the busbar — so restarting to the new, higher floor
             * is the one unsafe direction of an otherwise safe rule
             * (defect L3).  An implausibility drop reaches here as a leave
             * and takes the same clamp. */
            if ((leave != 0u) && (loopBefore != 0u) && (seed > loopBefore)) {
                seed = loopBefore;
            }
            *d->loop = seed;
        }
        *d->bindSeen = 0u;
        /* THE DEADBAND REFERENCE IS LATCHED HERE AND ONLY HERE.  Against a
         * per-tick reference a pack stepping its limit down 1 % per tick — a
         * JK approaching full charge, or in the cold — never trips at all,
         * because a deadband suppresses NOISE and a monotonic ramp is not
         * noise. */
        for (i = 0u; i < CLUSTER_PACK_MAX; i++) {
            d->prevLimit_mA[i] = ((i < n) && (part[i] != 0u))
                               ? (d->charge ? out->member[i].chargeLimit_mA
                                            : out->member[i].dischargeLimit_mA)
                               : 0u;
        }
    }
    o->restart = restart;
    o->left    = leave;

    /* --- §3.2.1 THE GATE, then the update ------------------------------
     *
     * THE UPDATE IS ONLY MEANINGFUL AT THE LIMIT.  When the inverter is
     * loafing, loadMax says nothing about what would happen at the limit and
     * the update is nonsense — 100 A published, 10 A drawn, and the rule
     * computes 1125 A, which then LATCHES because at 1125 A the bus really
     * does load the worst pack to exactly the setpoint.  A wrong answer that
     * looks converged is worse than one that oscillates.
     *
     * THE MEASUREMENT IS TAKEN AT THE EMITTED (post-derate, post-slew) VALUE.
     * That is what folds the derate into the setpoint: the loop variable
     * settles at loadTarget x min(L_i/f_i) and the emitted value at
     * loadTarget x derate x min(L_i/f_i), both at or below the binding pack's
     * own limit for EVERY derate (defect L1). */
    /* AND NOT WHILE THE SLEW WAS STILL CLIMBING TOWARD THE LOOP.  A value the
     * rate limiter is still moving is not a limit the inverter is respecting
     * — it is a number this board is walking upward through the bus current —
     * so `|S| >= published x bindFrac` is satisfied for a reason that has
     * nothing to do with the battery being at its limit, and the update
     * ratchets the loop DOWN to wherever the ramp happened to cross the load.
     *
     * IT IS A TRAP, NOT A TRANSIENT, because it then latches: the loop lands
     * low, the emitted value follows it, and the gate can never re-open —
     * reopening needs a bus draw at 90 % of a limit the loop has just made
     * too small to reach.  MEASURED ON BOARD 1 (Pd1.1.50, 2026-09-07): a
     * 150 A pack ramping in against a steady 1.007 A charge settled at a
     * published limit of 4.1 A, `why: notBinding`, and stayed there — a 36x
     * throughput loss in the safe direction.
     *
     * The condition is LAST tick's, because the current this tick measures
     * was drawn against last tick's emitted value.  Skipping an update is
     * unconditionally safe: it can only leave a limit lower than it might
     * have been.  Downward corrections are untouched — a fall is instant and
     * is never slew-limited. */
    if ((partCount > 0u) && (loadMax_pm > 0u) && (*d->slewed > 0u) &&
        (*d->slewing == 0u) &&
        ((absS_mA * 1000u) >=
         ((uint64_t)*d->slewed * (uint64_t)tune->bindFrac_pm))) {
        uint32_t target = (uint32_t)(((uint64_t)*d->slewed *
                                      (uint64_t)tune->loadTarget_pm) /
                                     (uint64_t)loadMax_pm);

        /* CLAMPED UPWARD ONLY.  A reduction is the safety action, so bounding
         * it bounds the safety action; garbage is stopped by the plausibility
         * pass, where it enters. */
        if (*d->loop != 0u) {
            const uint64_t cap = ((uint64_t)*d->loop *
                                  (uint64_t)CLUSTER_STEP_UP_MAX_PM) / 1000u;

            if ((uint64_t)target > cap) {
                target     = (uint32_t)cap;
                o->clamped = 1u;
            }
        }
        /* NEVER ZERO: loop' = published x t / loadMax is multiplicative, so
         * zero is an absorbing state the loop could not leave. */
        if (target == 0u) {
            target = 1u;
        }
        *d->loop        = target;
        *d->bindSeen    = 1u;
        *d->bindTick_ms = now_ms;
        o->binding      = 1u;
        o->bindingIdx   = bindIdx;
        if (bindIdx < n) {
            out->member[bindIdx].flags |= (uint32_t)d->bindFlag;
        }
    }

    if (partCount == 0u) {
        o->loopState = (uint8_t)cluLoop_idle;
    } else if (*d->bindSeen == 0u) {
        o->loopState = (uint8_t)cluLoop_searching;
    } else if (o->binding == 0u) {
        o->loopState = (uint8_t)cluLoop_holding;
    } else {
        const uint32_t now  = *d->loop;
        const uint32_t was  = loopBefore;
        const uint32_t diff = (now > was) ? (now - was) : (was - now);

        o->loopState = (((uint64_t)diff * 1000u) <=
                        ((uint64_t)was * (uint64_t)tune->convergeTol_pm))
                     ? (uint8_t)cluLoop_converged
                     : (uint8_t)cluLoop_searching;
    }

    /* --- the derate, the configured ceiling, then §3.7's rate limiter --- */
    o->loop_mA    = *d->loop;
    o->derated_mA = MulDiv1000(*d->loop, (uint32_t)d->derate_pm);
    if (o->derated_mA > tune->limitMax_mA) {
        o->derated_mA = tune->limitMax_mA;
        o->ceiling    = 1u;
    }

    {
        const uint32_t target = (partCount == 0u) ? 0u : o->derated_mA;

        if (target <= *d->slewed) {
            *d->slewed = target;                    /* falls INSTANTLY       */
        } else {
            uint32_t step = (uint32_t)(((uint64_t)tune->riseRate_mA_per_s *
                                        (uint64_t)dt_ms) / 1000u);

            /* The floor exists ONLY so a slow rate does not stall on integer
             * truncation of (rate * dt)/1000. */
            if (step < CLUSTER_RISE_STEP_MIN_MA) {
                step = CLUSTER_RISE_STEP_MIN_MA;
            }
            if (target < (*d->slewed + step)) {
                *d->slewed = target;
            } else {
                *d->slewed    += step;
                o->slewLimited = 1u;
            }
            /* A rise the limiter had to bound means the emitted value is
             * BELOW what the loop is asking for, so the next tick must not
             * learn from it. */
        }
    }
    o->slewed_mA = *d->slewed;

    /* PERMISSION AND LIMIT AGREE, ALWAYS.  The zeroing applies to the EMITTED
     * value and never to the loop: a transient alarm must not destroy what the
     * loop learned. */
    *d->slewing        = o->slewLimited;
    o->published_mA    = (o->allowed != 0u) ? *d->slewed : 0u;
    *d->forbidden      = (uint8_t)((o->allowed != 0u) ? 0u : 1u);
    o->partCount       = partCount;
    *d->lastLoadMax_pm = Sat16(loadMax_pm);
    *d->prevPart       = partMask;

    /* --- why, in the stated priority order ----------------------------- */
    {
        uint8_t why = (uint8_t)cluLimitWhy_start;

        if (o->binding != 0u) {
            why = (uint8_t)cluLimitWhy_binding;
        } else if (*d->bindSeen != 0u) {
            why = (uint8_t)cluLimitWhy_notBinding;
        }
        if (o->slewLimited != 0u) {
            why = (uint8_t)cluLimitWhy_slew;
        }
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

void ClusterCalc_Reset(sClusterCalcState *st, uint8_t reason)
{
    if (st == NULL) {
        return;
    }
    /* THE WHOLE STATE, not merely the slew: every value in it is slot-indexed
     * and a re-pointed slot makes all of them meaningless. */
    (void)memset(st, 0, sizeof(*st));
    st->pendingRestart = reason;
}

int ClusterCalc_Solve(const sClusterPackIn *in, uint8_t n,
                      const sClusterTune *tune, sClusterCalcState *st,
                      sClusterScratch *sc, uint32_t now_ms,
                      sClusterResult *out)
{
    sDirCtx  dChg;
    sDirCtx  dDsg;
    sDirOut  oChg;
    sDirOut  oDsg;
    uint32_t dt_ms;
    uint8_t  i;

    if ((tune == NULL) || (st == NULL) || (sc == NULL) || (out == NULL)) {
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

    dt_ms = (st->started != 0u) ? (uint32_t)(now_ms - st->lastTick_ms) : 0u;
    if (dt_ms > CLUSTER_DT_MAX_MS) {
        dt_ms = CLUSTER_DT_MAX_MS;      /* a debugger halt is not a step     */
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
            m->state = (uint8_t)cluMember_absent;
            m->why   = (uint8_t)cluWhy_notOnline;
            continue;
        }
        if (p->elecAge_ms > (uint32_t)tune->elecMaxAge_ms) {
            /* R3.3: judged on packGrp_electrical, the group the arithmetic
             * consumes.  A pack whose overall condition reads online on a
             * 15 s budget but whose electrical group is 362 s old is not one
             * this module may do arithmetic with. */
            m->state = (uint8_t)cluMember_stale;
            m->why   = (uint8_t)cluWhy_electricalStale;
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
     * Pass 2 — the two loops.  Independent by construction: charge and
     * discharge sharing differ, so each keeps its own learned value.
     * ====================================================================== */
    dChg.loop           = &st->loopCharge_mA;
    dChg.slewed         = &st->pubCharge_mA;
    dChg.bindTick_ms    = &st->bindChgTick_ms;
    dChg.prevLimit_mA   = st->prevLimitChg_mA;
    dChg.prevPart       = &st->prevPartChg;
    dChg.lastLoadMax_pm = &st->lastLoadMaxChg_pm;
    dChg.forbidden      = &st->chgForbidden;
    dChg.bindSeen       = &st->chgBindSeen;
    dChg.slewing        = &st->chgSlewing;
    dChg.derate_pm      = tune->chargeDerate_pm;
    dChg.charge         = 1u;
    dChg.bindFlag       = (uint8_t)cluMemFlag_bindingCharge;

    dDsg.loop           = &st->loopDischarge_mA;
    dDsg.slewed         = &st->pubDischarge_mA;
    dDsg.bindTick_ms    = &st->bindDsgTick_ms;
    dDsg.prevLimit_mA   = st->prevLimitDsg_mA;
    dDsg.prevPart       = &st->prevPartDsg;
    dDsg.lastLoadMax_pm = &st->lastLoadMaxDsg_pm;
    dDsg.forbidden      = &st->dsgForbidden;
    dDsg.bindSeen       = &st->dsgBindSeen;
    dDsg.slewing        = &st->dsgSlewing;
    dDsg.derate_pm      = tune->dischargeDerate_pm;
    dDsg.charge         = 0u;
    dDsg.bindFlag       = (uint8_t)cluMemFlag_bindingDischarge;

    SolveDirection(in, n, tune, st, sc, out, now_ms, dt_ms, &dChg, &oChg);
    SolveDirection(in, n, tune, st, sc, out, now_ms, dt_ms, &dDsg, &oDsg);

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

    out->pub.chargeLoop_mA       = oChg.loop_mA;
    out->pub.chargeDerated_mA    = oChg.derated_mA;
    out->pub.chargeSlewed_mA     = oChg.slewed_mA;
    out->pub.chargeLimit_mA      = oChg.published_mA;
    out->pub.chargeLoadMax_pm    = Sat16(oChg.loadMax_pm);
    out->pub.chargeLoopState     = oChg.loopState;
    out->pub.chargeWhy           = oChg.why;
    out->pub.chargeBindingIdx    = oChg.bindingIdx;
    out->pub.chargeAllowed       = oChg.allowed;

    out->pub.dischargeLoop_mA    = oDsg.loop_mA;
    out->pub.dischargeDerated_mA = oDsg.derated_mA;
    out->pub.dischargeSlewed_mA  = oDsg.slewed_mA;
    out->pub.dischargeLimit_mA   = oDsg.published_mA;
    out->pub.dischargeLoadMax_pm = Sat16(oDsg.loadMax_pm);
    out->pub.dischargeLoopState  = oDsg.loopState;
    out->pub.dischargeWhy        = oDsg.why;
    out->pub.dischargeBindingIdx = oDsg.bindingIdx;
    out->pub.dischargeAllowed    = oDsg.allowed;

    /* The more significant of the two, so an operator sees the event rather
     * than whichever direction happened to run second. */
    out->pub.lastRestart = (oChg.restart != (uint8_t)cluRestart_none)
                         ? oChg.restart : oDsg.restart;

    if (oChg.clamped != 0u)     { out->ev |= (uint32_t)cluCalcEv_stepClampedChg; }
    if (oDsg.clamped != 0u)     { out->ev |= (uint32_t)cluCalcEv_stepClampedDsg; }
    if (oChg.slewLimited != 0u) { out->ev |= (uint32_t)cluCalcEv_slewChg; }
    if (oDsg.slewLimited != 0u) { out->ev |= (uint32_t)cluCalcEv_slewDsg; }
    if (oChg.binding != 0u)     { out->ev |= (uint32_t)cluCalcEv_bindingChg; }
    if (oDsg.binding != 0u)     { out->ev |= (uint32_t)cluCalcEv_bindingDsg; }
    if (oChg.restart != (uint8_t)cluRestart_none) {
        out->ev |= (uint32_t)cluCalcEv_restartChg;
    }
    if (oDsg.restart != (uint8_t)cluRestart_none) {
        out->ev |= (uint32_t)cluCalcEv_restartDsg;
    }
    if ((oChg.left != 0u) || (oDsg.left != 0u)) {
        out->pub.clusterAlarms |= (uint32_t)cluAlarm_memberLost;
    }
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
        uint8_t  nVolt = 0u, nCap = 0u, nName = 0u, nVoltLim = 0u;
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
            if ((p->caps & (uint32_t)packCap_voltageLimits) != 0u) {
                if ((nVoltLim == 0u) || (p->chargeVoltLimit_mV < cvl)) {
                    cvl = p->chargeVoltLimit_mV;
                }
                if ((nVoltLim == 0u) || (p->dischargeVoltLimit_mV > dvl)) {
                    dvl = p->dischargeVoltLimit_mV;
                }
                nVoltLim++;
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
             * SAY SO. */
            if ((out->pub.chargeLoopState == (uint8_t)cluLoop_searching) ||
                (out->pub.dischargeLoopState == (uint8_t)cluLoop_searching)) {
                if (out->pub.socConf_pm > 300u) {
                    out->pub.socConf_pm = 300u;
                }
            }
        }

        /* §7.4: the cluster DECLARES the dependency and refuses to publish.
         * It does not invent the field and it does not publish zero — a zero
         * CVL is a valid, meaningful and catastrophic instruction. */
        if (nVoltLim > 0u) {
            out->pub.chargeVoltLimit_mV    = cvl;
            out->pub.dischargeVoltLimit_mV = dvl;
            out->pub.fields |= (uint16_t)cluField_chargeVoltLimit |
                               (uint16_t)cluField_dischargeVoltLimit;
        } else if (out->pub.onlineCnt > 0u) {
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
         * loop; a second one would fight it. */
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
                sc->share_pm[i] = m->share_pm;
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

    st->lastTick_ms    = now_ms;
    st->started        = 1u;
    st->pendingRestart = (uint8_t)cluRestart_none;
    return cluErr_ok;
}

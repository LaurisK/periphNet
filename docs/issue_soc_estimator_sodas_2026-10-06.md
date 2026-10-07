# Issue: the pack SOC estimator reads tens of points high while charging, and its coulomb count has never moved

**Status:** **OPEN. Diagnosed, nothing fixed.** No firmware, configuration or
board state was changed during the investigation; the board is still on
`Pd1.1.59` and still feeds the inverter the estimator's number (§2).
**Observed:** 2026-10-06 by the operator on sodas (`sodas15`, JK slave 15,
660 Ah nameplate), then measured live over the tunnel the same afternoon.
**Report:** at 11:40 the pack card read **SOC 81 %, confidence 10 %** with the
JK's own counter at **150 of 660 Ah** (22.7 %), the pack charging at ~0.15 C and
~3.35 V per cell. By 15:22 it read **94.7 %** with the counter at **503 of
660 Ah** (76 %). The operator's own judgement of the first reading was "between
15 % and 35 %".
**Related:** [design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md)
§4-§5.3, [design_battery_pack.md](design_battery_pack.md) §24,
[review_zaliakalnis_soc_estimation_2026-09-08.md](review_zaliakalnis_soc_estimation_2026-09-08.md)
§4, §6, §8 (same estimator, same family of defects, different site).
**Evidence:** [`configs/celllogs/sodas15_charge_2026-10-06.tsv`](../configs/celllogs/sodas15_charge_2026-10-06.tsv)
(150 rows, 20 s cadence, 15:24:20-16:14:03, which spans the top knee and a
reboot).

## 0. Summary

The operator's reading was right and the board's was wrong. This is **not** the
slow accumulation of coulomb-counting error that "drift" suggests. Three
independent defects stack:

| | Defect | Where | Effect |
|---|---|---|---|
| **D1** | The coulomb integrator **truncates every step to zero** on a pack this size | `PackSoc_UnitAdvance`, `pack_soc.c:204-228` | The estimate is *only* the OCV anchor, re-seeded every ~6 min. Nothing integrates |
| **D2** | The OCV anchor **overwrites** the estimate whatever its confidence, and its confidence **leaves out the dominant error** | `PackSoc_UnitApplyAnchor`; `pack.c:546-553`; `pack.c:2040-2066` | +~50 pp wrong at 11:40, reported as "10 % sure" and *used anyway* |
| **D3** | A **vendor re-calibration step is consumed as real charge** | `PackSoc_NoteCounter` with `cap / 8u`, `pack.c:1881-1882` | 16:08: a 68 Ah JK jump drove the estimate to 100 % and onto the wire |
| D4 | `socDrift_pm` and `socConf_pm` cannot show any of this | `pack.c:548`, `pack_soc.c` conf | The diagnostics read healthy (drift 1-3 pm, conf 62 %) throughout |

The inverter is told the estimator's number: BatComm is configured
`source: pack` (§2). Fix order and design are in §6.

## 1. What was measured

All figures `sodas15`, sampled over the tunnel, read-only GETs on port 80.

### 1.1 The three numbers on one pack

| Time | Estimator `soc_pm` (conf) | JK counter (`remaining / 660 Ah`) | Current | Mean cell | Likely truth (§4) |
|---|---|---|---|---|---|
| 11:40 (operator) | **81 %** (10 %) | 150 Ah = **22.7 %** | ~+99 A | ~3.35 V | **~28-33 %** |
| 15:24 | **94.8 %** (62 %) | 508 Ah = **77.0 %** | +99 A | 3.393 V | ~82-87 % |
| 16:07:40 | **95.8 %** (87 %) | 592 Ah = **89.7 %** | +102 A | 3.413 V | ~95-99 % |
| 16:08:00 | **100 %** | **660 Ah = 100 %** (+67.8 Ah in one 20 s window) | +109 A | 3.415 V (cell max 3.449) | - |
| 18:32 (rest) | **89.5 %** (57 %) | 659.9 Ah = **100 %** | -0.7 A | 3.338 V | ~93-97 % |

Over 15:24:20-16:07:40 the JK counter rose **84.0 Ah (12.7 pp)** and the board's
own integral of the measured current over the same samples is **84.1 Ah**. The
counter's *increments* are sound; only its absolute offset is in question.

### 1.2 Earlier captures show the same split

| Capture | State | Estimator | JK counter |
|---|---|---|---|
| 2026-10-03 14:58 (`sodas15`) | +50 A, cells 3379-3385 | 95.0 % (conf 93 %) | 79.5 % |
| 2026-10-04 20:19 (`sodas15`) | -1.5 A, cells 3291-3293 | 51.6 % (conf 12 %) | 32.2 % |
| 2026-09-09 12:38 (`sodas2` / `sodas15`, one bus, same cell voltage) | -25 A, cells ~3.27 V | 39.1 % / 39.6 % | **57.1 % / 78.4 %** |

The estimator sat 15-20 pp **above** the counter in the two sodas15 captures. The
09-09 pair is the reminder that **the JK counter is not ground truth either**:
two packs on one bus, at the same voltage, 21 pp apart.

## 2. What reaches whom

- **Estimator** (`pack.soc_pm`, flag `socEstimated`): OCV table applied to
  `V_pack/16 - I*R_ohmic/16`, anchored every `PACK_SOC_ANCHOR_EVERY` (120)
  admitted samples (~6 min on sodas).
- **JK counter** (`remaining_mAh`, `capacity_mAh`): the vendor's `SOCCapRemain`
  and `SOCFullChargeCap`, published as read (`pack.c:510-518`).
- **Cluster SOC** (`cluster_calc.c:742`): `sum(remaining) / sum(capacity)`, i.e.
  **the JK counter**, not the estimator (this is
  [the 09-08 review](review_zaliakalnis_soc_estimation_2026-09-08.md) §8, still
  open). `/api/cluster/status` showed 76.4 % at 15:22 while the pack card showed
  94.7 %.
- **The inverter**: `GET /api/batcomm/config` is `"source":"pack","pack":"sodas15"`,
  so `batcomm.c:249` sends `s_pack.soc_pm`, the **estimator**. With
  `source: cluster` it would get the JK counter (`batcomm.c:158`).
  `/api/batcomm/status` `input.soc_pm` read 948 against the pack card's 947.

So on sodas the operator sees one number on the pack card, a different one on
the cluster card, and the Solis acts on the first.

## 3. Findings

### D1 - The integrator truncates to zero (the main defect)

`PackSoc_UnitAdvance` (`pack_soc.c:204`) does, per charge-group commit:

```c
moved = (int32_t)(((int64_t)dQ_mAh * PACK_SOC_FULL_pm) / (int64_t)u->capacity_mAh);
```

C integer division truncates toward zero, and **the remainder is thrown away**.
One per-mille is `capacity / 1000` of charge: **660 mAh** on this pack. The JK
`charge` group is polled every 5 s (`configs/solis_sodas_gateway.json`, plan 0),
so a step at 100-125 A is **140-175 mAh**, and `moved` is exactly 0 for charge
*and* discharge. A step reaches 660 mAh only above ~475 A.

It does not depend on the capacity the unit happens to hold: the plausibility
band `[nameplate/4, nameplate x 2]` is 165-1320 Ah, so the zero threshold is
165-1320 mAh, above anything a 5 s poll delivers on a pack of this class.

**Host reproduction** (`pack_soc.c` unmodified, 660 Ah, 1000 steps from 50 %):

| per-step dQ | real charge in 1000 steps | SOC after |
|---|---|---|
| 100, 140, 300, 600, 659 mAh | 100 / 140 / 300 / 600 / 659 Ah | **500 pm (unmoved)** for every one |
| 660 mAh | 660 Ah | 1000 pm |
| -140, -600 mAh | -140 / -600 Ah | **500 pm (unmoved)** |

**Live reproduction.** Across the seven full anchor windows of the capture the
estimate is **flat between anchors** while the counter rises 1.2-1.8 pp per
window:

| Window | Estimate | JK counter over the window |
|---|---|---|
| 15:24:20-15:29:01 | 948 -> 948 | +7.8 Ah (+1.18 pp) |
| 15:29:22-15:35:04 | 950 -> 950 | +10.3 Ah (+1.55 pp) |
| 15:35:25-15:41:09 | 951 -> 951 | +10.9 Ah (+1.65 pp) |
| 15:41:29-15:47:11 | 952 -> 952 | +11.6 Ah (+1.75 pp) |
| 15:47:32-15:53:14 | 954 -> 954 | +12.0 Ah (+1.82 pp) |
| 15:53:34-15:59:17 | 955 -> 955 | +11.7 Ah (+1.77 pp) |
| 15:59:37-16:05:19 | 957 -> 957 | +11.5 Ah (+1.74 pp) |

Over the 43 minutes the counter rose **12.7 pp** and the estimate rose
**1.0 pp**, all of it in +1/+2 steps at the anchor instants (every ~6 min 3 s).
The estimator is a voltage-to-SOC lookup refreshed on a timer.

**Why the tests never caught it.** Every test in `tests/test_pack_soc.c` that
asserts SOC *movement* advances in steps of **4 Ah to 300 Ah** (the cell tests,
lines 316-465, and the `note()` cases at 199-251, which use 5-10 Ah), on a pack
where a real poll is ~0.15 Ah. The only sub-Ah steps (lines 170, 189: 500 and
100 mAh) test whether `NoteCounter` *accepts* a step, not whether SOC moves.

**Same defect on every pack.** The per-cell units use the same function with the
balancer correction added (`pack.c:1895-1903`), so per-cell SOC does not
integrate either. Zaliakalnis (261 Ah) has a 261 mAh threshold, reached only
above ~188 A at 5 s; the code is identical so the integrator should be dead
there too. **Not verified on that board.** The 09-08 review's "its coulomb count
free-ran to 82 %" most likely describes the JK's own number, which stands until
the first anchor (`pack.c:539-543`); this is a reading of the code, not a
measurement.

### D2 - The anchor overwrites, and its stated confidence omits the dominant error

`PackSoc_UnitApplyAnchor` ends with `u->soc_pm = anchorSoc_pm` unconditionally,
and `pack.c:546-553` replaces the vendor number with the estimate as soon as
`PackSoc_UnitGet_pm() >= 0`. The anchor's sigma only sets `conf_pm`
(`1000 - 10 x sigma`, floored at 100); **nothing compares it to the prediction it
replaces**. There is no fusion step.

The sigma itself is `3 mV` ADC noise plus a quarter of the ohmic correction
(`pack.c:2048-2064`, `PACK_SOC_IR_SIGMA_DIV`). What it does not carry is
**polarisation and hysteresis**, which the design doc estimates at 10-20 mV on
the plateau ("50-100 % SOC of apparent shift", `design_bms_cell_health_estimation.md`
:654) and which the code's own comment acknowledges "biases the result in the
direction the current flows". A bias is not a variance, so charging the 1/4 as
*weight* does not stop it moving the estimate: it only makes the wrong number
look less sure.

**Reproduction with the firmware's own arithmetic** (`pack_soc.c` fed the
sample the way `SocOnCommit` does, 120 samples, fitted R = 3.725 mOhm, n = 16):

| Case | Terminal | Corrected OCV | Anchor | sigma | conf |
|---|---|---|---|---|---|
| Live 15:22 (board said 948 / 620) | 3.392 V, +96.2 A | 3369 mV | **948 pm** | 38 pm | **620** (exact match) |
| 11:40, back-solved from the operator's 81 % | 3.345 V, +96 A | 3323 mV | **815 pm** | 125 pm | **100 = "10 %"** |
| 11:40, operator's figure | 3.350 V, +99 A | 3327 mV | 835 pm | 125 pm | 100 |

The first row validates the harness against the live board; the next two give
the operator's 81 % and 10 % from the operator's own voltage and current. At
11:40 the estimator knew it was about +/-12 pp sure and it was ~50 pp wrong, and
the number was published regardless. At 15:24 it claimed +/-3.8 pp (conf 62 %)
and was ~8-13 pp high (§4). Both sigmas count noise and leave out the bias.

### D3 - A vendor re-calibration was taken for charge

At **16:08:00** the JK counter went 592 166 -> **660 000 mAh** between two
consecutive 20 s samples (+67.8 Ah, against ~0.6 Ah of real charge), and the
cell maximum read 3449 mV. That is the JK re-anchoring itself to 100 %. The
estimator accepted it: `PackSoc_NoteCounter` rejects steps above `cap / 8u`
(`pack.c:1882`) = **82.5 Ah**, and 67.8 Ah is under it. `UnitAdvance` then moved
SOC by +103 pm and the estimate went 958 -> **1000** (the clamp), which is the
**only time the integrator moved the estimate at all that day**, and it moved it
by a step that was not charge.

Within the next 20 s the pack current went **108.8 A -> 17.8 A** and kept
tapering. That is **consistent with the Solis reacting to 100 % on `0x355`**; it
is **not proven** (no CAN capture of the frame, and a CV hand-over or the Solis's
own end-of-charge logic would also taper). Whatever the cause, the pack was
**not** full at that instant (§4): the board said 100 % to a live inverter on
the strength of a vendor re-calibration plus a clamp.

### D4 - The diagnostics were silent

- `socDrift_pm` is `anchor - u->soc_pm` at each resolve. With a dead integrator
  `u->soc_pm` is the *previous anchor*, so drift is "this anchor minus the last
  one" - **+1/+2 pm all afternoon**, against a real prediction-vs-voltage
  disagreement of 10 pp or more. A working integrator would have shown it.
- `socConf_pm` = `1000 - 10 sigma_pm`, with the sigma of D2.
- `capacityLearned` / `capConf_pm` are counters (review 09-08 §5) and the
  plausibility band excludes nothing.

## 4. How wrong, and relative to what

Voltage cannot settle this on the plateau (`design_bms_cell_health_estimation.md`
§4: +/-15 % from one reading, "the information is not present"), so the bound
comes from charge, not from the cells:

1. The JK counter's increments are sound (84.0 Ah against an integral of 84.1 Ah).
2. At 16:08 the JK declared full: cell max 3449 mV at 108 A, which is the top
   knee, **not** a proof of 100 % (3.449 V under 0.16 C includes tens of mV of
   overpotential). Take true SOC at that instant to be **95-100 %**.
3. Between 11:40 (150 Ah) and 16:07:40 (592.2 Ah) the counter rose **442 Ah =
   67 pp of 660 Ah**.
4. Hence true SOC at 11:40 was **~28-33 %**, which agrees with the operator's
   independent 15-35 %. The estimator's 81 % was **~+50 pp** wrong, and
   81 % + 67 pp is 148 %, so *the estimator's own two readings cannot both be
   right unless capacity is above ~2 300 Ah* (442 Ah into the 19 % that was left).
5. At 15:24 the same argument gives **~82-87 %**: the estimator (94.8 %) was
   ~8-13 pp high, the JK counter (77.0 %) was ~5-10 pp **low**.

**Assumptions, stated so they can be attacked:** capacity is ~660 Ah (at 600 Ah
the 11:40 truth becomes ~22-26 % and the conclusion is unchanged); the 16:08
trigger corresponds to 95-100 %; the counter's offset is constant over the five
hours (its increments match the integral to 0.1 %).

**The two numbers err in opposite directions for different reasons.** The
estimator is a voltage lookup, wrong by tens of points on the plateau and
converging as the pack climbs the shoulder. The JK counter has a steady
~-8 to -10 pp offset that its own 3.449 V calibration erased at 16:08. Neither is
the answer; a coulomb count **anchored at the knees** (what
`design_bms_cell_health_estimation.md` §5 proposes) is.

## 5. Not established

- **That the Solis stopped charging because of the 100 %.** §D3. Needs a CAN
  trace (`/api/can/trace/on`) across the next knee, or Solis-side history in HA.
- **What the JK's 100 % trigger is.** Whether it is a cell-voltage setpoint
  (`vol_soc100`) or a charge-complete test. `pack_jkbms.c:226` mentions
  `vol_soc0` as an SOC calibration point; the 100 % counterpart is not read.
- **The board reboot at ~16:11:26** (uptime 8 484 s at 18:32:50, still
  `Pd1.1.59`, `reset_cause 0x14000003`, `/api/system/last-restart` is
  `present:false` so it was not the board rescuing itself). Probably a
  deliberate reboot; unconfirmed. It restored the estimate from the saved record
  (957, conf 860, the pre-knee value) and zeroed the anchor counters, which is
  the sampler's row at 16:11:42.
- **Whether zaliakalnis shows the same** (§D1).
- **The capacity the estimator unit actually holds** (`soc.unit.capacity_mAh`):
  not exposed over HTTP. §D1's conclusion holds across the whole accepted band.

## 6. Planned fixes

> **2026-10-07: F1-F4 below are SUPERSEDED by
> [design_soc_knee_anchored.md](design_soc_knee_anchored.md).** The same
> defects (D1-D4) stand, and so does their diagnosis; but F2's inverse-variance
> fusion keeps a *systematic* plateau bias in the loop (that document §2.1),
> and the zaliakalnis log shows the discharge side of D1/D2 (§1 there: -47 pp
> against a true -12.8 pp over 46 min at -43 A, and +16 pp *while discharging*).
> F5 stands as the interim.

Nothing below is implemented. Order is by value per risk. None may ship without
host tests first (§7), and all four touch what a live inverter is told, so each
is an OTA to be confirmed by the operator, not by the build.

### F1 - Stop truncating the integrator (D1)

Make `PackSoc_UnitAdvance` carry the sub-per-mille remainder, or derive the
current SOC from the already-exact `chargeSinceAnchor_mAh`. Two shapes:

- **Remainder carry:** `num = dQ x 1000 + rem; moved = num / cap; rem = num % cap`.
  Smallest change; adds one `int32` to `sPackSocUnit`.
- **Derive at read time:** `soc = anchorBase + chargeSinceAnchor x 1000 / cap`,
  no new state, but needs a rebase rule at the 0 / 100 % clamps that does not
  corrupt `chargeSinceAnchor_mAh` (capacity learning uses it).

**Budget before choosing.** `sPackSocUnit` exists 1 + 16 times per pack and main
SRAM has **~2.4 KB free** (CCM ~5.9 KB). Check `sizeof` first; a naive `int32`
may cost 8 bytes of padding per unit. The pack unit is what the inverter sees, so
**if the budget forces a choice the per-cell units can stay as they are**
(their published capacities are already the untrusted ones, review 09-08 §4).

### F2 - Fuse the anchor instead of overwriting (D2)

Replace `soc_pm = anchor` with an inverse-variance blend of the prediction and
the anchor. State per unit: the estimate and a variance that **grows with charge
moved since the last accepted anchor** (so a long, quiet stretch earns a looser
prediction and a recent knee a tight one).

The anchor's variance must contain what it omits today:

```
sigma_V^2 = sigma_adc^2 + sigma_ir^2 + sigma_pol(I, recent charge)^2 + sigma_hyst^2
sigma_soc  = sigma_V / k(ocv)          k = local OCV slope, as now
```

with `sigma_hyst` ~10-20 mV when the pack was charged or discharged in the last
hours (the design doc's own range) and `sigma_pol` scaling with |I|. On the
plateau that makes the anchor weigh ~0 and the coulomb count carry the estimate;
at the knees `k` is large and the anchor dominates, which is the behaviour the
design doc specifies and the graded weighting was meant to produce. **The hard
gate stays dropped** (§24.3: it admitted 0 of 690 real samples); what was
missing is the model-error term and the fusion, not a gate.

**Open design points** (to settle with numbers from this capture and
`configs/celllogs/zaliakalnis_cells.tsv`, not by taste):
the `sigma_hyst` / `sigma_pol` values and whether `sigma_pol` should be fitted
the way the ohmic `R` is; the process-noise rate for the prediction variance
(sensor offset is ~0.4 %/day on a 280 Ah cell, design doc §5.2); and the OCV
table's 3340-3370 mV span (90 -> 95 %, 0.6 mV per pp), which is where charge-side
bias is worst and where this afternoon's 94.8 % came from.

### F3 - Judge counter steps against the board's own current (D3)

Replace the fixed `cap / 8u` ceiling in the `PackSoc_NoteCounter` call
(`pack.c:1882`) with a **plausibility test against the integral of the measured
current over the same interval** (the board has `I` at every electrical commit):
accept the step only if `|d_counter - integral(I)|` is within a stated tolerance
(a few percent of `|integral(I)|` plus an absolute floor for ADC resolution).
This catches the 67.8 Ah-vs-0.6 Ah case by two orders of magnitude and every
re-seed the existing comment already worries about.

A **rejected step is information, not noise.** A positive step that lands the
counter on `capacity_mAh` is a vendor "full" event: record it, and let the
estimator treat it as a top-knee anchor **at its true meaning** (95-100 % for a
trigger reached under load; the exact sigma is an open point, §5), not as
charge. Count rejected steps in `/api/pack/status`.

### F4 - Make the diagnostics able to fail (D4)

- `socDrift_pm` becomes the **innovation**: anchor minus *prediction before
  fusion*. With F1 it then shows 10 pp disagreements instead of 1 pm.
- `socConf_pm` derives from the fused variance including the model-error term,
  so a plateau-charging pack reads low and a knee-anchored pack reads high.
- Expose the capacity the unit holds, and the counts of vendor steps accepted and
  rejected (F3).

### F5 - Interim, no firmware (operator's call, not taken)

Switch BatComm from `source: pack` to `source: cluster`: the inverter then gets
the JK counter (it was 5-10 pp low at worst today, against ~50 pp for the
estimator). Costs: the cluster is `degraded` with `sodas2` lost, and the
cluster's current rule publishes 10 % less than a single pack
(`design_battery_cluster.md` §3.2); the JK counter also takes its own
re-calibration steps. `POST /api/batcomm/config` is **live on return** (no
staging), so it is one request, reversible by posting the old document
(`configs/batcomm_sodas.json`). It changes what a live inverter is told, so it
waits for the operator.

### Related, already open

The cluster reconstructs SOC from the vendor's amp-hours and uses the estimate
only for the divergence check ([review 09-08](review_zaliakalnis_soc_estimation_2026-09-08.md)
§8, step 6 of its §10). After F1-F3 the right input to that sum is the pack's
fused SOC; that is a separate change and should follow, not accompany, these.

## 7. Tests (written before the fixes)

Host-side, in `tests/test_pack_soc.c`, which is where the gap is:

1. **Small steps integrate.** 1000 steps of 140 mAh on 660 Ah moves SOC by
   212 pm +/-1 (today: 0). Both signs; a step of 1 mAh repeated 660 000 times
   moves it by 1 pm.
2. **Replay of the capture.** Feed `sodas15_charge_2026-10-06.tsv` at its real
   cadence: between anchors the estimate must follow the counter (+1.2-1.8 pp
   per window), within 1 pp.
3. **A plateau anchor 50 pp away does not move a confident prediction** by more
   than a few pp; the same anchor at the knee does.
4. **The 16:08 step is rejected** (+67.8 Ah against 0.6 Ah of integral) and
   reported as a vendor event; a genuine +4 Ah step against a matching integral
   is accepted.
5. **Drift is a real innovation:** a prediction deliberately 10 pp from the
   anchor reports ~100 pm, not 1.
6. **Existing tests stay green**, and each existing test that uses a bulk step
   gets a small-step twin so the shape that hid this is covered.

**Acceptance on hardware** has no ground-truth meter, so it is consistency: over
a charge window the estimate's change equals `integral(I) / C` within 1 pp, no
discontinuity at a JK re-calibration, and at the next knee the estimate lands
within ~5 pp of 100 % without being driven there. Deploy by the usual
unconfirmed cycle (`tools/deploy.sh sodas --fw-only`, stop before confirm); the
operator confirms.

## Appendix - reproducing the finding

`pack_soc.c` is pure and libc-only, so no board is needed.

```c
/* gcc -I. -o t t.c App/Pack/pack_soc.c && ./t */
#include "App/Pack/pack_soc.h"
#include <stdio.h>
int main(void) {
    static const int step[] = { 140, 659, 660, -140 };
    for (unsigned i = 0; i < sizeof step / sizeof *step; i++) {
        sPackSocUnit u;
        PackSoc_UnitInit(&u, 660000);
        PackSoc_UnitApplyAnchor(&u, 500, 40, 660000);
        for (int k = 0; k < 1000; k++) PackSoc_UnitAdvance(&u, step[i]);
        printf("%5d mAh x1000 -> soc %d (from 500)\n", step[i], u.soc_pm);
    }
    return 0;
}
```

Live sampler used for the capture: every 20 s, `GET /api/pack/status`, columns
`time, V_mV, I_mA, soc_est_pm, conf_pm, remaining_mAh, drift_pm, anchorN, irN,
dcRes_uOhm, cellMin, cellMax` (the TSV header). Port 80 only; nothing was sent to
`:502` (see `feedback_never_probe_502_while_ha_polls`).

# JK BMS data, and estimating per-cell health and pack SOC — Direction

> Status: **observations + direction**, 2026-08-25. Nothing frozen: no
> interfaces approved, no phase committed. §2–§4 are findings (firmware
> evidence, cited); §5–§10 are a proposal. Related:
> [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
> (why autonomy constrains this), [modbus.md](modbus.md) §2 (the subscription
> surface this would consume), [pylontech_can_protocol.md](pylontech_can_protocol.md)
> (where the output goes).

Evidence base: the decompiled JK PB-series V19.21 firmware in
`~/Projects/JK_BMS/FW/decompiled/` (all six board images, Ghidra output,
`base = 0x08002000` — see that tree's `README.md` §1 for how the base was
established), the vendor spec PDF `JK-PB-series_BMS-RS485-Modbus-V1.1.pdf`,
the Qt monitor app's register map `src/core/JkRegisters.h`, and the vendor
Windows app's own decrypted `.jsonds` protocol datasource
(`~/Projects/JK_BMS/WinApp/datasource-fields.md`). Where they disagree, the
firmware wins; the datasource is the tie-breaker on field *names, scales and
units*, since it is what the vendor's own tooling decodes with.

> **Two corrections worth recording**, both from re-reading the firmware after
> the first draft. (a) The JK *does* say which cells it is balancing — §2.5;
> the first draft said it did not, and §5.5 changed materially as a result:
> the balancer became a correction rather than an exclusion. (b) The balancer
> **topology** is not settled by anything here; a draft claimed cell↔pack on
> thin evidence and §2.5 now states only what the binary pins down.

---

## 1. What this is for

The device is becoming an edge controller: poll the JK BMS, fuse with
inverter data, present a synthetic Pylontech pack to the Solis over CAN. The
inverter makes real decisions from what we send it on `0x355` (SOC/SOH) and
`0x351` (charge/discharge limits). Today the only candidate source for those
numbers is the JK's own SOC and SOH.

The question this document answers is whether that is good enough, and if
not, what can be built from what the JK actually measures. Short answers:

- **The JK's SOC is better than "linear voltage estimation" — but only
  sometimes.** It is a persisted coulomb counter re-anchored at two voltage
  thresholds. The linear interpolation is real, but it is only the *cold-start
  seed* used when the persisted state is missing or inconsistent (§3.2).
- **The JK's SOH is close to useless on a solar ESS.** It is
  `learnedFullCapacity / designCapacity`, and the learning only completes on a
  single uninterrupted empty→full charge inside a bounded time window. That
  event essentially never happens on a self-consumption battery, so SOH sits
  at 100 % forever (§3.5).
- **There is no per-cell capacity, per-cell SOH or per-cell SOC anywhere in
  the JK.** It carries exactly one capacity number for the whole pack.
- **The raw measurements are good enough to do considerably better on our
  side**, including per-cell. Three things the JK exposes that nothing was
  reading before this pass: the per-cell **balance-lead resistance** array
  (§2.2), the identity of the two cells the **balancer** is working between
  (§2.5), and the **charge stage** (Bulk/Absorption/Float), **cell chemistry**
  and three further thermistors past `0xEF` (§2.6). All are in the config as
  of 2026-08-25; none has been read from a board yet.

---

## 2. What the JK actually provides

### 2.1 The measurement surface

The realtime block is **294 bytes / 147 registers**, and covering it takes two
FC03 reads (§2.3). The first 120 registers (`0x00`–`0xEF`) hold everything the
estimator needs every frame; the rest is slow-moving state that older maps here
stopped short of.

| Byte off | Type | Unit | Field | Measured or derived |
|---|---|---|---|---|
| `0x00`–`0x3F` | 32 × UINT16 | mV | `CellVol0..31` | **measured** |
| `0x40` | UINT32 | bitmask | `CellSta` — bit *n* = cell *n* present | derived |
| `0x44` | UINT16 | mV | `CellVolAve` | derived (mean) |
| `0x46` | UINT16 | mV | `CellVdifMax` | derived (max−min) |
| `0x48` / `0x49` | UINT8 | — | `MaxVolCellNbr` / `MinVolCellNbr` | derived (argmax/argmin) |
| `0x4A`–`0x89` | 32 × UINT16 | mΩ | **`CellWireRes0..31`** | **measured** — see §2.2 |
| `0x8A` | INT16 | 0.1 °C | `TempMos` | measured |
| `0x8C` | UINT32 | bitmask | `CellWireResSta` — per-cell wire alarm | derived |
| `0x90` | UINT32 | mV | `BatVol` | measured |
| `0x94` | UINT32 | mW | `BatWatt` | derived (V×I) |
| `0x98` | INT32 | mA | `BatCurrent` (+charge / −discharge) | **measured** |
| `0x9C` / `0x9E` | INT16 | 0.1 °C | `TempBat1` / `TempBat2` | measured |
| `0xA0` | UINT32 | bitmask | `Alarm` (22 defined bits) | derived |
| `0xA4` | INT16 | mA | `BalanCurrent` | measured |
| `0xA6` | UINT8 | — | `BalanSta` — 0 = not balancing, 1 = low side, 2 = high side (**not** the PDF's "charge/discharge", see §2.5) | state |
| `0xA7` | UINT8 | % | `SOC` | **derived — see §3** |
| `0xA8` | UINT32 | mAh | `SOCCapRemain` | **derived — see §3** |
| `0xAC` | UINT32 | mAh | `SOCFullChargeCap` | **derived — see §3.5** |
| `0xB0` | UINT32 | — | `SOCCycleCount` | derived — see §3.6 |
| `0xB4` | UINT32 | mAh | `SOCCycleCap` (lifetime throughput) | integrated |
| `0xB8` | UINT8 | % | `SOCSOH` | **derived — see §3.5** |
| `0xB9` | UINT8 | — | `Precharge` | state |
| `0xBA` / `0xC2` | UINT16 | bitmask | `UserAlarm` / `UserAlarm2` (undocumented) | — |
| `0xBC` | UINT32 | s | `RunTime` | counter |
| `0xC0` / `0xC1` | UINT8 | — | `ChargeState` / `DischargeState` (MOS) | state |
| `0xC4`–`0xCF` | 6 × UINT16 | s | protection auto-release countdowns | state |
| `0xD0` | UINT8 | bitmask | `TempSensorAbsent` (1 = present) | state |
| `0xD1` | UINT8 | — | `HeatingStatus` | state |
| `0xD4` | UINT16 | — | `TimeEmerg` (0 = off) | state |
| `0xD6` | UINT16 | — | `DischrgCurCorrect` — discharge current calibration factor | config |
| `0xD8` / `0xDA` | UINT16 | mV | `VolChargCur` / `VolDischargCur` — **raw shunt-amplifier voltages** | **measured** |
| `0xDC` | FLOAT32 | — | `BatVolCorrect` — pack-voltage calibration factor | config |
| `0xE0` | UINT16 | — | `ChargPWMDutyCycle` — high-side balancer duty. **Suspect: appears not to be populated over Modbus, §2.5** | state |
| `0xE2` | UINT16 | — | `DischargPWMDutyCycle` — low-side balancer duty. Zero between bursts, which is what makes it usable (§5.5a) | state |
| `0xE4` | UINT16 | 0.01 V | `TotalBatVol` — second pack-voltage measurement | measured |
| `0xE6` | INT16 | mA | `HeatCurrent` | measured |
| `0xE9`–`0xED` | UINT8 | — | `AccStatus`, `SpecialChargerSta`, `StartupFlag`, `VolC-`, `McuId` | state |
| `0xEF` | UINT8 | — | `ChargerPlugged` | state |
| `0xF0` | UINT32 | **0.1 s** | `SysRunTicks` — the firmware's own tick, see §3 | counter |
| `0xF4` | UINT32 | 0.1 s | `PvdTrigTimestamps` | counter |
| `0xF8` / `0xFA` / `0xFC` | INT16 | 0.1 °C | **`TempBat3` / `TempBat4` / `TempBat5`** | measured |
| `0xFE` | UINT16 | — | `ChrgCurCorrect` — charge current calibration factor | config |
| `0x100` | UINT32 | s | `RtcCounter` — seconds since 2020-01-01 | clock |
| `0x104` | UINT32 | — | `DetailLogsCount` | counter |
| `0x108` | UINT32 | s | `TimeEnterSleep` | state |
| `0x10C` | UINT8 | — | `PclModule` | state |
| `0x10D` | UINT8 | — | **`CellType`** 0=LFP 1=Li-ion 2=LTO | config |
| `0x110` | UINT16 | s | `ChargeStatusTime` — time in the current charge stage | state |
| `0x112` | UINT8 | — | **`ChargeStatus2`** 0=Bulk 1=Absorption 2=Float | state |
| `0x113` | UINT8 | bitmask | `SwitchStatus` — LCD buzzer / DRY1 / DRY2 alarm | state |
| `0x119`–`0x124` | 12 B | — | `EnableFlags` | config |

147 registers is exactly the firmware's own `quantity + (byteOffset/2) < 0x93`
bound (§2.3) — **the cap *is* the block size**. The final two bytes of
`EnableFlags` fall outside that bound and are unreachable, which costs nothing.
The Qt monitor app's `jk::rt` map stops at register 120 and leaves everything
from `0xF0` on undocumented.

Field names, sizes and scales for the whole block are corroborated
independently by the vendor Windows app's own decrypted `.jsonds` datasource
(`~/Projects/JK_BMS/WinApp/datasource-fields.md`, subtable `02`), whose BLE
frame offsets are exactly **Modbus byte offset + 6**. Every field the firmware's
snapshot builder touches lines up with it.

Settings block (`0x1000`) additionally supplies, as configuration we can
read and — with FC10 — write: `VolCellUV`/`VolCellOV` and their release
points, `VolSOC100` / `VolSOC0`, `VolCellRCV` (recommended charge) and
`VolCellRFV` (float), `CapBatCell` (design capacity, mAh), `CellCount`,
`BalanEN`, `CurBalanMax` (mA), `VolStartBalan`, `VolBalanTrig`, and 32 ×
`CellConWireRes` (µΩ, the compensation values). Device info (`0x1400`) has
`OddRunTime` (cumulative seconds) and `PwrOnTimes` — a crude pack age.

### 2.2 The per-cell balance-lead resistance array

`jk::rt` in the Qt app leaves a 64-byte hole between `MinVolCellNbr_off`
(`0x49`) and `TempMos_off` (`0x8A`), and nothing here read it before this pass.
That hole is **`CellWireRes0..31`, 32 × UINT16**, confirmed three ways:

- The vendor PDF's realtime table lists 32 rows `均衡线电阻n / CellWireResn`
  there, unit column `mΩ`.
- The firmware's realtime snapshot builder (`FUN_080109ba`,
  `unified/shared/shared_04.c:4001`) byte-swaps that region as a run of
  16-bit words — `for (i = 0; i < 0x21; i++) swap16(src + 0x4a + i*2)` —
  rather than copying it as opaque bytes. (The 33rd iteration overlaps
  `TempMos` at `0x8A`, which is then swapped again from the source; harmless,
  and it is why the bound reads 33 rather than 32.)
- The vendor datasource gives it as `cellWireRes`, `a[32]:u16`, **scale
  0.001, unit Ω**.

**The LSB is settled: 1 mΩ per count**, and the firmware proves it
dimensionally rather than by assertion. `FUN_0800d8f0` computes the alarm
threshold as

```c
limit = ((CellVolAve_mV - 1200) * 1000) / CurBalanMax_mA;   /* clamped to 4000 */
alarm = (wireRes > limit/4) || (wireRes < 5);
```

`(V − 1.2 V) / I` is a resistance. For a 3300 mV cell and `CurBalanMax` =
2000 mA that is 1.05 Ω, and the expression evaluates to **1050** — so one
count is one milliohm. Nothing else makes the units close.

The same expression bounds the range the firmware itself considers sane:
**5 counts (5 mΩ) to `limit/4`**, about 260 mΩ at those settings. Below 5 the
reading is treated as implausible; above a quarter of `(V−1.2)/I` the
balancer no longer has the headroom to reach its configured current, which is
what the alarm is really saying.

**It is the *balance lead*, not the busbar.** The Chinese name is 均衡线电阻,
"balance-wire resistance", and the measurement method proves it:
`FUN_0800d75a(cell)` (`shared_04.c:1733`) ramps the balancer's PWM onto that
one cell, takes five samples of `ΔV_cell × 1000 / I_balance`, averages and
scales them. It measures the path the *balance current* takes — the sense and
balance harness — not the main power path through the busbars. So it detects a
loose or corroded balance connector, and it says nothing directly about a
loose main terminal.

Two consequences worth keeping straight:

- The measurement **only exists when the balancer can run**. A pack with
  `BalanEN = 0`, or one sitting below `VolStartBalan`, has stale or zero
  readings.
- A high balance-lead resistance corrupts that cell's *measured voltage*
  while balancing current flows through it — an IR drop on the sense path.
  That is a direct threat to the anchors in §5.3, and a reason to gate on it.

Note the asymmetry with the settings block's `CellConWireRes` (32 × UINT32,
scale 0.001 **mΩ**, i.e. 1 µΩ per count): the compensation value the operator
can write has 1000× the resolution of the value the BMS measures.

### 2.3 Sampling budget

At 115200 8N1 a 120-register FC03 is 8 bytes out and 245 back ≈ 22 ms of
wire time, plus turnaround and the 3.5-character silences. Call it 50–80 ms
per frame, so **1 Hz costs under 10 % of the bus**.

The anchors, not the sample rate, set how well capacity is estimated (§5.9) —
but the rate is not free either: §5.4's resistance estimate needs to catch
current *steps*, and a 5 s poll aliases them badly. The shipped config polls
its live table at 5 s with the period a named constant, so moving to 1 Hz for
the estimator is a one-line change once the shared-bus budget is measured.

Hard caps from the firmware's own parser (`Modbus_ParseRequest`,
`shared_04.c:4358`): quantity < 124 registers, and `quantity + (byteOffset/2)
< 147`. The block is 147 registers (§2.1), so **covering it takes two
transactions**: registers 0–119 (bytes `0x00`–`0xEF`, everything the estimator
needs every frame) and registers 120–145 (bytes `0xF0`–`0x123`, the §2.6
fields). The second one is slow-moving — charge stage, chemistry, the extra
thermistors — so poll it at 0.1 Hz and the bus cost barely moves.

The device sits on the one wired RS485 port (`mbPort_rs485`, USART2) that the
Solis also uses, at a different baud. Per-device baud is a config field
(`modbus_records.h:159`), so time-multiplexing 9600 and 115200 on one pair is
expressible — whether it is *wise* is
[design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
§5's open question, not this document's.

### 2.4 What is *not* available, and what that forecloses

This list is the real constraint on §5, more than any algorithm choice.

- **No per-cell current.** Cells are in series, so pack current is each
  cell's current — *except* for whatever the balancer moves (§5.5). This is
  the single fact the whole per-cell method rests on, and the balancer is the
  single thing that breaks it.
- **No per-cell temperature.** Two pack sensors plus the MOS. Cell-to-cell
  thermal gradients across a 16S pack are real and invisible here.
- **No timestamps.** Frames are whatever the wire hands us; the local tick at
  reply time is the only timebase. Integration error from that is negligible
  next to current-sensor offset (§5.2).
- **Resolution floors:** cell voltage 1 mV, current 1 mA, capacity 1 mAh,
  SOC and SOH **1 % integers**. A 1 % SOC step on a 280 Ah pack is 2.8 Ah —
  the JK's SOC cannot express anything finer even when it is right.
- **No raw or unfiltered values.** Whatever averaging the JK applies to
  current and cell voltage is inside the firmware and not characterised here.
  (Though `VolChargCur` / `VolDischargCur` at `0xD8`/`0xDA` are the shunt
  amplifier's raw voltages, which is the closest thing to a raw current
  reading the device offers — useful for §10's offset question.)

### 2.5 Which cells the balancer is working on — answered

The vendor's mobile app highlights the cells being balanced, so the
information is derivable from what the BMS reports. It is not a hidden bitmask
— the Windows monitor's own datasource has no such field. **It is
`MaxVolCellNbr` and `MinVolCellNbr`, which the balancer uses as its operands by
construction.**

The balancer/measurement task (`shared_04.c:2160`) scans every cell once per
pass, and inside that scan does exactly this:

```c
for (cell = 0; cell < CellCount; cell++) {
    SelectCell(cell);                              /* FUN_0800add6      */
    if (cell == RT[0x48]) BalanceHighSide();       /* FUN_0800d9e8      */
    if (cell == RT[0x49]) BalanceLowSide();        /* FUN_0800dbb6      */
    EnableSense(1);  delay(1);
    v = ReadCellAdc();                             /* FUN_0800d1c6(2,…) */
    DeselectCell(cell);
    ...
}
```

`RT[0x48]` is `MaxVolCellNbr` and `RT[0x49]` is `MinVolCellNbr` — the same two
bytes the Modbus realtime block publishes. The two routines are the two
directions of the balancer: `FUN_0800d9e8` runs on the highest cell,
`FUN_0800dbb6` on the lowest, each ramping its own PWM duty (`RT[0xE0]` /
`RT[0xE2]` — the datasource names them `Charg. PWM` and `Discharg. PWM`) and
both writing the resulting current to `BalanCurrent` (`RT[0xA4]`) and the
state to `BalanSta` (`RT[0xA6]`).

**`BalanSta` is a hand-off, not a direction.** The two routines gate on it and
pass it back and forth: `FUN_0800dbb6` runs only while `BalanSta == 1` and sets
it to `2` on completion; `FUN_0800d9e8` runs only while `BalanSta == 2` and
sets it to `1`. `BalanSta` becomes `0` only when `FUN_0800d8f0` withdraws
permission entirely. So:

| `BalanSta` | Meaning |
|---|---|
| 0 | balancing not permitted (disabled, below `VolStartBalan`, delta under `VolBalanTrig`, a wire-res fault, or a cell-OV alarm) |
| 2 | the **high side** owns the converter — charge is leaving `MaxVolCellNbr` |
| 1 | the **low side** owns it — charge is entering `MinVolCellNbr` |

**Only one direction is live at a time, and both share one current sense**
(`FUN_0800d1c6(3, …)`, the same ADC channel in both routines). That removes the
ambiguity that would otherwise sink the accounting: `BalanCurrent` is never a
mixture, and `BalanSta` says which cell it belongs to.

**The topology is open.** The vendor's own description of the PB line is
cell-to-cell by flyback / capacitive energy transfer, and nothing found here
contradicts it. What the firmware and the binary do pin down:

| Fact | Where |
|---|---|
| The two duty registers drive **different timers at different switching frequencies** — `RT[0xE2]` (low side) is `TIM1_CCR1` at **60 kHz**, `RT[0xE0]` (high side) is `TIM3_CCR2` at **300 kHz** | `FUN_0800a210` / `FUN_0800a22a`; the pointers resolve to `0x40012C00` and `0x40000400` in the `.bin`, and the ARR basis is `f_clk/60000` and `f_clk/300000` |
| One shared current sense serves both | both routines read `FUN_0800d1c6(3, 0x2a)` |
| Cell selection is a **serially shifted switch matrix**, one 4-byte pattern per cell (with an odd/even bit distinction), taking milliseconds to change | `FUN_0800add6` / `FUN_0800aea6` → `FUN_0800acf4` |
| A separate 3-state path selector plus a balance-path enable | `FUN_0800ae60` (bit 1 / bit 2, mutually exclusive, or neither) and `FUN_0800ae48` (bit 0) |

Two different switching frequencies on two different timers say **two distinct
converter stages**, not two switches of one. And the millisecond-scale serial
mux says the max and min cells are **never connected at the same instant** —
which rules out a direct cell-to-cell transfer *within a switching cycle*, but
not one time-multiplexed across the 1 s bursts, which is what a flyback with an
intermediate store looks like from software.

The one argument against a purely passive intermediate is energetic: a burst
moves 2 A for up to 1 s — **2 coulombs** — and no capacitor at BMS voltages
holds that (0.2 F at 10 V), nor does any inductor. So either the store is
recycled at switching frequency against something large, or the other side of
the transformer sits on the pack or a sub-string. Which of those it is, this
analysis cannot say.

**It also does not gate anything.** §5.5 carries both variants, and §5.5a's
estimator B computes the per-cell transfer independently for every cell — so it
produces the answer rather than assuming it (§10 item 4).

**Bursts are ≤ 1 s, current-regulated.** Each routine latches
`SysRunTicks + 10` on entry and exits when the tick passes it — exactly 1.0 s
at 0.1 s per count. Inside, it ramps a PWM duty byte from 20 upward in steps of
5 every 10 ms, backing off whenever `|BalanCurrent|` exceeds `CurBalanMax`. So
a burst is a closed-loop current source at the configured limit, not an
open-loop bleed. **Both routines zero their duty register on exit**, which is
what makes those registers a usable "transferring right now" flag.

So the complete, exact answer, all from fields already on the wire:

| Question | Field |
|---|---|
| Is a balancing **phase** active? | `BalanSta != 0`. It does **not** mean charge is moving right now — see the duty note below |
| Which cell is charge being taken *from*? | `MaxVolCellNbr` (`0x48`) |
| Which cell is it going *to*? | `MinVolCellNbr` (`0x49`) |
| How much? | `BalanCurrent` (`0xA4`, mA) |
| Is charge moving **right now**? | duty register non-zero — `0xE2` for the low side, `0xE0` for the high side (suspect, below) |

Three details that matter for §5.5:

- **The pair is re-selected roughly every 5 s.** The scan recomputes
  max/min only when `SysRunTicks` has advanced by `0x32` = 50 counts, and
  `SysRunTicks` is 0.1 s per count. So the balance target is piecewise
  constant over ~5 s windows — coarser than a 1 Hz poll, so the poll resolves
  it comfortably and the residual aliasing is at most one sample.
- **Exactly two cells at a time, never more.** There is one high-side call
  and one low-side call per scan pass. Any model that assumes a set of
  balancing cells is over-general.
- **`ChargPWMDutyCycle` at `0xE0` appears never to be copied into the Modbus
  response.** The snapshot builder byte-swaps `0xDC` (the f32), then swaps
  `0xDA` a *second* time, then `0xE2` — the line that should have handled
  `0xE0` reads as a copy-paste duplicate of the `0xDA` line. If that survives
  from the real binary rather than being a decompiler artefact, `0xE0` carries
  stale buffer contents over RS485. **Verify before using it**; `0xE2` is
  unaffected and is the one to rely on.

### 2.6 Fields worth having

PeriphNet's shipped JK config already reached past `0xEF` — `TempBat3/4/5`,
`SysRunTicks`, `TempSensorAbsent`, `TotalBatVol` and `HeatCurrent` were added
by hand and are **hardware-verified** across three BMS
(`configs/readout_2026-08-18.tsv`). So the region is known-readable, not
speculative; what follows is what was still missing from it.

That read-out also settles the `SysRunTicks` scale independently of the vendor
datasource. Board #1 reports `sys_run_ticks` = 2 759 524.6 s (31.9 days) against
`total_runtime` = 25 671 489 s (297 days) over 21 power cycles. At a 1 s LSB the
same raw value would be 319 days — longer than the device's entire lifetime
runtime, which is impossible. **0.1 s per count, confirmed on hardware.**

- **`ChargeStatus2` (`0x112`): Bulk / Absorption / Float**, with
  `ChargeStatusTime` (`0x110`) counting seconds in the current stage. The JK
  already classifies the charge stage; §5.3's anchor gate wants exactly this.
- **`CellType` (`0x10D`): LFP / Li-ion / LTO** — lets the OCV table be
  selected rather than assumed, and catches a pack whose chemistry preset was
  set wrong (which is also one of the triggers for the §3.2 re-seed).
- **`TempBat3/4/5` (`0xF8`–`0xFD`)** — three more thermistors than the Qt app
  reads. `TempSensorAbsent` already says which are populated. Cell-to-cell
  thermal gradient stops being completely invisible.
- **`RtcCounter` (`0x100`)** — seconds since 2020-01-01. A real timestamp
  source for logged anchors, independent of our own clock.
- **`VolChargCur` / `VolDischargCur` (`0xD8`/`0xDA`)** — raw shunt-amplifier
  voltages, the input to whatever filtering produces `BatCurrent`. §10's
  current dead-band question is answerable by watching these move while
  `BatCurrent` reads zero.
- **`DischrgCurCorrect` / `ChrgCurCorrect` (`0xD6`/`0xFE`)** — the BMS's own
  current calibration factors, which bear directly on §10's gain question.

**All of this is now in the config.** `configs/gen_jk_config.py` went from 87
points to 114 on 2026-08-25 — the 16-cell balance-lead array, its alarm
bitmask, both balancer duty registers, and everything listed above.
[modbus.md](modbus.md) §11a.13 has what changed, the grouping decisions and the
measured wire cost. Both generated files compile through the real
`MbCfgCompile` on the host; **neither has been uploaded to a board.** That
upload is phase 0 (§9). It needs no firmware work beyond one two-line addition,
`unit: "ohm"` (DLMS code 38), without which the resistance array has no unit —
so a board must be flashed with that before the config will be accepted.

---

## 3. What the JK's SOC, SOH and capacity actually are

All of the following is one FreeRTOS task, `FUN_08016484`
(`unified/shared/shared_05.c:3674`), which loops **every 200 ms**:

```c
while (RT[0xd2] == 0) delay(5);        /* wait for measurement subsystem */
for (;;) {
    FUN_08015ef0();   /* §3.2  cold-start seed, only if RT[0xd3] is set  */
    FUN_08015f50();   /* current -> charge increment                     */
    FUN_08015fb0();   /* §3.6  lifetime throughput accumulator (0xB4)    */
    FUN_08016156();   /* §3.3/§3.4  coulomb count + anchor clamps        */
    FUN_0801622e();   /* §3.5  capacity learning                         */
    FUN_0801631a();   /* energy (Wh) accumulators, persisted 0x368/0x36d */
    FUN_0801641a();   /* §3.3  derive SOC%, SOH%, cycle count            */
    delayUntil(200);
}
```

Two RAM structures are involved throughout: `RT` (`DAT_080164dc`, the
realtime block, offsets identical to the Modbus byte offsets in §2.1) and
`SET` (`DAT_080164e0`, the settings block, likewise). That the RAM mirrors
share the wire offsets is corroborated at four independent points —
`SET[0x7C]` used as design capacity, `SET[0x18]` as the SOC-100 % voltage,
`SET[0x1C]` as SOC-0 %, `SET[0x114]` bit 9 as the float-charge flag.

### 3.1 Is the JK's SOC "just linear voltage estimation"?

**Half. And the half that is true matters.** A linear interpolation exists and
is exactly as bad as expected — but it is the cold-start *seed*, not the
running estimate. What runs is coulomb counting. The practical damage comes
from *when* the seed fires (§3.2) and from how rarely the anchors that correct
the counter are reachable (§3.4).

### 3.2 The cold-start seed — linear interpolation on average cell voltage

`FUN_08015ef0` (`shared_05.c:3220`), gated on `RT[0xd3] == 1`:

```c
RT[0xAC] = SET[0x7C];                        /* fullCap := designCap     */
persist(0x35e, &RT[0xAC], 4);
uint32_t uv = SET[0x04];                     /* VolCellUV   (mV)         */
uint16_t vavg = RT[0x44];                    /* CellVolAve  (mV)         */
if (uv < vavg)
    RT[0xA8] = (RT[0xAC] * (vavg - uv)) / (SET[0x0C] - uv);   /* SET[0x0C] = VolCellOV */
else
    RT[0xA8] = 0;
RT[0xd3] = 0;
```

So the seed is

> `remainingCapacity = designCapacity × (V̄cell − VolCellUV) / (VolCellOV − VolCellUV)`

**straight-line between the under-voltage and over-voltage *protection*
thresholds** — not even between `VolSOC0` and `VolSOC100`, which exist as
settings and are used elsewhere (§3.4). With typical LFP settings
(UV 2500 mV, OV 3650 mV) a pack resting at 3.30 V/cell seeds to
`(3300−2500)/(3650−2500) = 69.6 %`. The true SOC of an LFP cell resting at
3.300 V is anywhere from roughly 30 % to 90 %. At 3.35 V the seed says 74 %
while the cell is very likely above 95 %. As an SOC estimate for LFP it is
worthless, and §4 explains why no variation on it could be better.

**When does it fire?** `RT[0xd3]` is set by the boot-time restore path
`FUN_08005960` (`shared_01.c:1988-2013`) when the persisted state cannot be
trusted, and by roughly a dozen sites in `divergent.c` — the "one-key
LFP/Li-ion/LTO" chemistry presets and related reconfiguration commands.
Concretely, the seed runs when:

- the NVM signature does not match the current configuration (cell count or
  chemistry changed), or the stored record is absent — i.e. first boot after
  commissioning, or after any settings change that invalidates it;
- the restored `remainingCapacity` is negative or exceeds full capacity;
- an operator presses a one-key chemistry preset.

On an ordinary reboot the JK **restores** `RT[0xA8]` from NVM (key `0x35D`
signature + `0x35E` capacity, `0x363` throughput) and does *not* re-seed.
`FUN_08005922` (`shared_01.c:1948`) persists it periodically. That is the
correct design, and it means a JK that has been running undisturbed is not
carrying a linear guess around.

### 3.3 The running estimate — coulomb counting

`FUN_08016156` (`shared_05.c:3415`) integrates charge into a pair of
counters (`DAT_08016504[0]` = remaining mAh, `[1]` = full mAh), then
`FUN_0801641a` (`shared_05.c:3639`) derives the reported values:

```c
if (RT[0xAC] == 0) { RT[0xA7] = 0; RT[0xB0] = 0; }
else {
    v = ((RT[0xA8] * 1000) / RT[0xAC] + 5) / 10;
    RT[0xA7] = (v > 100) ? 100 : v;                 /* SOC%  = remain/full  */
    RT[0xB0] = RT[0xB4] / SET[0x7C];                /* cycles = thru/design */
}
v = ((RT[0xAC] * 1000) / SET[0x7C] + 5) / 10;
RT[0xB8] = (v > 100) ? 100 : v;                     /* SOH%  = full/design  */
```

Integration is fenced: charge is only added while remaining sits inside
`[full/100, full×99/100]`, and is hard-clamped to `[0, full]`. Outside that
band the counter is pinned, which is why a JK reporting exactly 0 % or 100 %
tells you nothing about how far past the endpoint the pack actually is.

### 3.4 The two anchors

`FUN_08016032` (`shared_05.c:3328`) — **full**:

- Trigger: `cellVoltage[MaxVolCellNbr] ≥ VolSOC100` held continuously for
  `(RT[0xF0] − t0)/10 > 3` — **3 s**. `RT[0xF0]` is `SysRunTicks`, and the
  vendor datasource gives its scale as 0.1 s per count (§2.1), so the `/10`
  converts to seconds exactly.
- Alternate path when `SET[0x114]` bit 9 (`ChargingFloatMode`) is set *and*
  the device address is 0: latch on a charger-state check instead of the
  timer — the master unit in a parallel string defers to the charger.
- Release: max cell falls below `VolSOC100 × 90/100`, or SOC drops below 99 %.
- Effect (`FUN_08016156`, edge-triggered): `remaining := full`.

`FUN_080160fa` (`shared_05.c:3378`) — **empty**:

- Trigger: `cellVoltage[MinVolCellNbr] < VolSOC0` held for the same **≈3 s**.
- Release: min cell rises above `VolSOC0 × 105/100`.
- Effect: `remaining := 0`.

These two are the *only* corrections the counter ever gets. Between them it
is open-loop, and its drift is whatever the current sensor's offset
integrates to. Note both anchors key on a **single extreme cell**, not the
average — correct for protection, and it means the JK's 100 % is "the first
cell got there", which is exactly the semantics a series pack should have.

### 3.5 Capacity learning, and why SOH is stuck at 100 %

`FUN_0801622e` (`shared_05.c:3477`):

1. **Start:** the empty latch sets while learning is inactive → zero the
   accumulator, mark learning active, stamp the start tick.
2. **Abort:** elapsed seconds (`(SysRunTicks − start)/10`) exceed a firmware
   constant (`DAT_08016508`) → clear the accumulator and the active flag. The
   learning window is time-bounded; the constant's value was not resolved, but
   the unit is now known to be seconds.
3. **Accumulate:** while active, add charge into a 32-bit accumulator, floored
   at zero.
4. **Capture:** on the full latch **or** on `Alarm` bit 4 (`CellOverVolt`):
   ```c
   acc = accumulator;
   RT[0xAC] = (acc > designCap || acc == 0) ? designCap : acc;   /* fullCap */
   persist(0x35e, &RT[0xAC], 4);
   RT[0xA8] = RT[0xAC];                                          /* remain := full */
   ```
5. **Clamp, every pass:** `if (designCap < fullCap) fullCap = designCap`.

Three consequences, all of them load-bearing:

- **Learning requires one uninterrupted empty→full charge**, starting from a
  cell below `VolSOC0` and ending with a cell above `VolSOC100`, inside a
  bounded time window. A self-consumption solar battery cycles roughly
  30–80 % and is never deliberately taken to the bottom knee. On such an
  install the learning never completes, `fullCap` stays at `designCap`, and
  **SOH reads exactly 100 % for the life of the pack**.
- **Capacity can never exceed nameplate.** The clamp is unconditional. If the
  installed cells are better than the configured `CapBatCell` — the common
  case, since installers set it conservatively — the surplus is discarded and
  SOH is truncated to 100 % with no indication.
- **SOH has 1 % resolution and is a pure ratio.** It carries no information
  about *which* cell is weak, because there is no per-cell capacity anywhere
  in the firmware to carry it.

### 3.6 Cycle count and throughput

`RT[0xB4]` (`SOCCycleCap`) is a genuine lifetime charge-throughput
accumulator in mAh, integrated by `FUN_08015fb0` and persisted to NVM key
`0x363` every 1000 mAh. It is the one number here that is honest and useful.

`RT[0xB0]` (`SOCCycleCount`) is `RT[0xB4] / SET[0x7C]` — integer division of
throughput by *currently configured* design capacity. It is not a count of
anything that happened; it is a derived ratio, it moves in whole-number
steps, and **editing design capacity retroactively rewrites the pack's cycle
history**. Read `SOCCycleCap` and derive your own if you want a cycle number.

### 3.7 Trust summary

| Value | Trust | Why |
|---|---|---|
| Cell voltages, pack V/I, temperatures | **Yes** | direct measurements |
| `CellWireRes` array | **Yes** (1 mΩ/LSB, §2.2) | direct measurement — but of the *balance lead*, not the busbar, and only fresh while the balancer runs |
| `MaxVolCellNbr` / `MinVolCellNbr` | **Yes, and more than they look** | argmax/argmin *and* the balancer's two operands (§2.5) |
| `ChargeStatus2` (Bulk/Abs/Float), `CellType` | **Yes** | firmware state, in the region older maps here stopped short of (§2.6) |
| `SOCCycleCap` (throughput) | **Yes** | genuine integrator, persisted |
| `SOC` / `SOCCapRemain` | **Conditionally** | coulomb counting, but open-loop between anchors, 1 % resolution, and re-seeded by §3.2 whenever config changes |
| `SOCFullChargeCap` | **No** | equals design capacity unless a full empty→full charge has completed; clamped to nameplate |
| `SOCSOH` | **No** | ratio of the above; reads 100 % indefinitely on a solar ESS |
| `SOCCycleCount` | **No** | derived ratio, rewritten by a settings edit |
| Per-cell capacity / SOH / SOC | **Does not exist** | — |

---

## 4. Why LiFePO4 makes this the interesting problem

Everything above would be a footnote on an NMC pack. LFP's open-circuit
voltage curve is the reason it is not.

Approximate rest OCV for a prismatic LFP cell at 25 °C:

| SOC | 0 % | 5 % | 10 % | 20 % | 30 % | 50 % | 70 % | 80 % | 90 % | 95 % | 99 % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| OCV | 2.50 | 3.10 | 3.20 | 3.25 | 3.27 | 3.29 | 3.31 | 3.32 | 3.34 | 3.37 | 3.45 |

Differentiating that table gives the working slope `k = dOCV/ds`, and with a
cell-voltage noise of `σ_V` ≈ 3 mV (the figure §10 item 7 measures) the SOC a
single voltage reading can resolve is just `σ_V / k`:

| Region | `k` | SOC from one reading |
|---|---|---|
| 30–80 % (plateau) | 0.2 mV/% | **±15 %** — no filter fixes this; the information is not present |
| ~90 % (shoulder) | 2 mV/% | ±1.5 % |
| above ~95 % (top knee) | 20 mV/% | ±0.15 % |
| below ~10 % (bottom knee) | 10 mV/% | ±0.3 % |

Add cell-to-cell offset on top of `σ_V` when comparing *different* cells'
absolute SOC; it cancels when tracking one cell over time, which is what §5.2
does. §5.9.3 carries the same figures expressed in amp-hours.

Two conclusions drive the whole proposal:

1. **Voltage is an anchor, never a continuous estimate.** Any scheme that
   maps volts to SOC in the plateau — the JK's seed, or a naive Kalman filter
   with an OCV measurement model — is fitting noise.
2. **The knees are where the information is**, and they are visited far less
   often than daily on a self-consumption battery. Spending those visits well
   is most of the design; §5.9 is what makes rare visits sufficient.

Hysteresis compounds it: LFP charge-OCV and discharge-OCV differ by roughly
10–20 mV across the plateau — which is 50–100 % SOC of apparent shift there,
and a small fraction of a percent at the knees. Another reason to only ever
believe voltage at the knees.

---

## 5. Proposal

### 5.1 The idea in one line

Every cell in a series string sees the same current, so **coulombs are shared
and only the anchors differ**. Fit each cell's own OCV-derived SOC against
the shared charge integral, and the slope of that fit *is* one over that
cell's capacity.

### 5.2 The model

Per cell *i*, over a window:

```
s_i(t) = s_i(t0) + (Q(t) − Q(t0)) / C_i
```

where `s_i` is fractional SOC, `Q` is the shared charge integral in Ah, and
`C_i` is cell capacity in Ah. Take observations `(x_k, y_ik)` where
`x_k = Q(t_k) − Q_origin` and `y_ik = f(V_i(t_k))` is SOC read off the
inverse OCV table — **but only at instants that pass the §5.3 gate**. Then a
weighted least-squares line through those points gives

```
slope b_i = 1 / C_i        →  C_i = 1 / b_i
intercept a_i              →  s_i at the window origin
```

Five accumulators per cell (`Σw, Σwx, Σwx², Σwy, Σwxy`) and nothing else.
Weight each sample by the local curve slope,
`w = (dOCV/ds)² / σ_V²` — which makes the plateau discount itself
automatically, and makes the hard gate merely an optimisation. Apply an
exponential forgetting factor once per day (λ ≈ 0.98, ~50-day memory) so
genuine capacity fade can move the estimate instead of being averaged away
against three-year-old anchors.

**Numerics.** The FPU here is `fpv4-sp-d16` — **single precision only**, so
`double` is soft-float and expensive (`CMakeLists.txt:25`). Keep `Q` as an
`int64` in mAs, re-origin the window whenever `|x|` grows large, and run the
accumulators in `float`. Do not reach for `double`.

**Current integration error** is dominated by sensor *offset*, not by the
1 Hz sampling. A 50 mA offset integrates to 1.2 Ah/day — 0.43 %/day on a
280 Ah pack. Anchors erase the accumulated error, but the important move is to
**estimate the offset itself** as a shared parameter and subtract it, which
turns anchor scarcity from a threat into an asset. That argument, with its
numbers, is §5.9 — it is what separates this from the JK's own estimator, and
it should be read before treating the anchor rate as a requirement.

### 5.3 The anchor gate

A sample enters the fit only if **all** hold:

| Condition | Threshold (starting point, config) | Why |
|---|---|---|
| Curve steep enough | `\|dOCV/ds\| > 4 mV/%` | excludes the plateau |
| Current small, or IR-corrected | `\|I\| < C/50`, else use `V_ocv = V_i − I·R_i` | terminal voltage ≠ OCV under load |
| Relaxed | ≥ 10 min since `\|I\|` last exceeded C/20, when using the rest path | LFP relaxation is slow |
| Balancer accounted for | `BalanSta == 0`, **or** cell is neither `MaxVolCellNbr` nor `MinVolCellNbr`, **or** §5.5's correction applied at reduced weight | the balancer breaks the shared-coulomb identity — but only for two known cells (§2.5), so all but two stay clean even mid-balance. Deliberately stricter than §5.5's bookkeeping gate: that one keys on the duty register, this one on `BalanSta`, because an anchor is worth being conservative about and a coulomb is not |
| Sense path trustworthy | `CellWireRes_i` below threshold when that cell is the balance source/sink | balance current through a degraded lead adds an IR error to the very voltage being used as OCV |
| Temperature in band | 10–35 °C, and recorded with the sample | capacity and OCV are both temperature-dependent |
| No active protection | `Alarm` clear of the voltage/current bits | the pack is not in a normal operating state |

The gate is the whole design. An estimator that admits plateau samples will
converge confidently to a wrong answer, which is worse than not converging.

### 5.4 Per-cell resistance — the independent, always-available signal

This one needs no anchors and works from day one. On each sample step where
`|ΔI|` exceeds a threshold (a solar ESS supplies inverter load steps for
free), fit through the origin:

```
R_i = Σ(ΔI · ΔV_i) / Σ(ΔI²)
```

Two accumulators per cell, binned by temperature. Rising `R_i` over months is
the earliest and most reliable degradation indicator available here — it
moves long before capacity does, and it does not wait for a deep cycle.

And because the JK *separately* reports measured **balance-lead** resistance
(§2.2), the two together separate three failure modes rather than one. Note
these are different circuits: our `R_i` is the main power path (busbar,
terminal, cell internals, seen by the pack current), the JK's `CellWireRes_i`
is the sense/balance harness (seen only by balance current).

| `R_i` (ours, power path) | `CellWireRes_i` (JK, balance lead) | Reading |
|---|---|---|
| rising | flat | cell internals aging, or a loosening **main terminal / busbar** |
| flat | rising | balance harness degrading — balancing gets weaker, and that cell's voltage reads high while balancing |
| rising | rising | shared cause: one terminal that carries both |
| flat | flat | healthy, or the balancer never ran (§2.2 — check `BalanSta` history before concluding) |

Separating "cell internals" from "main terminal" needs a third input the JK
does not have, so `R_i` rising alone is a *dispatch a human* signal rather
than a diagnosis. That is still worth far more than what exists today.

### 5.5 The balancer — a correction, not just an exclusion

The unit is a **JK PB2A16S…**: the "2A" is the balance current, and the PB
line is JK's **active** (charge-transfer) balancer. Active balancing moves
charge *between* cells, so per-cell coulomb counts diverge from the shared
pack integral. At 2 A running for hours at the top of charge that is
amp-hours of divergence — enough to destroy a per-cell capacity fit outright,
and it happens exactly where the most valuable anchors are.

**The good news, from §2.5: the balancer is observable.** It acts on
`MaxVolCellNbr` and `MinVolCellNbr` — two cells, never more — at
`BalanCurrent`, re-selecting the pair about every 5 s, and all four of those
facts arrive in the same frame as the cell voltages. That is not an assumption
about what the balancer probably does; it is what the firmware's scan loop
does, line by line. So the per-cell charge bookkeeping becomes:

```
for every cell i:      q_i += I_pack · Δt
if duty != 0:          q_[MaxVolCellNbr] −= I_bal · Δt          /* BalanSta == 2 */
                       q_[MinVolCellNbr] += I_bal · Δt · η      /* BalanSta == 1 */
```

Note the gate is the **duty register**, not `BalanSta`. `BalanSta != 0` marks
the balancing phase, which continues between bursts; only the duty says charge
is moving right now (§2.5, §5.5a).

— that being the **cell↔cell** form. If the topology turns out to be
cell↔pack (§2.5, undecided), the same two lines apply plus a `∓ I_bal·Δt/N`
spread across every cell, which is a small correction to the untouched ones
and no change at all to the two that matter. Either way the structure is the same and
the phase-0 log picks the variant.

`η` is the converter's transfer efficiency — unknown, order 0.8–0.9, and a
free parameter the replay harness in §8 can fit rather than guess. The only
other approximation left is the ≤ 1 s of aliasing against the balancer's own
5 s re-selection. Both are small next to the amp-hours the naive model gets
wrong.

Three responses, in order of preference:

1. **Correct, and keep the anchor.** Apply the bookkeeping above and admit the
   sample at moderately reduced weight. The default, because it keeps the
   top-knee anchors that make the whole method work.
2. **Exclude** (`BalanSta != 0` → no anchor) stays as the conservative
   fallback and as the phase-2 starting point, since it needs no `η` and no
   trust in the correction. Expect it to converge much more slowly.
3. **Suppress during a learning window** — write `BalanEN = 0` for the
   duration of a scheduled learn. Still **blocked**: the JK firmware has no
   FC 0x06 handler at all (`protocol-rs485-modbus.md` §2.1) and PeriphNet's
   config compiler rejects `writable` on anything wider than one register,
   while every JK setting is 32-bit. Now much less attractive anyway —
   deliberately disabling a safety function is a poor trade for a correction
   we can just compute.

Two secondary signals sharpen it further:

- **`DischargPWMDutyCycle` (`0xE2`)** is the gate that makes the transfer
  integrable at all — it is zero between bursts, where `BalanSta` is not.
  §5.5a is the error budget that follows from it.
- **`CellWireRes_i`** bounds the sense-path IR error on the very cell being
  balanced, and the magnitude is not subtle. The JK senses and balances
  through the same harness, so at 2 A even a healthy 20 mΩ lead puts **40 mV**
  between the cell and what the ADC reads — against a §5.3 gate whose whole
  steep-region criterion is 4 mV per percent of SOC. At the firmware's own
  260 mΩ alarm threshold it would be volts, which is why the alarm exists.
  Gate on it hard: a cell that is currently the balance source or sink does
  not supply an OCV anchor unless its lead resistance is low *and* the
  correction is applied.

**Consequence for scope:** pack-level capacity and SOC are robust regardless
— charge is conserved within the pack, less transfer losses. The per-cell case
is what the observability buys: the balancer touches **exactly two cells at a
time and names them**, so the other `N−2` (14 of 16 on these packs) keep a
clean shared-coulomb identity even mid-absorption. Per-cell capacity is
therefore tractable on ordinary cycling rather than needing balancer-idle
windows — *provided* the correction survives contact with real data, which is
what the phase-0 log and the replay harness exist to decide. Per-cell
resistance (§5.4) is unaffected either way.

### 5.5a Measuring the balance transfer, and what its error actually is

The bookkeeping above needs one number per episode: **how many amp-hours left
`MaxVolCellNbr` and arrived at `MinVolCellNbr`.** It is worth being precise
about how well that can be known, because it is the difference between per-cell
capacity being a real measurement and being a plausible-looking guess.

There are **two independent estimators**, and having two is the whole point —
one is precise but has an unknown scale, the other is unbiased but noisy. Each
alone is a guess; together they give a bounded error.

#### A — integrate the sensed balance current

`BalanCurrent` is better instrumentation than it looks. From §2.5's routines:

- It is a **difference of two ADC reads** on one channel — a baseline taken
  before the converter is enabled, subtracted from the reading during. Offset
  and drift cancel by construction, which is the opposite of the pack current
  sensor's situation (§10).
- The scaling is `(ΔmV × 1000) / 510`, i.e. an effective transimpedance of
  **0.510 V/A** (shunt × amplifier gain), off a 12-bit ADC with a 3000 mV
  reference. One LSB is `3000/4096/0.510` = **1.44 mA**; on a 2 A transfer that
  is 0.07 %.
- The converter is **current-regulated to `CurBalanMax`**, so within a burst the
  current is close to constant and close to a value we already know.
- `BalanSta` says which cell it belongs to, unambiguously (§2.5).

The error is therefore not in the current. It is entirely in **for how long**:

| Term | Size | Why |
|---|---|---|
| Duty cycle | **the dominant term** | bursts are ≤ 1 s inside a scan pass of roughly 4–5 s, and the duty register is zeroed between them. Integrating `BalanCurrent × Δt` without gating over-counts by the pass/burst ratio — 2× to 5×, systematically |
| Sample-fraction estimate of duty | ≈ ±1 % | gate on `duty != 0` and simply count the fraction of samples that are on. Bernoulli: over an 8 h window at 1 Hz, `N` = 28 800 and `p` ≈ 0.25 give `σ_p` = 0.26 %. Duty is *measurable*, not a fudge factor |
| Phase-locking | unquantified | that estimate is only unbiased if the poll is not commensurate with the scan pass. Check the run-length distribution of `duty != 0`; a scan pass whose length varies with convergence makes lock-in unlikely, but do not assume it |
| Transimpedance tolerance | ±2 % | shunt plus gain resistors, uncalibrated |
| Transfer efficiency `η` | unknown, ~0.8–0.9 | matters only on the sink side |

**One catch, and it is the reason §10 item 3 matters.** The low-side duty lives
at `0xE2` and is published. The high-side duty lives at `0x00E0` — the register
the snapshot builder appears to skip. `BalanSta` does *not* substitute for it:
it stays non-zero between bursts, so it marks the balancing *phase*, not the
transfer. If `0xE0` really is dead on the wire, the high-side (source) duty has
no direct observation, and estimator A can only measure the sink side directly.
The source side then has to come from charge conservation (below) or from B.

#### B — measure the transfer by its effect on the untouched cells

This one needs no duty, no `η` and no trust in the current sense at all. Over an
interval, with `ΔQ` the shared pack charge integral:

```
b_i = C_i · Δs_i − ΔQ
```

`b_i` is the net balance charge into cell *i*, and it is **zero for every cell
the balancer did not touch** — all but two at any moment, 14 of 16 on these
packs (§5.5). So the untouched cells calibrate `ΔQ` and supply `C_i`
through the §5.2 regression,
and the two touched cells' residual against that reference *is* the transfer.
No circularity: a cell contributes to its own `C_i` only from intervals in
which it was untouched.

Error budget, for a 280 Ah cell over one top-of-charge episode:

```
σ(b_i)² = (Δs_i · σ_C)²  +  (C_i · σ_Δs)²          σ_Δs = √2 · σ_V / k
```

with `k = dOCV/ds`. At the top knee `k` ≈ 20 mV per % SOC (2000 mV per unit
fraction). Taking `σ_V` = 3 mV, `σ_C` = 2 % and `Δs` = 0.05 over the episode:

- voltage term: `280 × √2 × 3/2000` = **0.59 Ah**
- capacity term: `0.05 × 5.6` = **0.28 Ah**
- total **σ(b_i) ≈ 0.65 Ah**

Against a typical episode — 2 A at ~40 % duty for 4 h ≈ 3.2 Ah — that is
**±20 % on a single episode.** If `σ_V` turns out to be quantisation-limited at
1 mV, it improves to about ±11 % — the capacity term then dominates.

**This method lives or dies on `k`.** In the plateau `k` ≈ 0.2 mV/%, a hundred
times worse, and the same arithmetic gives ±2000 % — i.e. nothing. It works
only at the knee. The saving grace is that **balancing happens at the knee**:
`VolStartBalan` is set to 3.450 V on board #1, so the balancer only runs where
the curve is steep enough to measure it.

#### C — use B to calibrate A, which is the actual answer

A is precise per-sample with an unknown scale; B is unbiased per-episode with
noise. So fit **one scalar α per direction** across episodes:

```
b_i (from B)  ≈  α · ∫ I_bal · 1{duty ≠ 0} dt
```

α absorbs `η`, the duty estimate's residual bias, the transimpedance tolerance
and the source/sink sense polarity in one measured number instead of four
assumed ones. Per-episode noise of ±20 % averages down as `1/√N`:

| Episodes | α known to |
|---|---|
| 1 | ±20 % |
| 9 | ±7 % |
| 25 | ±4 % |
| 100 | ±2 % |

An "episode" is a balancing phase that also produced a usable knee anchor, so
the calendar rate is the anchor rate of §5.9, not the balancing rate — the
balancer runs above `VolStartBalan` far more often than the pack reaches an
anchor. Do not assume one per day.

With α known, estimator A gives continuous balance charge at roughly
**±5 %**, which is well inside what the per-cell capacity fit needs — the
balance transfer over an episode is a few percent of cell capacity, so a ±5 %
error on it is a fraction of a percent of `C_i`.

#### The free consistency check

Charge is conserved, so across any window the estimated transfers must satisfy

```
Σ_i b_i  =  −(1 − η) · Σ |b_source|   ≤ 0
```

A positive sum means the model is wrong — wrong cells, wrong sign, or a missed
episode. It costs nothing, needs no extra data, and it is the single best
guard against the estimator quietly drifting into fiction. It also recovers the
source-side transfer when `0x00E0` is unreadable: `b_source ≈ −b_sink / η`.

The second check is closure — over a full cycle returning to the same pack SOC,
the pack coulomb count must come back to itself, and any non-closure bounds the
total accounting error including the balance term.

#### Two error terms the JK's own scan loop removes

- **Sense-lead IR while balancing.** At 2 A through even a healthy 20 mΩ
  balance lead there is 40 mV between the cell and the ADC — 2 % SOC at the
  knee, 5.6 Ah on a 280 Ah cell, which would dwarf every term above. **The JK
  avoids it for us**: in the scan loop (§2.5) the balance routine runs *and
  zeroes its duty* before that cell's voltage is read. Reported cell voltages
  are balancer-off readings by construction. This is worth re-verifying on
  hardware, because it is load-bearing.
- **Frame skew.** The flip side of the same loop: cell voltages are written one
  cell at a time across a scan pass, so the 16 voltages in one Modbus frame are
  spread over ~4–5 s and are up to a pass old, while `BalanCurrent` and the duty
  are instantaneous. During absorption — steady current, which is when anchors
  are taken — this is harmless. Under a swinging load it is not, and it is
  another reason §5.3 gates on low and stable current rather than merely
  correcting for it.

### 5.6 Pack SOC as the weakest link

Once per-cell `(s_i, C_i)` exist, the pack numbers stop being averages:

```
Ah_to_empty = min over i of ( s_i · C_i )
Ah_to_full  = min over i of ( (1 − s_i) · C_i )
SOC_pack    = Ah_to_empty / (Ah_to_empty + Ah_to_full)
```

`s_i` is defined against *usable* endpoints (the configured `VolSOC0` /
`VolSOC100`, or our own cutoffs), not absolute chemistry limits. This is the
honest definition for a series pack, and it is what the inverter needs: the
pack stops when the *first* cell stops.

The same per-cell headroom yields a genuinely better `0x351` (CCL/DCL) than
a fixed table: taper charge current as the first cell approaches `VolCellOV`,
and discharge current as the first cell approaches `VolCellUV`. That directly
attacks the classic failure on these installs — the JK trips cell OV
protection and drops the pack because the charger had no reason to back off.

Per-cell SOH is then `C_i / C_nameplate`. Report both `min` (what the pack
can actually deliver) and `mean` (what the cells average) — they diverge as a
pack ages, and the divergence is itself the interesting number.

### 5.7 Confidence, and never being confidently wrong

Every published value carries a confidence derived from the fit: number of
admitted anchors, the SOC span they cover, the regression residual, and the
age of the most recent anchor. **Below threshold the module publishes the
JK's own values instead**, and says so.

This is not a nicety. The CAN frames drive an inverter that will drop the
battery on bad data, and autonomy is a hard requirement
([design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
§1). A new estimator earns its way onto the control path by agreeing with the
JK for a season first — see §9 phase 4.

### 5.8 Persistence

State splits by how fast it changes, and the split is a wear decision:

- **Fast** — `s_i` and the charge-integral origin. Changes continuously,
  must survive a reboot. Use the **`wg_time` pattern**: an append-only ring
  of small fixed slots in one nvDb area, one write per 10–15 min. Rewriting a
  whole record at that cadence would be ~35 000 erases/year on one area,
  which a 100 k-cycle NOR part will not carry for a decade.
- **Slow** — `C_i`, `R_i`, the five regression accumulators per cell, and the
  anchor history. Changes only when an anchor lands. One CRC'd, versioned
  `sNvRecord` (`App/nv_record.h` — versioning is USER-side policy; nvDb never
  learns what a version is), rewritten on anchor events only.

Both under a new `nvdbUser_packModel`, appended to `eNvDbUser` — **appended**,
never inserted, since the enum is persisted in the layout
(`Shared/NvDb/nvdb.h:97`).

On restart: restore `s_i` from the ring, then **widen the uncertainty by the
unknown downtime** rather than trusting it. If the pack was loaded while we
were down, the restored value is wrong and only a fresh anchor can say so.
Cross-check against the JK's own restored `SOCCapRemain` (independent
integrator, §3.2) and against terminal voltage *if* it happens to sit in a
steep region. Never re-seed linearly — that is the mistake being corrected.

### 5.9 Anchor scarcity and drift — how this actually differs from the JK

A fair objection to everything above: **structurally this is still coulomb
counting corrected at the knees, which is what the JK does.** A solar ESS
rarely reaches the bottom, reaches the top more often but far from daily, and
open-loop integration between rare anchors drifts. If the answer were only
"better anchors", the objection would stand.

It is not only better anchors. There are four differences, and the third is the
one that matters.

| | JK | This |
|---|---|---|
| Anchor form | threshold: max cell ≥ `VolSOC100` for 3 s | OCV lookup, weighted by local curve slope |
| Anchor value | binary — `remaining := full` | an SOC estimate carrying a σ |
| A charge that stops at 90 % | **nothing** | a usable ±2.1 % anchor |
| **Current-sensor offset** | **never estimated** | **estimated from anchor pairs — §5.9.2** |
| Granularity | one pack number, 1 % integer | per cell |
| Uncertainty | none reported | explicit, grows between anchors |
| Capacity update | only on a complete empty→full inside a timeout | continuous weighted regression |

#### 5.9.1 What the drift actually is

Two error terms, and they behave completely differently:

- **Gain error** integrates with *throughput* and largely self-cancels over a
  cycle: a +2 % error on 150 Ah in and −2 % on 150 Ah out nets to almost
  nothing. It is not the problem.
- **Offset error** integrates with *time*, in one direction, whether or not the
  pack is doing anything. It is the entire problem.

On a 280 Ah pack:

| Offset | Per day | Over 30 d | Over 60 d |
|---|---|---|---|
| 50 mA | 0.43 % | 12.9 % | 25.7 % |
| 20 mA | 0.17 % | 5.1 % | 10.3 % |
| 5 mA | 0.043 % | 1.3 % | 2.6 % |
| 2.5 mA | 0.021 % | 0.64 % | 1.3 % |

So the objection is quantitatively right: an uncharacterised 50 mA offset and a
two-month winter with no top-of-charge makes the SOC meaningless. The JK, which
never estimates its own offset, has exactly this failure mode and no way to
know it is happening.

#### 5.9.2 Anchor scarcity is self-limiting, which is the counter-intuitive part

Two anchors separated by time `T` measure the offset directly:

```
ε = ( ∫I dt  −  C·(s₂ − s₁) ) / T
```

The numerator's error is fixed — it is the two-anchor uncertainty,
`√2 · C · σ_V / k` = **0.59 Ah** at the top knee, the same term §5.5a uses —
while the denominator grows with the gap. **A sparser anchor gives a *more*
precise offset**, not a less precise one:

| Anchor gap | Offset resolved to | Residual drift after correction |
|---|---|---|
| 7 d | 3.5 mA | 0.030 %/day |
| 14 d | 1.8 mA | 0.015 %/day |
| 30 d | 0.8 mA | 0.007 %/day |
| 60 d | 0.4 mA | 0.004 %/day |

And the two effects cancel exactly. Residual drift over the *next* gap of the
same length is `σ_ε × T` = 0.59 Ah — **the anchor uncertainty itself, 0.21 %
SOC, whatever the gap length.** Once the offset is being learned, the length of
the anchor drought stops mattering to first order.

The honest caveat: this holds only while `ε` is constant across the two
intervals. It is not — it drifts thermally and with age. So model it as a
slowly-varying state (a random walk), and **its own drift rate is what really
bounds long gaps**. That rate is measurable from consecutive `ε` estimates, so
it becomes a number in the confidence budget rather than an assumption. This is
the single most valuable thing phase 2 produces, and it needs no knee at all
after the first pair.

Implementation is nearly free: `ε` is one **shared** parameter across all 32
cells' equations, so it is far better determined than any per-cell quantity.
Either add it to the §5.2 fit directly, or estimate it pack-level from anchor
pairs and feed it back — the second is simpler and loses little.

#### 5.9.3 Graded anchors mean partial charges count

The JK's anchor is a step function: below `VolSOC100` it learns nothing at all.
An OCV lookup weighted by slope degrades gracefully instead:

| Where the charge stopped | `dOCV/ds` | One anchor, on a 280 Ah cell |
|---|---|---|
| ~97 % (top knee) | 20 mV/% | 0.42 Ah — **0.15 % SOC** |
| ~90 % (shoulder) | 2 mV/% | 4.2 Ah — **1.5 % SOC** |
| 30–80 % (plateau) | 0.2 mV/% | 42 Ah — 15 % SOC, i.e. nothing |

(These are `C · σ_V / k` for a single reading. A *pair* of anchors, which is
what §5.9.2 and §5.5a difference, carries √2 times as much — 0.59 Ah at the
top knee.)

A 90 % shoulder is a mediocre anchor. It is also **nine times better than a
month of uncorrected 50 mA drift**, and a solar ESS reaches 90 % vastly more
often than it reaches 100 %. Those anchors are worth taking precisely because
the good ones are rare.

The plateau row is the boundary of what is possible: no algorithm extracts SOC
from a flat curve. Plateau OCV is therefore used as a **contradiction check**,
never as an estimate — if the filter claims 95 % while a rested cell sits at
3.29 V, something is wrong and confidence should collapse.

#### 5.9.4 SOH is not exposed to this, and the reason is worth stating

The objection applies to **SOC**, which must be right continuously. It largely
does not apply to **capacity and SOH**, for two reasons:

- Capacity moves over *months*. An anchor every few weeks is not a compromise;
  it is oversampling.
- **The knee is a readout amplifier, not a required operating point.** Two cells
  differing by 2 % in capacity diverge by 1.1 % SOC over 150 Ah of throughput.
  In the plateau that divergence is 0.22 mV — invisible, below the 1 mV
  quantisation. At the top knee the *same* accumulated divergence reads
  **22 mV**, a hundred times larger and unmistakable. The divergence accrues
  silently during weeks of plateau cycling and is *read out* the moment the pack
  next touches the knee.

Two caveats. The balancer erases exactly this divergence at the top — which is
why §5.5a exists, so it can be added back rather than lost. And **differential
self-discharge** mimics capacity mismatch; the two separate because
self-discharge divergence accumulates with *time* while capacity divergence
accumulates with *throughput*, so regressing on both splits them — the same
trick that separates balancing from capacity.

#### 5.9.5 What this obliges the implementation to do

- **Estimate `ε` and persist it** (§5.8's slow record). Without it, everything
  above is just the JK with a nicer OCV table.
- **Measure `ε` honestly at rest too.** §10's dead-band question decides whether
  `BatCurrent` can even show a resting offset. If it clamps small currents to
  zero, use the raw shunt-amplifier voltages at `0xD8`/`0xDA` — which is why
  they are now in the config (§2.6).
- **Carry σ(SOC) as a first-class output**, growing with time since the last
  anchor and collapsing at each one. The CAN `0x355` value can then be reported
  conservatively, and §5.7's fallback to the JK's own number triggers on a
  number instead of a guess.
- **Never re-seed from the plateau.** That is the §3.2 mistake. When confidence
  is gone, say so; do not manufacture a value from a flat curve.

## 6. What it costs

| Resource | Estimate | Notes |
|---|---|---|
| RAM | **≈ 2.2 KB** `.bss` | sized for the JK's maximum 32 cells (these packs are 16S) × 52 B — capacity, SOC, resistance, 5 capacity accumulators, 2 resistance accumulators, counters — plus two frame snapshots and module scalars |
| RAM placement | **main SRAM, not CCM** | CCM is ~92 % full and this is not hot. Main SRAM is ~86 KB of 128 KB used |
| CPU | **< 50 µs per 1 Hz frame** | ≤ 32 × (OCV table lookup + two rank-1 regression updates) ≈ 2000 single-precision flops at 168 MHz with the FPU |
| Flash | **6–8 KB** | application has ~97 KB of its 480 KB free |
| Ext flash | one nvDb area, 16 KB suggested | ring + record, §5.8 |
| Bus | **≈ 2 %** for one BMS, 4 % for two | measured from the shipped config, [modbus.md](modbus.md) §11a.13. §2.3 has the per-transaction arithmetic |
| New task | **none** | runs inside the existing Modbus subscriber callback, or on `mqttTask`-style deferral if the callback contract forbids the work |

Nothing here is close to a limit. The scarce resource in this project is CCM,
and this design does not touch it.

---

## 7. How it would fit PeriphNet

A new module, **`App/Pack/`**, an ordinary Modbus consumer — the same status
the MQTT bridge now has:

| File | Role |
|---|---|
| `pack.h` | the only consumer header: `Pack_Init()`, `Pack_GetState()`, `Pack_GetCell()`, confidence |
| `pack_source.c` | subscribes via `Modbus_Subscribe`, assembles coherent frames |
| `pack_estimator.c` | the per-cell filter and the two regressions |
| `pack_ocv.c` | OCV table + inverse lookup + temperature correction |
| `pack_store.c` | the nvDb ring + record of §5.8 |

**Frame coherence — settled, and not the way this section first assumed.**
`service_block` (`modbus_engine.c:314-346`) dispatches `mbEvt_txn` immediately
after the wire returns and *then* decodes and emits the points. So **a txn is a
LEADING marker, not a closing one**: everything between txn *N* and txn *N+1*
belongs to txn *N*. Worse for the naive reading, **a transaction is a read
block, not a device poll, and there is no end-of-sequence event at all** — one
timer firing derives 1..N blocks and emits one txn each.

That turns out not to matter, because of how the config was grouped: the **live
table is exactly 8 points in one block, so it is exactly one transaction**, and
current, voltage, SOC, remaining and the balancer fields therefore arrive
atomically. Cells span three tables and are anyway skewed ~4–5 s by the JK's own
scan (§5.5a).

> **Strict whole-device coherence is neither achievable nor needed.** What a
> consumer needs is a per-field timestamp and a stated skew budget. The one
> group that must be coherent already is.

[design_battery_pack.md](design_battery_pack.md) Part I §8 carries the same
finding, and its Part II §10.4 is the API that acts on it — per-group ages
rather than one age for a whole pack.

Everything else is existing machinery:

- **Config** — the JK register map is an ordinary capability in the uploadable
  Modbus config JSON. The OCV table, nameplate capacity, gate thresholds and
  cell count belong in their own uploaded JSON record, not in code. There is
  no built-in default worth having: a pack the firmware has never met should
  say "unprovisioned", exactly as the Modbus config does.
- **Outputs** — `App/Can/bms_sim.c` fills `0x355` / `0x351` from
  `Pack_GetState()` instead of from raw JK fields (gated on confidence, §5.7);
  the MQTT bridge publishes per-cell capacity, SOH, resistance and confidence
  as ordinary HA entities; `GET /api/pack/status` and a `pack` CLI command for
  inspection, matching `sysmon` and `nvdb`.
- **`App/Data/telemetry.c`** is currently reserved with no producers or
  consumers, and this is plausibly what it was reserved for — but its
  `sEnergyTelemetry` is inverter-shaped and has no room for per-cell data.
  Either extend it with a separate pack structure or leave it alone; do not
  force the pack model through a struct built for a Solis.

---

## 8. How it gets tested

The project already tests flash-shaped things host-native against a
NOR-faithful mock. The same approach applies, and it is what makes this
tractable without a board:

1. **A synthetic pack model** in `tests/` — 16 cells with individual
   capacities, resistances and an OCV table; a simulated 2 A active balancer
   that moves charge between the max and min cells as §2.5 describes;
   measurement quantisation (1 mV, 1 mA) and noise; a current-sensor offset
   knob. Drive it with recorded or synthesised solar-day current profiles.
   Assertions: converges to true `C_i` within X % after N simulated days;
   **never reports high confidence while wrong** (the important one); the gate
   rejects plateau samples; a power cut at any step leaves recoverable state.
2. **Replay.** Log real assembled frames from the board (Trice or MQTT) to a
   file and replay them host-side into the same estimator. This is the highest
   value tool in the list — it makes the gate thresholds and forgetting factor
   tunable against the actual pack without touching firmware.
3. **Hardware acceptance.** One deliberate full charge/discharge cycle,
   comparing the estimator's capacity against the measured throughput, and
   against what the JK finally learns for itself when given the one cycle it
   needs.

---

## 9. Suggested order

Each phase is useful on its own and none commits the next.

| # | Phase | Delivers | Risk |
|---|---|---|---|
| 0 | **Observe.** Upload the extended config (§2.6 — already written, compiled host-side, not yet on a board), frames assembled and logged. Persist nothing, publish nothing, change no behaviour. | Answers every §10 hardware question, including the balancer efficiency `η` (§5.5a). Data for the replay harness. | none |
| 1 | **Resistance and balancer telemetry.** §5.4 plus the `CellWireRes` array, plus balance source/sink/current/duty (§2.5) and the §2.6 fields, published to MQTT. | A real per-cell health signal and a visible balancer, immediately — no anchors, no persistence, no estimator. | low |
| 2 | **Pack coulomb + anchors + offset.** Shared integrator, graded anchor gate, and the shared current-offset estimate of §5.9.2. Pack SOC with an explicit σ, published alongside the JK's, not instead of it. | A second opinion, and the offset number — the thing that actually bounds drift. Two anchors is the whole prerequisite. | low — nothing consumes it |
| 3 | **Per-cell regression.** §5.2, §5.5, persistence. Per-cell capacity and SOH published. | The stated goal. | medium — balancer, §5.5 |
| 4 | **Onto the control path.** CAN `0x355`/`0x351` from `Pack_GetState()`, gated on confidence with JK fallback. | The edge-controller win: CCL taper that stops the pack tripping OV. | **highest — a wrong number here drops the battery.** Requires a season of phase-2/3 agreement first |

---

## 10. What to settle before building

**Hardware checks — all answerable in phase 0, most in minutes:**

1. **The second read.** Does the JK actually serve registers 120–145
   (bytes `0xF0`–`0x123`)? The firmware's bound says yes; nothing has ever
   asked it. Everything in §2.6 depends on this one transaction working.
2. **Is `ChargPWMDutyCycle` (`0xE0`) garbage over Modbus?** §2.5 — the
   snapshot builder appears to skip it. **The most consequential unknown for
   §5.5a**, because it shows the duty register is the
   only direct observation of *when* charge is actually moving (`BalanSta`
   stays non-zero between bursts, so it does not substitute). If `0xE0` is
   dead, the high-side transfer has no direct measurement and must come from
   charge conservation instead. Test: read `0xE0` and `0xE2` together at 1 Hz
   through a balancing phase and check that `0xE0` goes non-zero while
   `BalanSta == 2`.
3. **Balancer topology and correction (§2.5, §5.5).** Log `BalanSta`,
   `BalanCurrent`, both duty registers, `MaxVolCellNbr`, `MinVolCellNbr`, every
   cell voltage and `BatCurrent` at 1 Hz through a full absorption phase.
   Three things to extract:
   - **cell↔cell or cell↔pack?** Genuinely open (§2.5) — two converter stages
     on two timers at 60 kHz and 300 kHz, a millisecond-scale cell mux, and a
     vendor description of flyback/capacitive cell-to-cell transfer that the
     code does not contradict. Settle it by observation, not inference: over a
     balancing phase, does the *common mode* of the non-selected cells move
     against the two selected ones? If yes, charge routed through the
     pack; if they sit still, it went cell to cell. §5.5a's estimator B
     produces this as a by-product, so this costs no extra instrumentation.
   - **The duty cycle**, by the sample-fraction method of §5.5a — and the
     run-length distribution of `duty != 0`, which is what says whether the
     1 Hz poll has phase-locked to the ~4–5 s scan pass. A locked poll makes
     the duty estimate biased in a way no averaging removes.
   - the transfer efficiency `η`, and hence α (§5.5a);
   - whether the max/min pair really is piecewise-constant over ~5 s.
   - **that cell voltages really are balancer-off readings** (§5.5a). Look for
     a step in the balanced cell's reported voltage synchronised with
     `BalanSta` changing — there should be none. If there is one of ~40 mV,
     the anchor gate needs the lead-IR correction after all.
4. **Current zero-clamp — the other first-order question, and the one §5.9
   turns on.** Log
   `BatCurrent` at true rest for an hour. If it reads exactly 0 always, there
   is a dead-band and the resting offset is invisible in that register, which
   removes the cheapest of the two ways to estimate `ε`. Cross-check against
   the raw shunt voltages at `0xD8`/`0xDA`, which should still move — that is
   what they were added to the config for. Note the anchor-pair method of
   §5.9.2 works regardless, so a dead-band delays the offset estimate to the
   second anchor rather than preventing it.
5. **Current gain error.** Integrate a known charge (inverter-metered kWh, or
   a measured discharge) and compare against the JK's `SOCCycleCap` delta.
6. **Cell voltage noise floor.** Standard deviation of a resting cell over an
   hour. This sets `σ_V` = 3 mV, the assumption every error budget in §4, §5.5a
   and §5.9 rests on — and the one most likely to be wrong.
7. **Balance-lead resistance sanity.** Live `CellWireRes` values should sit in
   the 5–260 mΩ band the firmware itself expects (§2.2), and be non-zero at all
   — they only update while the balancer runs.

**Design questions:**

8. ~~Modbus event ordering.~~ **Closed** — `mbEvt_txn` leads its samples
   rather than closing them, a transaction is a read block rather than a device
   poll, and there is no end-of-sequence event (§7). The frame boundary is
   therefore "a txn for this device closes the previously open frame", and the
   live table's 8 points in one block are already atomic.
9. **Bus sharing.** Solis at 9600 and JK at 115200 on one pair is
   *expressible*; whether it is acceptable is
   [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
   §5, still open, and phase 0's timing data is what should settle it. The
   shipped config costs 0.53 transactions/s for one BMS and 1.05 for two.
10. **Do we ever want to write to the JK?** Nothing on the critical path needs
    it (§5.5 option 3 is the only candidate, and it is the worst of the three).
    Keep it that way.
11. **Where the OCV table lives.** Proposed: its own uploaded JSON record —
    though `CellType` (§2.6) means the *chemistry* can be read from the pack
    rather than configured, so the record only has to hold the curve for each
    chemistry, not the selection.

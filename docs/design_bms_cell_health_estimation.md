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

> **Revised 2026-08-25 (second pass).** The first pass concluded the JK never
> says which cells it is balancing. That was wrong, and §2.5 now answers it
> from the firmware's scan loop. The same pass mapped the 54 bytes of realtime
> data past `0xEF` that no tooling here reads, settled the balance-lead
> resistance LSB, and settled the firmware tick unit. §5.5 changed materially
> as a result — the balancer is now a *correction*, not just an exclusion.

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
  side**, including per-cell. Three things the JK exposes and no tooling here
  reads: the per-cell **balance-lead resistance** array (§2.2), the identity of
  the two cells the **balancer** is currently working between (§2.5), and 54
  bytes past `0xEF` that include the **charge stage** (Bulk/Absorption/Float),
  the **cell chemistry**, and three more temperature sensors (§2.6).

---

## 2. What the JK actually provides

### 2.1 The measurement surface

One FC03 read of 120 registers from block base `0x1200` returns bytes
`0x00..0xEF` and covers everything below. That is the whole realtime surface;
there is no second read worth making at telemetry rates.

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
| `0xA6` | UINT8 | — | `BalanSta` 0=off 1=charge 2=discharge | derived |
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
| `0xE0` | UINT16 | % | `ChargPWMDutyCycle` — balancer duty, **see §2.5: not populated over Modbus** | measured |
| `0xE2` | UINT16 | % | `DischargPWMDutyCycle` — balancer duty | **measured** |
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

**The realtime block is 294 bytes (147 registers), not 240.** Everything from
`0xF0` on is past the 120 registers the Qt app reads, and is undocumented in
its `jk::rt` map. The 147-register figure is exactly the firmware's own
`quantity + (byteOffset/2) < 0x93` bound (§2.3) — the cap *is* the block size.
Two FC03 reads cover it (registers 0–119, then 26 registers from offset 120);
the final two bytes of `EnableFlags` fall outside the firmware's bound and are
unreachable, which costs nothing.

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

### 2.2 The per-cell balance-lead resistance array is real, and nothing reads it

`jk::rt` in the Qt app leaves a 64-byte hole between `MinVolCellNbr_off`
(`0x49`) and `TempMos_off` (`0x8A`). That hole is **`CellWireRes0..31`, 32 ×
UINT16**, confirmed three ways:

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
per frame. **1 Hz costs under 10 % of the bus**; 2 Hz is affordable if the
estimator ever wants it (it does not — see §5.3, the anchors are what matter,
not the rate).

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
- ~~No indication of which cell is being balanced.~~ **This turned out to be
  wrong — see §2.5.** `MaxVolCellNbr` / `MinVolCellNbr` are not a guess about
  the balancer's target; they *are* the balancer's target, by construction.
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

The vendor app shows it, so the information exists. It is not a hidden
bitmask: **it is `MaxVolCellNbr` and `MinVolCellNbr`, which the balancer uses
as its operands by construction.**

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

**Which cells, is settled. The topology, is not** — and it changes the
bookkeeping in §5.5:

- **cell↔cell**: charge taken from the max cell is delivered to the min cell.
  Only two cells' coulomb counts move; the pack is untouched.
- **cell↔pack**, two independent converters: one bleeds the max cell into the
  pack, the other charges the min cell from it. Then *every* cell's count
  moves a little as well as the two moving a lot.

Two separately-named duty registers and two separate state machines lean
towards cell↔pack; a single shared `BalanCurrent` register that both routines
write leans towards one converter running at a time. **§10 has the
discriminating test** — it is a straight observation, not an inference: watch
whether thirty cells move in the opposite direction to the two, and whether
the pack current sensor sees the balance current at all.

So the complete, exact answer, all from fields already on the wire:

| Question | Field |
|---|---|
| Is balancing happening? | `BalanSta != 0` (the vendor datasource maps 1, 2 **and** 3 all to "ON", so treat it as a boolean, not the tri-state the PDF implies) |
| Which cell is charge being taken *from*? | `MaxVolCellNbr` (`0x48`) |
| Which cell is it going *to*? | `MinVolCellNbr` (`0x49`) |
| How much? | `BalanCurrent` (`0xA4`, mA) |
| How hard is the converter working? | `DischargPWMDutyCycle` (`0xE2`, %) — and `ChargPWMDutyCycle` (`0xE0`), with the caveat below |

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

**All of this is now in the config** — `configs/gen_jk_config.py` went from 87
points to 114 on 2026-08-25, including the 16-cell balance-lead array, its
alarm bitmask, both balancer duty registers and everything listed above. See
[modbus.md](modbus.md) §11a.13 for what changed and what it costs on the wire
(≈2 % bus duty for one BMS, 4 % for two). Both generated files were compiled
through the real `MbCfgCompile` on the host; **neither has been uploaded to a
board yet.** That upload is phase 0 (§9), and it is now a config upload rather
than any firmware work — except for one two-line addition, `unit: "ohm"`
(DLMS code 38), without which the resistance array has no unit to report.

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

### 3.1 The claim under test

"JK's SOC is a linear estimation, which for LiFePO4 is useless."

**Half right, and the half that is right matters.** The linear estimation
exists and is exactly as bad as expected — but it is the seed, not the
running estimate. What runs is coulomb counting. The practical damage comes
from *when* the seed fires and from how rarely the anchors that correct it
are reachable.

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
while the cell is very likely above 95 %. The user's characterisation of this
as useless is correct.

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
| `CellWireRes` array | **Yes** (1 mΩ/LSB, §2.2) | direct measurement, currently unread — but of the *balance lead*, and only fresh while the balancer runs |
| `MaxVolCellNbr` / `MinVolCellNbr` | **Yes, and more than they look** | argmax/argmin *and* the balancer's two operands (§2.5) |
| `ChargeStatus2` (Bulk/Abs/Float), `CellType` | **Yes** | firmware state, past the region anything here reads (§2.6) |
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

The working slope:

- **30–80 % SOC: ≈ 0.2 mV per 1 % SOC.** With a realistic ±5 mV of combined
  measurement and cell-offset error, voltage alone locates SOC to about
  **±25 %**. No filter fixes this; the information is not present.
- **Below ~10 %: ≈ 10 mV/%.** Error ≈ ±0.5 %.
- **Above ~95 %: ≈ 15 mV/%.** Error ≈ ±0.3 %.

Two conclusions drive the whole proposal:

1. **Voltage is an anchor, never a continuous estimate.** Any scheme that
   maps volts to SOC in the plateau — the JK's seed, or a naive Kalman filter
   with an OCV measurement model — is fitting noise.
2. **The knees are where all the information is**, and a solar ESS visits the
   top knee most sunny days. That is enough, if you spend the visits well.

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
280 Ah pack. That is exactly what the anchors exist to erase, and it sets the
useful anchor interval at "at least weekly", which a solar site clears
easily.

### 5.3 The anchor gate

A sample enters the fit only if **all** hold:

| Condition | Threshold (starting point, config) | Why |
|---|---|---|
| Curve steep enough | `\|dOCV/ds\| > 4 mV/%` | excludes the plateau |
| Current small, or IR-corrected | `\|I\| < C/50`, else use `V_ocv = V_i − I·R_i` | terminal voltage ≠ OCV under load |
| Relaxed | ≥ 10 min since `\|I\|` last exceeded C/20, when using the rest path | LFP relaxation is slow |
| Balancer accounted for | `BalanSta == 0`, **or** cell is neither `MaxVolCellNbr` nor `MinVolCellNbr`, **or** §5.5's correction applied at reduced weight | the balancer breaks the shared-coulomb identity — but only for two known cells (§2.5), so 30 of 32 cells stay clean even mid-balance |
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
if BalanSta != 0:      q_[MaxVolCellNbr] −= I_bal · Δt
                       q_[MinVolCellNbr] += I_bal · Δt · η
```

— that being the **cell↔cell** form. If the topology turns out to be
cell↔pack (§2.5, undecided), the same two lines apply plus a `∓ I_bal·Δt/N`
spread across every cell, which is a small correction to thirty cells and no
change at all to the two that matter. Either way the structure is the same and
the phase-0 log picks the variant.

`η` is the converter's transfer efficiency — unknown, order 0.8–0.9, and a
free parameter the replay harness in §8 can fit rather than guess. The only
other approximation left is the ≤ 1 s of aliasing against the balancer's own
5 s re-selection. Both are small next to the amp-hours the naive model gets
wrong.

That changes the ordering of the three responses:

1. **Correct, and keep the anchor.** Apply the bookkeeping above and admit the
   sample at moderately reduced weight. This is now the default, and it keeps
   the top-knee anchors that make the whole method work.
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

- **`DischargPWMDutyCycle` (`0xE2`)** says how hard the converter is working,
  which is an independent cross-check on `BalanCurrent` and catches the case
  where the balancer is enabled but achieving nothing (a degraded balance
  lead — §5.4).
- **`CellWireRes_i`** bounds the sense-path IR error on the very cell being
  balanced, and the magnitude is not subtle. The JK senses and balances
  through the same harness, so at 2 A even a healthy 20 mΩ lead puts **40 mV**
  between the cell and what the ADC reads — against a §5.3 gate whose whole
  steep-region criterion is 4 mV per percent of SOC. At the firmware's own
  260 mΩ alarm threshold it would be volts, which is why the alarm exists.
  Gate on it hard: a cell that is currently the balance source or sink does
  not supply an OCV anchor unless its lead resistance is low *and* the
  correction is applied.

**Consequence for scope:** pack-level capacity and SOC were always robust
(charge is conserved within the pack, less transfer losses). The bigger shift
is per-cell: because the balancer touches **exactly two cells at a time**, the
other 30 of a 16S–32S pack keep a clean shared-coulomb identity even in the
middle of an absorption phase. The first pass treated balancing as poisoning
the whole frame; it poisons two cells, and names them. Per-cell capacity
therefore looks tractable on a normal cycle rather than needing weeks of
balancer-idle windows — *provided* the correction survives contact with real
data, which is precisely what the phase-0 log and the replay harness exist to
decide. Per-cell resistance (§5.4) is unaffected either way.

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

---

## 6. What it costs

| Resource | Estimate | Notes |
|---|---|---|
| RAM | **≈ 2.2 KB** `.bss` | 32 cells × 52 B (capacity, SOC, resistance, 5 capacity accumulators, 2 resistance accumulators, counters) + two frame snapshots + module scalars |
| RAM placement | **main SRAM, not CCM** | CCM is ~92 % full and this is not hot. Main SRAM is ~86 KB of 128 KB used |
| CPU | **< 50 µs per 1 Hz frame** | 32 × (OCV table lookup + two rank-1 regression updates) ≈ 2000 single-precision flops at 168 MHz with the FPU |
| Flash | **6–8 KB** | application has ~97 KB of its 480 KB free |
| Ext flash | one nvDb area, 16 KB suggested | ring + record, §5.8 |
| Bus | **< 10 %** of the RS485 port at 1 Hz | §2.3 |
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

**Frame coherence is the one contract to check.** The estimator needs all 32
cell voltages *and* the current from the same transaction. The subscription
surface delivers one `mbEvt_sample` per point plus an `mbEvt_txn` keyed by
`{devOrd, planId, timeTableId}` (`App/Modbus/modbus.h:176-226`). The natural
implementation accumulates samples and closes the frame on the matching
`mbEvt_txn`. That requires the engine to raise every sample of a transaction
before that transaction's `txn` event — **verify this against
[modbus.md](modbus.md) §4 before committing to it**; if it does not hold, the
frame boundary has to come from somewhere else and that is a design change,
not an implementation detail.

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
  `sEnergyTelemetry` is inverter-shaped and has no room for 32 cells. Either
  extend it with a separate pack structure or leave it alone; do not force the
  pack model through a struct built for a Solis.

---

## 8. How it gets tested

The project already tests flash-shaped things host-native against a
NOR-faithful mock. The same approach applies, and it is what makes this
tractable without a board:

1. **A synthetic pack model** in `tests/` — 32 cells with individual
   capacities, resistances and an OCV table; a simulated 2 A active balancer;
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
| 0 | **Observe.** Upload the extended config (§2.6 — already written, compiled host-side, not yet on a board), frames assembled and logged. Persist nothing, publish nothing, change no behaviour. | Answers every §10 hardware question, including the balancer efficiency `η` (§5.5). Data for the replay harness. | none |
| 1 | **Resistance and balancer telemetry.** §5.4 plus the `CellWireRes` array, plus balance source/sink/current/duty (§2.5) and the §2.6 fields, published to MQTT. | A real per-cell health signal and a visible balancer, immediately — no anchors, no persistence, no estimator. | low |
| 2 | **Pack coulomb + anchors.** Shared integrator, anchor gate, pack SOC with confidence. Published alongside the JK's, not instead of it. | A comparable second opinion; proves the gate against a live pack. | low — nothing consumes it |
| 3 | **Per-cell regression.** §5.2, §5.5, persistence. Per-cell capacity and SOH published. | The stated goal. | medium — balancer, §5.5 |
| 4 | **Onto the control path.** CAN `0x355`/`0x351` from `Pack_GetState()`, gated on confidence with JK fallback. | The edge-controller win: CCL taper that stops the pack tripping OV. | **highest — a wrong number here drops the battery.** Requires a season of phase-2/3 agreement first |

---

## 10. What to settle before building

**Hardware checks — all answerable in phase 0, most in minutes:**

1. ~~`CellWireRes` LSB.~~ **Closed** — 1 mΩ per count, confirmed by the
   firmware's own threshold arithmetic (§2.2). What remains is a sanity check
   that live values sit inside the 5–260 mΩ band the firmware itself expects,
   and that they are non-zero at all (they only update while the balancer
   runs).
2. **The second read.** Does the JK actually serve registers 120–145
   (bytes `0xF0`–`0x123`)? The firmware's bound says yes; nothing has ever
   asked it. Everything in §2.6 depends on this one transaction working.
3. **Is `ChargPWMDutyCycle` (`0xE0`) garbage over Modbus?** §2.5 — the
   snapshot builder appears to skip it. Read it alongside `0xE2` while the
   balancer runs; if `0xE0` is stale or nonsensical, that is a genuine vendor
   firmware bug worth writing down.
4. **Balancer topology and correction (§2.5, §5.5).** Log `BalanSta`,
   `BalanCurrent`, both duty registers, `MaxVolCellNbr`, `MinVolCellNbr`, all
   32 cell voltages and `BatCurrent` at 1 Hz through a full absorption phase.
   Three things to extract:
   - **cell↔cell or cell↔pack?** If the thirty non-selected cells drift
     measurably *opposite* to the two selected ones, it is cell↔pack. If they
     sit still, it is cell↔cell. A second tell: does `BatCurrent` show the
     balance current when the pack is otherwise at rest?
   - the transfer efficiency `η`;
   - whether the max/min pair really is piecewise-constant over ~5 s.
5. **Current zero-clamp.** Log `BatCurrent` at true rest for an hour. If it
   reads exactly 0 always, there is a deadband, the offset is hidden inside
   it, and the drift model in §5.2 is optimistic. Cross-check against the raw
   shunt voltages at `0xD8`/`0xDA`, which should still move.
6. **Current gain error.** Integrate a known charge (inverter-metered kWh, or
   a measured discharge) and compare against the JK's `SOCCycleCap` delta.
7. **Cell voltage noise floor.** Standard deviation of a resting cell over an
   hour — this sets `σ_V`, which sets every weight in §5.2.

**Design questions:**

8. **Modbus event ordering** (§7) — are all samples of a transaction raised
   before its `txn` event? If not, where does the frame boundary come from?
   Now slightly harder: a coherent frame spans **two** transactions (§2.1), so
   the estimator must either tolerate the split or read only the first.
9. **Bus sharing.** Solis at 9600 and JK at 115200 on one pair is
   *expressible*; whether it is acceptable is
   [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
   §5, still open, and phase 0's timing data is what should settle it. Note
   the JK poll is now two transactions per second, not one.
10. **Do we ever want to write to the JK?** After §5.5's rewrite, nothing on
    the critical path needs it. Keep it that way.
11. **Where the OCV table lives.** Proposed: its own uploaded JSON record —
    though `CellType` (§2.6) means the *chemistry* can be read from the pack
    rather than configured, so the record only has to hold the curve for each
    chemistry, not the selection.

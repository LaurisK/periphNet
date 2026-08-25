# Battery Pack module — storyline and API

> Status: **Part I storyline — direction** (agreed 2026-08-25).
> **Part II API — design**: the contract an implementation must meet. Not
> implemented.
>
> Staged deliberately: storyline → API → details → implementation. Part I is
> settled and should be argued with only if a decision in it is wrong. Part II
> is what implementation will be held to.
>
> Related: [design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md)
> (what lives *inside* a pack, later), [modbus.md](modbus.md) §2/§4 (the surface
> `pack_jkbms` consumes), [pylontech_can_protocol.md](pylontech_can_protocol.md)
> (what `pack_pylontech` consumes),
> [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
> (why autonomy constrains all of it).

---

# Part I — Storyline

## 1. Why this exists

PeriphNet is becoming an edge controller. Eventually a **battery cluster** will
present one synthetic battery to the Solis inverter over CAN, aggregating
several real packs. That cluster is future work. What it needs first is a thing
to aggregate: **a pack object that is the same shape whatever is inside it.**

Two facts set the whole design:

1. **A pack is not necessarily a JK BMS.** JK over Modbus RTU is the first
   *type*, not the definition. Pylontech is the second — a Dyness Powerbrick
   speaks it. Anything that bakes JK registers into the pack object is wrong the
   day the second type arrives.
2. **A pack must report its own condition faithfully**, including the ways it
   can stop contributing. It does not need to know what a cluster is to do that.

So: **turn "some vendor's battery, reachable somehow" into "a pack, with a state
and a condition, that anything can consume."**

### Decisions taken

| Question | Decision |
|---|---|
| Cluster scope | **One cluster per board.** No inter-board transport. |
| Pack types | **Both push and pull.** *"All packs need to expose the same API so one cluster could consume any pack data."* |
| **Membership** | **Not the pack's concept.** A pack does not know whether it is in a cluster. It reports its own state; **the consumer decides what to consume.** |
| Disconnection | **Three-fold** — commanded, pack-detected, consumer-sensed (§4). |
| Object model | A pack **type** (blueprint) and pack **instances** of it. |
| Existence & identity | **Configuration declares packs; types bind to them.** Operator-assigned name. |
| Control | **Capability-gated commands, day one.** |
| Where it runs | **One shared task**, on the ZhagaFW `App/Func/func.c` pattern (§7). |
| Testing | **Implementation first**; a test pack type is deferred. |

## 2. The site as it actually is

The 2026-08-18 read-out from three live JK BMS
(`configs/readout_2026-08-18.tsv`) — the only ground truth available.

| | board #1 "Zaliakalnis" | board #2 "sodas" slave 2 | board #2 slave 15 |
|---|---|---|---|
| Model | JK_PB2A16S15P | JK-PB2A16S30P | JK-PB2A16S30P |
| Design capacity | 261 Ah | **660 Ah** | **300 Ah** |
| Learned full capacity | 261 Ah | 594.7 Ah | 300 Ah |
| SOH | 100 % | **90 %** | 100 % |
| Pack voltage | **52.502 V** | **51.206 V** | **51.212 V** |
| Pack current | −3.446 A | −0.384 A | −2.738 A |
| SOC / remaining | 52 % / 136.5 Ah | 7 % / 39.6 Ah | 1 % / 3.0 Ah |
| Charge + discharge MOS | both on | both on | both on |

1. **One cluster per board.** 52.5 V against 51.2 V is not a busbar drop — two
   sites, two DC buses.
2. **Board #2's two packs really are paralleled** — six millivolts apart. Their
   normal voltage spread is single-digit millivolts, which is what makes §4's
   third case detectable at all.
3. **Packs are not identical.** 660 Ah beside 300 Ah on one bus. **A pack API
   that reports only percentages is unusable** — consumers need amp-hours.
4. **Current sharing is wildly unequal** — −0.384 A against −2.738 A, a factor
   of seven, from packs six millivolts apart. This kills "current ≈ 0 means
   gone" as a rule.
5. **The vendor's own SOH is visibly wrong-shaped** — 90 % on one pack, a stuck
   100 % on the other two, exactly as
   [design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md)
   §3.5 predicts. A pack must say *how much* to believe its own numbers.

## 3. A pack type is a blueprint; a pack is an instance

- A **pack type** is the vendor blueprint: it knows one protocol and one map,
  declares what it can report and which commands it accepts, and translates
  vendor facts into neutral ones. `pack_jkbms` consumes the Modbus subscription
  surface; `pack_pylontech` consumes inbound Pylontech CAN.
- A **pack instance** is one configured, operator-named battery bound to a type,
  with its own addressing and nameplate facts. Several instances of one type are
  ordinary — two JK packs on board #2 today.
- The **core** owns what every battery has in common: state, condition, the
  staleness clock, confidence, persistence, command dispatch. It never learns
  what a holding register or a CAN ID is.

```
JK PB    ──(Modbus RTU)──→ ┌ type: jkbms     ┐   instances: "sodas_a", "sodas_b"
Pylon /  ──(CAN in)──────→ └ type: pylontech ┘   instance:  "zaliakalnis"
Dyness                            │
                                  ▼
                              Pack core  ──→ state + condition + commands
                                                │
                                                ├→ (future) Cluster → CAN → inverter
                                                └→ MQTT / HTTP / CLI
```

**A pack does not know it is in a cluster.** Nothing in `pack.h` mentions
membership, aggregation or "the battery".

**The per-cell estimator lives inside the pack**, behind this API, and is
**optional per type**. A JK supplies cell voltages, balancer state and
balance-lead resistance, so it can run. A Pylontech-speaking pack supplies
pack-level numbers only, so it cannot. Both answer the same questions with
different confidence. Version 1 of `pack_jkbms` forwards the JK's own SOC/SOH at
low confidence; the estimator lands later without a consumer noticing.

## 4. Disconnection is three-fold

*"Each battery pack should be capable to detect its own disconnection"* resolves
into three cases with three different owners. Only the middle one is the pack's.

| Kind | Who causes it | Who can detect it | Example |
|---|---|---|---|
| **Commanded** | a consumer | nobody needs to — it was ordered | cold weather: disable some packs to stop them cycling below temperature, or force one in so it discharges and self-heats |
| **Pack-detected** | the pack's own protection | **the pack**, which reports the condition | a protection opens the charge MOS; one pack sustains a deeper discharge than another before dropping out |
| **Consumer-sensed** | the world — breaker, fuse, busbar | **only a consumer that sees several packs** | the pack reports all-OK and carries no current while its neighbours carry plenty |

> The pack's job across all three: **report condition faithfully, accept the
> commands it advertises, and expose enough raw evidence that a consumer can
> sense the third kind.** Voltage, current and contactor state are published for
> exactly that reason.

**At these sites the mechanism is a breaker** (confirmed 2026-08-25), which
lands the real-world case squarely in the third row and makes it the one that
matters most:

- A breaker is **manual and upstream of the BMS**. Nothing is commanded, nothing
  is protection-tripped, and the BMS keeps answering with its MOS closed and a
  plausible voltage. The pack has *no local evidence at all* — which is exactly
  why the pack must not be the thing that judges it.
- **A single-pack site is therefore blind to the only mechanism it has.** Board
  #1 runs one pack; there is nothing to compare against. Physics, not a defect —
  but it should be said out loud rather than discovered.
- **Do not design for breakers specifically.** A contactor would add a
  *commanded* path and make the same event appear in row 1; a fuse behaves like
  a breaker but never comes back; a loose busbar arrives gradually as rising
  resistance rather than as a step. The pack's obligation is unchanged across
  all of them — publish voltage, current and switch state faithfully — and that
  is precisely why it is stated as an obligation rather than as a detector.
- **The comparison is differential.** A pack that just left the bus still reads
  the right voltage; it drifts only once the bus moves. On LFP's flat curve that
  takes a while, and §2 point 4 shows raw current is a poor discriminator.
  Detection latency is real, and it is the consumer's to budget.
- **Commanded disconnection flows *into* the pack** — §6.

## 5. What varies by type

| | JK PB (Modbus) | Pylontech-speaking pack |
|---|---|---|
| Per-cell voltages | 16–32, 1 mV | no |
| Balancing detail | source/sink cell, current, duty | no |
| Balance-lead resistance | per cell, 1 mΩ | no |
| Contactor state per direction | yes (`0xC0`/`0xC1`) | `0x35C` charge/discharge enable flags |
| Capacity in Ah | yes, plus a learned value | **absent from the implemented frame set** — `sPylonBatteryData` has no capacity field and `0x35F` is unimplemented |
| Per-cell estimator possible? | **yes** | **no** |
| Commands accepted | 34 writable points | protocol-dependent, likely none |

So `sPackState` cannot be a struct where every field is always meaningful. It
needs a **capability set** — what this pack reports and what it accepts. A
consumer wanting cell detail asks; a consumer wanting only Ah and condition
never notices the difference.

**Condition evidence is type-specific too.** MOS state is a JK concept, `0x35C`
flags a Pylontech one. The type maps whatever it has onto the neutral condition;
where it has nothing, the pack degrades to comms-liveness and says so through
its confidence.

## 6. Control is capability-gated, and safety-relevant

**A JK is already controllable from here.** The FC 0x06 problem was solved by the
config carrying `writeFc: 16`; the shipped JK config has **34 writable points of
114** — `charge_enable`, `discharge_enable`, `balance_enable`,
`charge_current_max`, `discharge_current_max` and every protection threshold.
`Modbus_Request` and `POST /api/modbus/write` already reach them.

So control is a scope decision, not a constraint, and the use cases are real
(§4 row 1). Which makes it safety-relevant:

- **A pack that can write `charge_enable` can disconnect itself.** Commands need
  a declared capability and bounds, not a generic write-through.
- **A command outstanding when the pack goes stale has an unknown outcome**, and
  the answer cannot be "assume it obeyed".
- **Commands must be attributable** — who asked, when, what happened.

## 7. Where it runs — the shared functionality task

**Packs cannot each own a task.** `SYSMON_MAX_TASKS` is 16, the board runs about
twelve, and `uxTaskGetSystemState()` returns *zero* rather than truncating when
over-subscribed — blinding all of sysmon at once.

The answer is the pattern ZhagaFW uses in `App/Func/func.c`: **one task, one
statically allocated queue, and a packed event-ID space with one contiguous
range per client**, each range sized by that client's own `_last` enum. A client
is handed its base and a post function at init
(`Init<Client>(uint16_t evtIdOffset, void (*irq2Task)(uint16_t, void *))`) and
**never sees the queue or the task**. One place chooses between the task-context
and ISR-context post macros; dispatch is a range-check chain that subtracts the
base to recover the client's own enum.

**Why it fits:** one task for N packs, and — the important part — **a pull type
posts from the Modbus callback while a push type posts from its RX ISR, and the
core cannot tell them apart.** That is exactly the seam §3 needs.

Not a foreign import: `modbus_engine.c` already has this shape internally —
timer slots whose callback only sets `due` and pokes, one non-blocking queue,
one task draining it, missed events counted. It has simply never been factored
out.

**Two departures from Zhaga:** keep its **static queue allocation** (CCM heap is
the tight resource here), and **count drops rather than only logging them**, as
`modbus_engine.c` does with `s_droppedPokes` — a dropped pack event is a missed
state change and belongs in `sysmon`, not only in a log.

## 8. What the Modbus surface guarantees

Constrains `pack_jkbms` only. Two of these were assumed wrongly before.

**`mbEvt_txn` arrives *before* its samples.** `service_block`
(`modbus_engine.c:314-346`) dispatches the txn immediately after the wire
returns and *then* decodes. A txn is a **leading** marker.

**A transaction is a read block, not a device poll, and there is no
end-of-sequence event.** One timer firing derives 1..N read blocks and emits one
txn each. Nothing marks "this device's table is fully delivered".

That matters less than it sounds: the JK config's **live table is exactly 8
points in one block, so it is exactly one transaction** — current, voltage, SOC,
remaining and the balancer fields arrive atomically. Cells span three tables and
are anyway skewed ~4–5 s by the JK's own scan.

> **Strict whole-device coherence is neither achievable nor needed.** A type
> needs a per-field timestamp and a stated skew budget. The one group that must
> be coherent already is.

**The module refuses to judge liveness, deliberately.** `mqtt_bridge.c:353-383`
is the reference: count consecutive failed txns, treat a Modbus **exception as
an answer**. **But silence is not failure** — `run_sequence` returns without
emitting anything when the port has no driver, a lookup fails, the capability
will not open, or `spanCount == 0`, and §11a.8 records that a config swap can
leave a subscribed device with no armed timers while `polled: true` still reads
true. **Hence: a wall-clock timeout, not a failure counter.**

Also: callbacks run in the modbus task, synchronously, must not block, pointers
borrowed until return; identity is `{devOrd, ptOrd}` and must be re-keyed on
every `mbEvt_config`; the request path emits no events; `MB_MAX_DEVICES = 8`
bounds *Modbus-sourced* packs only.

---

# Part II — API

Naming per [`C coding standard.md`](../C%20coding%20standard.md): `s`/`e`/`u`/`f`
prefixes, enum values `<modulePrefix><Category>_<value>`, mandatory unit
suffixes, header/source section dividers. Module prefix `pack`.

One principle runs through all of it and resolves most of the hard cases at
once:

> **Nothing binds by position, nothing publishes without a lock chosen for its
> context, and nothing is advertised that has no accessor behind it.**

---

## 9. File layout and boundary rules

```
App/Func/
  func.h              Func_Init / Func_Start / fFuncPost / sFuncStats
  func.c              packed event-ID space, one task, one static queue,
                      per-client drop counters, dispatch, client wiring.
                      A composition root, like cmd_parser.c.
App/Pack/
  pack.h              THE consumer header.  Nothing else is included by
                      consumers.  No float, no RTOS type, no protocol word.
  pack.c              surface: instance table, subscriptions, command claim,
                      publish/commit, dispatch, persistence, statistics.
  pack_fsm.c/.h       condition · staleness · confidence · command
                      validation.  libc only, NO RTOS, NO HAL — compiled by
                      tests/ as well as by the firmware.
  pack_cfg.c/.h       the uploaded pack configuration: streaming parse,
                      re-serialise, nvDb record.
  pack_type.h         the blueprint contract — included by TYPES, never by
                      consumers.
  pack_jkbms.c        type: JK over Modbus RTU (pull)
  pack_pylontech.c    type: Pylontech over CAN (push)
App/Can/
  can_rx.c/.h         NEW: the single CAN RX dispatcher (§13, §16 item 5)
```

**`pack_fsm.c` is the whole answer to "how is this tested".** Everything that
can be a pure function of `(previous condition, group ticks, now)` lives there,
and nothing in it may call FreeRTOS, HAL or Trice. `App/Net/wg_conf.c` is
already host-tested libc-only App code, so the precedent exists.

### Boundary rules, enforced in CMake in the spirit of `CMakeLists.txt:368-392`

| Rule | Greppable check |
|---|---|
| `pack.c`, `pack_fsm.c`, `pack_cfg.c` may not include a protocol or consumer header | `#include` matching `App/(Mqtt\|Http\|Can\|Data)/`, `lwip/`, `modbus`, `pylontech` |
| Nothing outside `App/Pack/` may include `pack_type.h`, `pack_fsm.h`, `pack_cfg.h` | consumer scan, exactly like the Modbus one |
| A **type** may include `pack_type.h`, `pack.h` and its own protocol's headers, nothing else from the module | |
| **A type may not take a lock** | `taskENTER_CRITICAL`, `taskDISABLE_INTERRUPTS`, `__disable_irq` banned in `pack_*type*.c`, `pack_jkbms.c`, `pack_pylontech.c` |
| **No floating point crosses the API** | `float`/`double` banned in `pack.h`, `pack_type.h` |
| Nothing outside `App/Func/func.c` may name a packed event id | `func_` identifiers banned elsewhere |
| Flash only through `NvDb_*` | already enforced globally |

The lock ban is mechanical, not advisory. **A lock is a context decision, and a
type does not know its context statically** — the same `pack_pylontech.c` code
is reachable from an ISR and from a task. So a type is simply not allowed to
have one, and the class of error becomes unrepeatable rather than merely fixed.

---

## 10. `pack.h` — the consumer surface

```c
/**
 * @file    pack.h
 * @brief   PeriphNet battery pack module — THE public API.
 *
 * A pack is one configured, operator-named battery bound to a TYPE.  A type
 * knows one vendor protocol and translates its facts into neutral ones; the
 * core owns what every battery has in common — state, condition, the
 * staleness clock, confidence, persistence and command dispatch — and never
 * learns what a holding register or a CAN id is.
 *
 * A PACK DOES NOT KNOW IT IS IN A CLUSTER.  Membership, aggregation and "the
 * battery" appear nowhere here and never will: a pack reports its own state,
 * and the consumer decides what to consume (Part I §3).
 *
 * WHAT THIS MODULE WILL NOT DO, so nobody proposes it again: a generic
 * write-through to the vendor's registers; a retry or an "assume it obeyed"
 * on a command whose outcome is unknown; a judgement about whether a pack has
 * been disconnected by something outside itself (Part I §4); a borrowed
 * pointer into live state; a float.
 */

/* Bounds --------------------------------------------------------------- */

#define PACK_MAX            8u    /* instances.  NOT MB_MAX_DEVICES: a
                                     CAN-sourced pack spends no Modbus device
                                     slot, so the ceilings are unrelated     */
#define PACK_NAME_LEN      16u    /* including NUL                           */
#define PACK_BIND_LEN      24u    /* stored width of the `bind` token (§12).
                                     The token reaches a type as a borrowed
                                     const char *; this is the configuration
                                     record's field width, and the parser
                                     rejects anything longer                 */
#define PACK_CELLS_MAX     16u    /* raising to the JK's 32 costs 1.3 KB more
                                     .bss and no API change                  */
#define PACK_MAX_SUBS       8u    /* subscription table; §15 budgets it     */

/** age_ms of a group that has NEVER been delivered.  Distinct from 0, which
 *  means "delivered, this instant".  This is the one mechanism that separates
 *  "not read yet" from "read as zero"; there is no second validity mask. */
#define PACK_AGE_NEVER     0xFFFFFFFFu
```

### 10.1 Type and chemistry — persisted, append-only

```c
typedef enum {
    packType_jkBms     = 0,
    packType_pylontech = 1,
    packType_last                   /* a count, never stored                 */
} ePackTypeId;
```

```c
typedef enum {
    packChem_lfp       = 0,
    packChem_liIon     = 1,
    packChem_lto       = 2,
    packChem_last
} ePackChemistry;
```

Both are **stored in the pack configuration**, so both are **never renumbered,
append only** — the same rule as `eNvDbUser` and `eModbusDecodeType`.

`ePackChemistry` exists now, ahead of its consumer: it selects the OCV table the
per-cell estimator will need
([design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md)
§5.2), and a JK reports its own chemistry at realtime `0x10D`, so a type can
cross-check the configured value and raise a mismatch rather than silently
estimating against the wrong curve.

### 10.2 Capabilities — what THIS INSTANCE reports

```c
typedef enum {
    packCap_capacityAh      = 1u << 0,  /* remaining_mAh, capacity_mAh, soc  */
    packCap_learnedCapacity = 1u << 1,  /* capacity_mAh is MEASURED          */
    packCap_soh             = 1u << 2,
    packCap_temperatures    = 1u << 3,
    packCap_currentLimits   = 1u << 4,
    packCap_switchState     = 1u << 5,
    packCap_cellSummary     = 1u << 6,  /* cellCount, cellMax/Min_mV + idx   */
    packCap_cellDetail      = 1u << 7,  /* Pack_GetCells() → per-cell mV     */
    packCap_leadResistance  = 1u << 8,  /* Pack_GetCells() → leadRes_mOhm    */
    packCap_balancer        = 1u << 9,  /* Pack_GetCells() → balance fields  */
    packCap_cellEstimator   = 1u << 10, /* soc/soh/capacity are ESTIMATED
                                           here.  Reserved; no type sets it
                                           until the estimator lands         */
} ePackCap;
```

**A capability bit is set only when an accessor exists for it AND the live
configuration actually backs it.** A type declares what it *could* do; what an
instance advertises is what its `bind()` confirmed.

That distinction is load-bearing, and the JK config is the worked example. It
carries **34 writable points of 114**, but §11a.5 shows the R/RW split is a
decision an operator makes and revises: the realtime and device-info blocks are
read-only, only settings-block parameters carry `access: "rw"`, and `DevAddr`
and `CellCount` are deliberately read-only *despite being RW in the PDF*,
because writing them loses the device or corrupts the pack model. A statically
declared per-type command set would advertise commands a given config cannot
issue. **So capabilities are per instance, confirmed at bind.**

**The capability set governs field validity.** A field group listed against a
capability is meaningful only when that capability is set; otherwise it reads
zero and means nothing. *Age is a separate question*, answered per group by
`age_ms[]`.

### 10.3 Commands — a dense id, and a bitmask built from it

```c
typedef enum {
    packCmd_undefined       = 0,    /* an all-zero struct is not a command   */
    packCmd_chargeEnable    = 1,    /* value 0/1                             */
    packCmd_dischargeEnable = 2,    /* value 0/1                             */
    packCmd_balanceEnable   = 3,    /* value 0/1                             */
    packCmd_chargeLimit     = 4,    /* value mA                              */
    packCmd_dischargeLimit  = 5,    /* value mA                              */
    packCmd_last
} ePackCmdId;

#define PACK_CMD_BIT(id)   (1u << (uint32_t)(id))

/* A mask with two or more bits set must never collide with a valid id, or
 * passing a mask where an id belongs would command the wrong thing and pass
 * every check.  The smallest two-bit mask is PACK_CMD_BIT(1)|PACK_CMD_BIT(2);
 * this assert fails the build if adding commands ever lets an id reach it. */
_Static_assert(((1u << 1) | (1u << 2)) >= (uint32_t)packCmd_last,
               "a two-bit command mask aliases a valid ePackCmdId");
```

Two different things, deliberately not conflated: **a command is one id**, **a
capability set is a mask**, and `PACK_CMD_BIT` is the only bridge.

**This needs a guard rather than an assertion of safety, and an earlier draft
got it wrong.** With ids dense from 0, `PACK_CMD_BIT(chargeEnable) |
PACK_CMD_BIT(dischargeEnable)` is `3` — which *was* a valid id, so a mask passed
into the id parameter would have commanded a third, unrelated command and
satisfied every check. C cannot type its way out of this. So: ids start at **1**,
`packCmd_undefined = 0` means an all-zero struct is not a command, and the
`_Static_assert` above keeps the property true as commands are added instead of
leaving it to memory.

**That assert currently sits exactly on its boundary and this is deliberate.**
The smallest two-bit mask is 6 and `packCmd_last` is 6, so it passes — and a
**sixth** command makes `packCmd_last` 7 and breaks the build. That is the
tripwire working, not a defect, and the remedy is one line: widen the shift to
`#define PACK_CMD_BIT(id) (1u << ((uint32_t)(id) + 3u))`, which moves the
smallest two-bit mask to 48 and buys room to 47 commands. It costs nothing but
the low bits of a mask nobody reads positionally. Do that when the assert fires;
do not delete the assert.

### 10.4 Field groups — the unit a timestamp is kept for

```c
typedef enum {
    packGrp_electrical = 0, /* voltage_mV, current_mA — the liveness group   */
    packGrp_charge,         /* soc_pm, remaining_mAh, capacity_mAh, soh_pm   */
    packGrp_temperature,
    packGrp_limits,
    packGrp_switches,
    packGrp_alarms,
    packGrp_cells,          /* the summary here AND Pack_GetCells()          */
    packGrp_vendorInfo,
    packGrp_last
} ePackGroup;

#define PACK_GRP_BIT(g)    (1u << (uint32_t)(g))
```

**One age for a whole pack is a lie.** A JK delivers the live electrical group
in a single transaction every 5 s and its cell voltages ~4–5 s behind, skewed by
the BMS's own scan loop (Part I §8); a Pylontech pack delivers some frames of
its set and not others. So the clock is per group, and the pack **states its
skew instead of hiding it**.

### 10.5 Condition, switches, alarms, provenance

```c
typedef enum {
    packCond_absent = 0,    /* configured, has never answered                */
    packCond_stale,         /* answered before, not within its timeout       */
    packCond_online,
    packCond_last
} ePackCondition;

/** WHY a pack is not online.  Without this, "the battery is unreachable",
 *  "no device matches that address", "this firmware has no such type" and
 *  "bound, but no live plan covers it" all render identically as `absent`,
 *  and they are three different call-outs plus a configuration error. */
typedef enum {
    packWhy_none = 0,       /* it is online                                  */
    packWhy_noType,         /* config names a type nobody registered         */
    packWhy_typeUnavailable,/* the type is registered but structurally cannot
                               bind on this build — e.g. pack_pylontech
                               before the CAN RX dispatcher exists (§16.5).
                               Distinct from noType: the operator's config is
                               correct and the firmware is the limitation     */
    packWhy_noBinding,      /* bindKey resolved to zero or several devices   */
    packWhy_notPolled,      /* bound, but no live plan covers it             */
    packWhy_noReply,        /* polled, silent                                */
    packWhy_last
} ePackAbsentReason;

typedef enum {
    packSwitch_unknown = 0, /* this type cannot report it — a first-class
                               value, not an error                           */
    packSwitch_open,
    packSwitch_closed,
    packSwitch_last
} ePackSwitch;

typedef enum {
    packAlarm_cellOverVoltage    = 1u << 0,
    packAlarm_cellUnderVoltage   = 1u << 1,
    packAlarm_packOverVoltage    = 1u << 2,
    packAlarm_packUnderVoltage   = 1u << 3,
    packAlarm_overTemperature    = 1u << 4,
    packAlarm_underTemperature   = 1u << 5,
    packAlarm_chargeOverCurrent  = 1u << 6,
    packAlarm_dischargeOverCur   = 1u << 7,
    packAlarm_cellImbalance      = 1u << 8,
    packAlarm_internalFault      = 1u << 9,
    packAlarm_protectionOpen     = 1u << 10, /* the pack's own protection has
                                                opened a contactor — Part I
                                                §4's middle case, stated      */
} ePackAlarm;

typedef enum {
    packFlag_socEstimated   = 1u << 0,
    packFlag_sohEstimated   = 1u << 1,
    packFlag_capacityLearnt = 1u << 2,
    packFlag_bindVerified   = 1u << 3,  /* the pack read its OWN address back
                                           and it matched (§11a.5)           */
} ePackFlag;
```

A type maps its native bits onto the neutral alarm set **and** passes the raw
words through in `vendorAlarms`, so nothing is lost to the translation. The JK's
22 bits do not fit exactly; that is the point — a consumer reasons about the
neutral set, an operator diagnoses with the raw one.

### 10.6 The state

```c
typedef struct {
    /* --- always valid ------------------------------------------------- */
    uint32_t        caps;               /* ePackCap, this instance          */
    uint32_t        cmds;               /* PACK_CMD_BIT set accepted.
                                           0 = read-only pack               */
    uint32_t        flags;              /* ePackFlag                        */
    uint32_t        alarms;             /* ePackAlarm                       */
    uint32_t        vendorAlarms[2];    /* the type's own raw words         */
    uint32_t        nameplate_mAh;      /* FROM CONFIGURATION, so valid even
                                           for a pack that never answered   */
    uint32_t        groupsStale;        /* PACK_GRP_BIT set = past ITS budget.
                                           Computed by the core, because the
                                           budget is the core's and a
                                           consumer must not need to know it */
    uint32_t        age_ms[packGrp_last];   /* PACK_AGE_NEVER = never seen   */

    /* --- packGrp_electrical ------------------------------------------- */
    uint32_t        voltage_mV;
    int32_t         current_mA;         /* + charge, − discharge            */

    /* --- packCap_capacityAh ------------------------------------------- */
    uint32_t        remaining_mAh;
    uint32_t        capacity_mAh;       /* usable; nameplate unless
                                           packFlag_capacityLearnt          */
    /* --- packCap_currentLimits ---------------------------------------- */
    uint32_t        chargeLimit_mA;
    uint32_t        dischargeLimit_mA;

    /* --- identity: always valid --------------------------------------- */
    char            name[PACK_NAME_LEN];

    /* --- SOC/SOH and how much to believe each ------------------------- */
    uint16_t        soc_pm, soh_pm;
    uint16_t        socConf_pm, sohConf_pm;

    /* --- packCap_cellSummary / temperatures --------------------------- */
    uint16_t        cellMax_mV, cellMin_mV;
    int16_t         tempMax_dC, tempMin_dC;     /* 0.1 °C                   */

    /* --- small, always valid ------------------------------------------ */
    uint8_t         idx;
    uint8_t         typeId;             /* ePackTypeId                      */
    uint8_t         cond;               /* ePackCondition                   */
    uint8_t         cellCount;
    uint8_t         cellMaxIdx, cellMinIdx;
    uint8_t         chargeSwitch, dischargeSwitch;  /* ePackSwitch          */
    uint8_t         why;                /* ePackAbsentReason                */
} sPackState;                           /* 132 bytes                        */
```

**Two confidences, not one.** SOC drifts between anchors and SOH does not —
[design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md)
§5.9.4 states exactly why one number would already be wrong before the estimator
exists. Both are **capped by the core at what the electrical group's age
permits**, so a pack going quiet loses confidence *before* it goes stale, and a
type can never claim more than its freshness earns.

### 10.7 Per-cell detail

A separate call: ten times the size of the state, almost nobody wants it, and it
has **its own clock** — on a JK it is 4–5 s behind the electrical group and
refreshed on a different plan.

```c
typedef struct {
    uint32_t        age_ms;             /* PACK_AGE_NEVER if never           */
    int32_t         balanceCurrent_mA;  /* packCap_balancer                  */
    uint16_t        cell_mV[PACK_CELLS_MAX];        /* packCap_cellDetail    */
    uint16_t        leadRes_mOhm[PACK_CELLS_MAX];   /* packCap_leadResistance*/
    uint16_t        balanceDuty_pm;     /* packCap_balancer                  */
    uint8_t         cellCount;          /* entries actually filled           */
    uint8_t         balanceSrcIdx;      /* 0xFF = none                       */
    uint8_t         balanceSinkIdx;     /* 0xFF = none                       */
    uint8_t         balanceActive;
} sPackCells;                           /* 80 bytes at 16 cells              */
```

This is where the per-cell estimator will publish, and the shape is already what
[design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md)
§5.4 produces. Fields it will add — per-cell capacity, per-cell R, per-cell
confidence — go on the **end** behind their own capability bits and change no
consumer.

### 10.8 Results

```c
typedef enum {
    packErr_ok             =   0,
    packErr_badArg         =  -1,
    packErr_notFound       =  -2,
    packErr_notSupported   =  -3,   /* command not advertised by THIS pack   */
    packErr_outOfRange     =  -4,   /* outside the instance's bounds         */
    packErr_notOnline      =  -5,
    packErr_busy           =  -6,   /* a command is already in flight        */
    packErr_timeout        =  -7,   /* the type never completed in time      */
    packErr_unknownOutcome =  -8,   /* went stale mid-command                */
    packErr_refused        =  -9,   /* the transport refused it outright     */
    packErr_full           = -10,   /* subscription table                    */
    packErr_unprovisioned  = -11,
    packErr_transport      = -12,   /* the wire failed; nothing was decided  */
} ePackErr;
```

Negative, like `eModbusErr`; no `_undefined = 0`, for the same reason the rest of
the project omits one. **Append-only** — it crosses into consumers and onto the
HTTP surface.

The last four are **produced by a type through `PackType_CommandDone`**, not by
the core: `packErr_refused` when the pack answered and rejected the write
(a Modbus exception), `packErr_transport` when the wire failed so nothing was
decided, `packErr_timeout` when the type itself gave up. `packErr_unknownOutcome`
is the core's, and only the core's — it is the answer when the pack went stale
before any of the others arrived.

**Command results partition into decided and undecided, and a consumer must
treat the partition, not the individual codes:**

| | Codes | What a consumer may conclude |
|---|---|---|
| **Decided** | `packErr_ok`, `packErr_refused` | the write landed, or the pack answered and rejected it |
| **Decided, nothing was sent** | every synchronous refusal (`notSupported`, `outOfRange`, `notOnline`, `busy`, `badArg`) | no wire traffic occurred |
| **UNDECIDED** | `packErr_timeout`, `packErr_transport`, `packErr_unknownOutcome` | **the write may or may not have landed** |

`packErr_timeout` is exactly as undecided as `packErr_unknownOutcome` — a Modbus
write that times out may well have been executed by the slave before the reply
was lost. Reading `timeout` as "it did not happen" is precisely the inference
§16 item 2 forbids, and the earlier wording named only `unknownOutcome`.

**Which of the two undecided codes a consumer sees is largely an artefact of
`timeout_ms` versus `staleAfter_ms`.** With `timeout_ms = 5000` against a JK's
`staleAfter_ms = 15000`, a pack that dies mid-command always completes
`packErr_timeout`, ten seconds before staleness is even evaluated. That is not a
defect, but it does mean **`packErr_unknownOutcome` must not be treated as the
only signal of an unknown state** — the partition is what carries the meaning.

### 10.9 Lifecycle, reading, subscribing, commanding

```c
#define PACK_EVT_COUNT     8u   /* event ids occupied in the shared task's
                                   packed space.  For func.c ONLY            */

int  Pack_Init(uint16_t evtIdBase, fFuncPost post);

int  Pack_Count(void);                              /* 0 = unprovisioned     */
int  Pack_FindByName(const char *name);             /* → idx, packErr_notFound */
int  Pack_GetState(uint8_t idx, sPackState *out);   /* COPIES               */
int  Pack_GetCells(uint8_t idx, sPackCells *out);   /* packErr_notSupported
                                                       if no cell capability */
```

`Pack_Init` is called by `App/Func/func.c` and by nothing else — the module never
sees the queue or the task, only its event-id base and a post function.
Requires `NvDb_Init()`. **Zero packs is a valid, first-class state**, reported as
unprovisioned rather than as an error: a pack configuration describes hardware
the board may not have, so a fallback would be a guess about the deployment.

`Pack_GetState` **copies**, so there are no borrowed pointers and no lifetime
rules. Callable from any task, **not from an ISR** (it takes a task-context
critical section). `age_ms[]` and `groupsStale` are computed inside the same
critical section, so an age can never disagree with the value it describes.

**Name is the identity.** An index is a position and may change at a config
reload; a name may not. A consumer caching an index re-resolves on
`packEvt_config`.

```c
typedef enum {
    packEvt_state     = 1u << 0,  /* one or more field groups refreshed      */
    packEvt_condition = 1u << 1,
    packEvt_alarm     = 1u << 2,
    packEvt_command   = 1u << 3,
    packEvt_config    = 1u << 4,  /* every cached index is now suspect       */
    packEvt_released  = 1u << 5,
    packEvt_all       = 0x3Fu,
} ePackEventType;

typedef struct {
    ePackEventType type;
    uint32_t       tick_ms;
    const char    *name;        /* BORROWED for the call — saves every
                                   subscriber a lookup, creates no lifetime
                                   rule, dies with the call                  */
    uint8_t        idx;
    union {
        struct { uint32_t groups; }         state;      /* PACK_GRP_BIT     */
        struct { uint8_t  from, to; }       condition;
        struct { uint32_t added, cleared; } alarm;
        struct { uint8_t  cmd; int32_t value; int16_t result; } command;
    } u;
} sPackEvent;

typedef void (*fPackSubscriber)(const sPackEvent *ev, void *ctx);

int Pack_Subscribe(uint32_t evtMask, fPackSubscriber cb, void *ctx);
int Pack_Unsubscribe(int handle);
```

**The event carries no state payload** — only what changed and for which
instance. A subscriber wanting values calls `Pack_GetState`. That removes every
borrowed-pointer lifetime rule the Modbus surface has to spell out.

**Ordering:** the core commits the live slot *before* it dispatches, so a
subscriber calling `Pack_GetState` from inside its callback is guaranteed to see
at least the update the event announced.

`Pack_Subscribe` does **not** post, deliberately: a posted subscribe cannot
return "table full", which is a real init-time error. `Pack_Unsubscribe` posts
and is legal from inside a callback; **the final `packEvt_released` call is the
release point** — delivered regardless of `evtMask`, and after it returns the
module never calls again and `ctx` may be freed.

Callbacks run on the shared functionality task, synchronously, and **must not
block** — same reasoning and same remedy as `docs/modbus.md` §4.7.

```c
typedef struct {
    int32_t     value;      /* scaled per command; 0/1 for booleans          */
    ePackCmdId  cmd;
    const char *origin;     /* "cluster", "cli", "http".  Borrowed for the
                               call; the core copies what it logs            */
} sPackCommand;

typedef void (*fPackCmdDone)(uint8_t idx, const sPackCommand *cmd,
                             int16_t result, void *ctx);

#define PACK_CMD_TIMEOUT_MAX_MS  60000u

int Pack_Command(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms,
                 fPackCmdDone done, void *ctx);
int Pack_CommandBounds(uint8_t idx, ePackCmdId cmd, int32_t *min, int32_t *max);
```

**Asynchronous.** `packErr_ok` means *accepted*, not done: a Modbus-backed
command reaches the wire later and its outcome arrives through `done`. Anything
else is a synchronous refusal and `done` does **not** fire.

The rules, all safety-motivated (Part I §6):

1. **Refused unless advertised** — `!(cmds & PACK_CMD_BIT(cmd))` →
   `packErr_notSupported`, before anything reaches a wire.
2. **Refused unless in range** — the instance declares bounds per command and
   the core checks them. **There is no clamping.**
3. **Refused when not `packCond_online`** — commanding an absent or stale pack
   is a programming error, not a retry.
4. **One command in flight per pack** — a second returns `packErr_busy`.
   Queueing would make "what state is this pack in" unanswerable.
5. **A command outstanding when the pack goes stale completes
   `packErr_unknownOutcome`** — never "assume it obeyed". The consumer decides
   what to do about a pack in an unknown state; the module will not decide.
6. **`done` always fires** for an accepted command, within `timeout_ms` + one
   tick, whatever the type does or fails to do.
7. **Every command is logged** with origin, value and result, and counted per
   pack in `Pack_Stats()`.
**These are evaluated in the order listed, most specific first**, and the order
is part of the contract rather than an implementation detail: two
implementations that disagree on whether an unadvertised command to a stale pack
is `notSupported` or `notOnline` would give a consumer's retry logic two
different answers to the same question. Advertisement and bounds are properties
of the *configuration* and are checked before anything about the pack's current
state, because they are wrong regardless of what the pack is doing.

8. **A configuration change completes it `packErr_unknownOutcome`** — either
   this module's (§10.10) or the Modbus module's. A `jkbms` instance re-resolves
   its `devOrd` on every `mbEvt_config`, so a command issued against the old
   resolution must not be allowed to land silently against a new one. Note the
   Modbus module's own rule is that a config swap *completes* outstanding
   requests rather than dropping them (`modbus.h:445`) — it completes them
   against the **old** generation, which is exactly why this module cannot wait
   for that answer and must decide for itself.

`timeout_ms` is 1..`PACK_CMD_TIMEOUT_MAX_MS`; **0 is rejected** — an unbounded
deadline is the same as no guarantee.

`Pack_CommandBounds` exists so an HA number entity, a CLI or the future cluster
can render the limits without guessing.

**Consequence worth stating, because otherwise every consumer invents its own:**
rules 3 and 4 mean the module **will not hold intent across a stale period**.
Part I §4's headline use case — disabling packs in the cold — involves exactly
the packs most likely to be asleep or protection-tripped. Re-asserting a command
when a pack returns is **the consumer's policy**, deliberately not the module's.

### 10.10 Configuration

The HTTP and CLI surfaces in §12 need functions to call. Same streaming
byte-source contract as the Modbus compiler, so the HTTP body cursor already in
`http_server.c` drives it unchanged.

```c
typedef int (*fPackByteSource)(void *ctx, uint8_t *buf, uint32_t maxLen);
typedef int (*fPackByteSink)  (void *ctx, const char *data, uint32_t len);

typedef struct {
    int      ok;
    int      packIdx;           /* first failure location; -1 = n/a          */
    char     field[24];         /* offending key, or "json" for syntax       */
    char     reason[64];
    uint8_t  packs;             /* how many parsed before the failure        */
} sPackCfgResult;

int Pack_ConfigVerify(fPackByteSource src, void *srcCtx, sPackCfgResult *res);
int Pack_ConfigApply (fPackByteSource src, void *srcCtx, sPackCfgResult *res);
int Pack_ConfigExport(fPackByteSink   sink, void *ctx);
int Pack_ConfigErase (void);
```

**One parser, one pass, one result struct** — `Verify` and `Apply` differ only in
whether they commit, so there is never a second validator that can disagree with
the first. `docs/modbus.md` §5.3 is the precedent and the reason: an upload
consumes the inactive region on success *and* on failure, so a verify that took
a different path would be testing something other than what runs.

`Pack_ConfigApply` rebuilds every instance, unbinds and rebinds every type, and
raises `packEvt_config`. **Outstanding commands complete with
`packErr_unknownOutcome`** — the binding they were issued against no longer
exists, so they are neither dropped nor assumed to have landed. This is refusal
path eight in §10.9's list, and it is symmetric with what happens on a *Modbus*
config swap, which invalidates a `jkbms` instance's `devOrd` resolution the same
way.

`Pack_ConfigErase` is Erase, not Reset: with no built-in default there is nothing
to reset **to**.

Called on the caller's task (HTTP), not posted — an upload takes seconds and
posting it would stall the shared task.

### 10.11 Diagnostics

```c
typedef struct {
    uint32_t updates;       /* state commits                                 */
    uint32_t cmdAccepted, cmdOk, cmdFailed;
    uint32_t cmdUnknown;    /* went stale mid-command — the one to watch     */
    uint32_t staleEvents;   /* online → stale transitions                    */
    uint32_t lateCompletes; /* a type answered AFTER the core gave up        */
    uint8_t  provisioned;
} sPackStats;

int  Pack_Stats(sPackStats *out);
void Pack_LogStatus(void);      /* `pack status` on the CLI                  */
```

---

## 11. `pack_type.h` — the back-end contract

Included by types, never by consumers. A type knows one protocol and one map; it
never sees the queue, the task, the subscription table, a lock, or another type.

> **The seam this exists to create:** a pull type publishes from the Modbus
> callback (modbus task) and a push type publishes from its CAN RX ISR, and
> **the core cannot tell them apart**. That is what makes one core serve both.

```c
typedef struct {
    int32_t     min_scaled;         /* inclusive; booleans use {0, 1}        */
    int32_t     max_scaled;
    ePackCmdId  cmd;
} sPackCmdBound;

typedef struct {
    const char *name;               /* borrowed for the call only            */
    const char *bindKey;            /* borrowed for the call only, §12       */
    uint32_t    nameplate_mAh;
    uint32_t    staleAfter_ms;      /* the electrical group's budget         */
    uint32_t    cellStaleAfter_ms;  /* the cell group's, which differs       */
    uint32_t    cmdAllow;           /* PACK_CMD_BIT set the OPERATOR permits.
                                       The core applies
                                         res->cmds &= info->cmdAllow
                                       after every bind, so "the type may
                                       narrow, never widen" is ENFORCED and
                                       not merely stated — the same move the
                                       lock ban makes                        */
    uint8_t     idx;
    uint8_t     cellCount;
    uint8_t     chemistry;          /* ePackChemistry, for the estimator     */
} sPackBindInfo;

typedef struct {
    uint32_t             caps;      /* confirmed against the live transport  */
    uint32_t             cmds;
    const sPackCmdBound *bounds;    /* one per bit in cmds; storage is the
                                       TYPE's and must outlive the bind      */
    uint8_t              boundCount;
} sPackBindResult;

typedef struct {
    const char *name;               /* config key: "jkbms", "pylontech"      */
    ePackTypeId id;
    uint32_t    capsMax;            /* the most any instance could offer     */
    uint32_t    cmdsMax;
    uint32_t    defaultStaleAfter_ms;
    uint32_t    defaultCellStaleAfter_ms;
    const sPackCmdBound *ceiling;   /* the widest bounds ANY instance of this
                                       type could accept.  The config parser
                                       rejects a `commands` bound wider than
                                       this (§12); it is the only numeric
                                       ceiling in the system                  */
    uint8_t     ceilingCount;
    uint8_t     maxInstances;       /* 0 = up to PACK_MAX.  A transport that
                                       cannot distinguish two of its packs
                                       says so HERE rather than letting a
                                       config declare three of them and
                                       having them overwrite each other      */

    int  (*bind)  (const sPackBindInfo *info, sPackBindResult *res);
    int  (*unbind)(uint8_t idx);
    int  (*submit)(uint8_t idx, const sPackCommand *cmd, uint32_t timeout_ms);
    void (*tick)  (uint32_t now_ms);    /* optional; NULL is legal            */
} sPackType;

int PackType_Register(const sPackType *type);
```

**Note what is not in `sPackBindInfo`: no `devOrd`, no array position, no
ordinal of any kind.** `bindKey` is an opaque, type-parsed token (§12).

`bind` and `unbind` and `submit` are **task context only, on the shared task**,
and `bind` may block briefly because it walks a catalogue. A bind that fails
leaves the instance `packCond_absent` with `caps = 0` — a reportable state, not
a boot failure.

`submit` **is asynchronous and its return value is acceptance only.**
`packErr_ok` means the type has taken it; the outcome must later arrive through
`PackType_CommandDone()`, exactly once, within `timeout_ms`. Anything else is a
refusal and no completion follows. The core already validated capability and
bounds; **a type must not re-check them and must not clamp.**

`tick` runs at ~4 Hz on the shared task. It is where a **push** type closes a
partial frame set that stopped arriving, and where a **pull** type notices its
transport went quiet.

### 11.1 Delivering an update

```c
typedef struct {
    uint32_t voltage_mV;
    int32_t  current_mA;
    uint32_t remaining_mAh, capacity_mAh;
    uint32_t chargeLimit_mA, dischargeLimit_mA;
    uint32_t alarms;                /* ePackAlarm                            */
    uint32_t vendorAlarms[2];
    uint32_t flags;                 /* ePackFlag the TYPE owns               */
    uint16_t soc_pm, soh_pm;
    uint16_t socConf_pm, sohConf_pm;/* the type's honest opinion; the core
                                       only ever lowers it                   */
    uint16_t cellMax_mV, cellMin_mV;
    int16_t  tempMax_dC, tempMin_dC;
    uint8_t  cellMaxIdx, cellMinIdx;
    uint8_t  chargeSwitch, dischargeSwitch;
} sPackRaw;

sPackRaw   *PackType_Staging    (uint8_t idx);  /* NULL if unbound           */
sPackCells *PackType_CellStaging(uint8_t idx);  /* NULL if no cell capability*/

void PackType_Publish     (uint8_t idx, uint32_t groups);
void PackType_PublishCells(uint8_t idx);
void PackType_CommandDone (uint8_t idx, ePackErr result);
void PackType_NoteLiveness(uint8_t idx, int answered);
```

`sPackRaw` is `sPackState` minus everything the core owns — name, idx,
condition, ages, staleness, confidence caps, capability confirmation. One
staging slot per instance, **single-writer by construction** (its owning
instance), and the core touches it only inside the same lock.

**`PackType_Publish` is legal from any context, including an ISR.** It is the
one place that chooses between the task- and ISR-context critical section and
between the task- and ISR-context queue post — exactly as `func.c` chooses
between its two post macros. It never blocks and copies **`sizeof(sPackRaw)` =
60 bytes** with interrupts masked (~0.3 µs). Note it is `sPackRaw` that is
copied, not `sPackState`: the core owns the difference and fills it after.

`groups` is a `PACK_GRP_BIT` set of **what this commit actually refreshed**. A
type that got only half a frame set says so, and the groups it did not name keep
their previous age — which is what makes a partial delivery visible instead of
silent. `PublishCells` is separate because the cell group has its own clock.

**`PackType_CommandDone` is the type→core completion path.** Legal from any
context — a Modbus type calls it from `fModbusReqDone` on the modbus task. A
completion for a command the core has **already finished** (timeout expired,
pack went stale, configuration replaced) is **discarded and counted** as
`sPackStats.lateCompletes`. A late answer must never be written into a decision
the consumer has already been told about.

`PackType_NoteLiveness` reports a transport fact that is not a measurement — the
pack answered with an exception, the port has no driver, the frame set arrived
incomplete. It feeds condition and confidence **without faking a value**.

### 11.2 How each type works

**`pack_jkbms` (pull).** At `bind`:

1. `Modbus_PlanList()` → subscribe with the mask of plans it recognises, or
   `MB_PLAN_ALL` while `gen_jk_config.py` still names plans positionally
   (§18 follow-up). Both `PlanList` and `Subscribe` are synchronous.
2. Resolve `bindKey` → `devOrd` via `Modbus_DeviceList()` (§12), also
   synchronous.
3. **Build the `name → ptOrd` map with a synchronous walk**, not from the
   catalogue burst: iterate `ptOrd` from 0 calling
   `Modbus_PointInfo(devOrd, ptOrd, &meta)` until it returns
   `mbErr_idNotFound`. `sModbusPointMeta` carries exactly what confirmation
   needs — `name`, `flags` (`MB_PT_READ|WRITE|BOUNDED`), `writeMin`/`writeMax`,
   `unit`, `scalePow10`, `decodeType`.

   **This matters, and it is why the catalogue is not used here.** `bind()`
   returns a filled `sPackBindResult`, so it must confirm capabilities
   *synchronously*; the catalogue arrives later as an `mbEvt_pointDesc` burst on
   the modbus task, which cannot satisfy a synchronous return. Using it would
   reproduce, in `bind`, exactly the acceptance/completion confusion that
   `submit` was designed to avoid.

   `modbus.h:474` warns that `Modbus_PointInfo` is a flash walk and "belongs on
   a control path and not in a loop". A bind **is** a control path — once per
   instance per config change — so the loop is the sanctioned kind. At ~114
   points that is a bounded, one-off cost, and it happens on the shared task,
   which is why §13 marks `bind` as the one call that may block briefly.

   **The catalogue burst is still consumed**, but only for what it is good at:
   noticing on `mbEvt_config` that ordinals moved, which triggers a re-bind.

   **Binding is by physical address at the device level and by point *name* at
   the point level**, so a reordered `devices[]` or a renumbered point is a
   *lost binding* (reported) rather than a *wrong battery* (silent).
4. Confirm each capability against what the walk actually found, and each
   command against `MB_PT_WRITE` plus the point's `writeMin`/`writeMax`,
   narrowed by `cmdAllow`. Fill `sPackBindResult`.
5. Issue one `Modbus_Request` for `device_address` (4360); on read-back match,
   set `packFlag_bindVerified`. This is the one asynchronous step, and it is
   deliberately *not* a precondition of the bind: the instance binds, comes
   online, and gains the flag when the answer arrives. This is §11a.5's proof turned into a runtime
   check — three self-consistent answers were what proved the map right, and a
   pack can prove its own binding the same way.

Then the frame rule, which follows from Part I §8: **`mbEvt_txn` is a leading
marker**, so a txn for this device *closes the previously open frame* (publish)
and opens a new one; `tick` closes a frame no further txn followed.

SOC and SOH arrive as packed bytes (`balsta_soc` lo, `soh_precharge` hi) because
there is no `u8` decode type; the type splits them, as the MQTT bridge does.

**`pack_pylontech` (push).** Accumulates into staging from the CAN RX path,
publishes when its `rxMask` completes or when `tick` finds a set older than
1.2 s. It reads the **packed frame structs** in `pylontech.h` and never touches
`sPylonBatteryData`, whose fields are floats. Capacity is absent from the
implemented frame set, so the instance advertises no `packCap_capacityAh` — which
is precisely why `nameplate_mAh` is always valid.

---

## 12. Configuration

Its own uploaded JSON, because a non-Modbus pack has no Modbus config to derive
from. Streaming-parsed from the HTTP body cursor on the `wg_conf.c` pattern — no
whole-document buffer.

```json
{
  "version": 1,
  "packs": [
    { "name": "sodas_a", "type": "jkbms", "bind": "rs485:2",
      "nameplate_ah": 660, "cells": 16, "chemistry": "lfp",
      "staleAfter_ms": 15000, "cellStaleAfter_ms": 60000,
      "commands": {
        "chargeEnable": true,
        "dischargeEnable": false,
        "chargeLimit": { "min_a": 0, "max_a": 200 }
      } },

    { "name": "sodas_b", "type": "jkbms", "bind": "rs485:15",
      "nameplate_ah": 300, "cells": 16, "chemistry": "lfp" },

    { "name": "rack_c", "type": "pylontech", "bind": "can:0",
      "nameplate_ah": 261, "cells": 16, "chemistry": "lfp" }
  ]
}
```

### `bind` is an opaque string the TYPE parses — and it names a physical address

The core never learns what it means. What each type reads:

| Type | `bind` | Resolves via |
|---|---|---|
| `jkbms` | `"<port>:<slaveAddr>"`, e.g. `"rs485:2"` | `Modbus_DeviceList()` → the entry whose `portId` and `slaveAddr` match, giving `devOrd` |
| `pylontech` | `"can:<nodeId>"` | the CAN RX dispatcher's node filter |

**What the parser may check about `bind`, and what it may not.** The core does
not know what a bind token means, so it validates only shape: **non-empty, at
most `PACK_BIND_LEN - 1` characters, printable, no control characters**. It does
**not** know that `jkbms` tokens look like `"<port>:<slaveAddr>"`, and must not
learn — a token that is well-formed but names nothing real, `"rs485:99"`, is
accepted by the parser and fails later at `bind()` as `packWhy_noBinding`
(§10.5). That split is deliberate: syntax is the core's, meaning is the type's.

**Why not `devOrd`, and why not `topicPrefix`.** A `devOrd` is a *position in
another module's array*: adding the Solis as `devices[0]` — which
`docs/modbus.md` §6's "id must equal position, no gaps" rule actively encourages
— silently rebinds every pack after it to a different battery, with both configs
still valid and both boards reporting healthy. That is §11a.3's wrong-but-aligned
write promoted from registers to whole devices.

`topicPrefix` is not the answer either: `docs/modbus.md` §1.2 lists *deriving a
device's identity from a consumer's string* under **do not re-propose**, and the
compiler does not guarantee it is unique.

`{port, slaveAddr}` is **unique by construction, stable under array reorder, and
changing it means "I physically moved the battery"** — which is the honest
meaning. `Modbus_DeviceList()` already returns both fields. The type re-resolves
on **every `mbEvt_config`**; zero matches or more than one leaves the instance
`packCond_absent` with a stated reason rather than binding to whatever is there.
And `packFlag_bindVerified` (§11.2 step 4) closes the class rather than narrowing
it.

### The rest of the schema

- **`name` is the identity.** Unique, `[A-Za-z0-9_-]`, ≤ 15 chars. Persisted
  per-pack state keys on it, so reordering the array cannot reattach one pack's
  history to another.
- **`commands` is a narrowing allowlist, and narrowing only.**

  | | Meaning |
  |---|---|
  | the whole block **absent** | every command the type confirms is allowed |
  | block present, a command **listed `true`** | allowed |
  | block present, a command **listed `false`** | forbidden |
  | block present, a command **not listed at all** | **forbidden** — that is what "allowlist" means |

  The third row is redundant with the fourth and is kept deliberately: an
  explicit `false` documents an intentional deny for the next operator, where an
  omission looks like an oversight. **The distinction between "block absent" and
  "block present but empty" is the one that carries meaning** — the first allows
  everything, the second allows nothing.

  A bound **wider** than the type's ceiling is a **422 at parse time**, not a
  silent clamp. This is what makes it safe to forbid `dischargeEnable` on a pack
  that must never be opened remotely — and it is where the answer to "who may
  disconnect this battery" belongs.

  **"Wider than the type's" needs a referent, and `cmdsMax` is only a mask.** So
  `sPackType` also carries a numeric **ceiling** — the widest bounds any instance
  of that type could ever accept (§11). The parser resolves `"type"` against the
  registered blueprint, which exists because `PackType_Register` runs at init,
  long before any upload; **an unregistered type name is a 422 in its own right**,
  so there is no case where the parser must guess. This is the only place a
  numeric ceiling is declared; nothing else may invent one.
- **Unknown keys are rejected**, as the Modbus compiler does — the useful
  failure mode, and what makes a stale config fail by name.
- **No built-in default.** A board with no pack configuration is
  **unprovisioned**. `packCond_absent` is a *different* state: configured,
  bound, never answered.
- Defaults per type: `staleAfter_ms` 15000 for `jkbms` (three missed 5 s laps),
  5000 for `pylontech` (five missed 1 Hz frames); `cellStaleAfter_ms` 60000 for
  `jkbms` (four missed 15 s cell laps).

### Surfaces

| | |
|---|---|
| `POST /api/pack/config` | upload; 422 with the offending field |
| `POST /api/pack/config/verify` | validate, write nothing |
| `GET /api/pack/config` | re-serialise the active config |
| `DELETE /api/pack/config` | become unprovisioned |
| `GET /api/pack/status` | per instance: name, type, cond **and `why` when not online**, caps, cmds, per-group ages and stale bits, the state |
| `GET /api/pack/cells?pack=<name>` | the `sPackCells` snapshot |
| `POST /api/pack/command` | **202 Accepted** with a correlation id |
| `pack list \| status <name> \| cells <name> \| cmd <name> <cmd> <value>` | CLI |

**The command route returns 202, not 200-with-result**: a synchronous HTTP status
for an asynchronous wire operation is the same category error as a synchronous
`command()` return. The outcome appears in `/api/pack/status` and on MQTT.

`GET /api/pack/status` builds into a `pvPortMalloc` block rather than `resp_buf`
— not because one is CCM and the other is not (**both are**: `resp_buf` is
`.ccmram` and the heap is `.ccmheap`), but because it is **transient**, which is
the argument `http_server.c:1097-1101` actually makes.

---

## 13. Task, context and concurrency

| Piece | Context | May it block? | Notes |
|---|---|---|---|
| `pack_jkbms` Modbus subscriber cb | modbus task | **no** | copy from borrowed pointers into staging; publish on the txn boundary |
| `pack_jkbms` `fModbusReqDone` | modbus task | **no** | `PackType_CommandDone` and return |
| `pack_pylontech` CAN RX | **ISR** | **no** | fill staging, publish via the ISR path |
| `PackType_Publish` / `PublishCells` / `CommandDone` | **either** | **no** | context-correct critical section + non-blocking post |
| Core dispatch, condition, staleness, command timeout | shared func task | **no** | |
| `bind` / `unbind` / `submit` | shared func task | briefly | a catalogue walk is flash I/O; it is why these are task-only |
| Consumer callbacks, `fPackCmdDone` | shared func task | **no** | post and get out, like the MQTT bridge |
| `Pack_GetState` / `GetCells` / `Stats` | any **task** | — | copies under a critical section; **not ISR-callable** |
| `Pack_Command` | any task | **no** | validates and claims synchronously, posts the wire work |
| `Pack_Config*` | caller's task (http) | yes | an upload takes seconds; posting it would stall the task |

### The lock

`taskENTER_CRITICAL()` misbehaves from an ISR on this port; `taskENTER_CRITICAL_FROM_ISR()`
is the ISR form. The core has exactly one internal helper:

```c
/* The ONE place that knows there are two contexts.  Everything else calls
 * Enter/Exit and never learns which it got. */
static inline uint32_t CoreLock(void) {
    if (0u != __get_IPSR()) {
        return taskENTER_CRITICAL_FROM_ISR();
    }
    taskENTER_CRITICAL();
    return 0u;
}

static inline void CoreUnlock(uint32_t saved) {
    /* RE-CHECK IPSR; do NOT infer the context from `saved`.  Zero is a
     * legitimate interrupt mask, so a zero return does not mean "task
     * context" — inferring it would unlock the wrong way exactly when
     * interrupts were already unmasked, which is the hardest case to
     * reproduce. */
    if (0u != __get_IPSR()) {
        taskEXIT_CRITICAL_FROM_ISR(saved);
    } else {
        taskEXIT_CRITICAL();
    }
}
```

What makes this sufficient:

1. **The live slot has exactly one writer at a time and always inside
   `CoreLock`.** A commit copies ~90 B — masked for well under 1 µs.
2. **A reader also runs inside the lock**, so it cannot observe a half-written
   state.
3. **Staging is single-writer by construction**, and the core touches it only
   inside the same lock, so a CAN ISR cannot rewrite staging mid-copy.
4. **Nothing else in the module locks.** The subscription slot claim and the
   in-flight command claim reuse the same helper, for the same reason
   `Modbus_Subscribe` does: so they can return a real error synchronously.

**Why not a seqlock.** It looks attractive — no masking, wait-free writer — and
it is wrong here. A seqlock reader spins while the sequence is odd, so **a reader
at higher priority than a writer preempted mid-copy livelocks on a uniprocessor.**
`http` and `tcpip_thread` run at 24 and `EthIf` at 48, and the writer can be the
modbus task. One microsecond of masked interrupts is the cheaper guarantee.

### The tick

The shared task **must not block on `portMAX_DELAY`**, and this is the single
most important departure from the ZhagaFW template. Staleness is a wall-clock
timeout (Part I §8: silence is not failure, and a failure counter cannot see it),
and **a silent pack posts no events**, so an event-only task can never fire the
timeout it exists to enforce.

```
xQueueReceive(funcQueue, &evt, pdMS_TO_TICKS(FUNC_TICK_MS))   /* 250 ms */
```

`modbus_engine.c:459-465` already established exactly this shape and states the
reason in a comment — the 200 ms floor is what makes its sysmon deadline
meaningful. The pack tick does three things per wake: evaluate condition and
confidence for every instance, expire command deadlines, and call each type's
optional `tick`. 250 ms gives ≤ 250 ms of latency on a staleness transition
against budgets of 5–60 s, for ~4 wakes/s of a few microseconds each.

### Priorities and sysmon

| Task | Stack | Priority | Why |
|---|---|---|---|
| `func` | 512 words, **statically allocated in `.bss`** | `osPriorityNormal-1` (23) | Below `modbus` (24) so pack work never delays the bus; the same slot the MQTT bridge occupies, for the same reason |

`SysMon_TaskRegister(512U, 2000U)` from inside the task body, check in once per
loop. A 2 s deadline covers the worst legitimate iteration (a tick plus a `bind`
walking a catalogue). That is **13 of `SYSMON_MAX_TASKS`'s 16**.

The stack is a static `StackType_t` array via `xTaskCreateStatic`
(`configSUPPORT_STATIC_ALLOCATION` is already 1). That keeps 2 KB out of the
48 KB `.ccmheap`, which is where every other task's stack comes from and is the
constrained region.

### The shared task

```c
typedef enum {
    func_start = 0,
    func_tick,
    func_packedEventsStart,
    func_packEvt = func_packedEventsStart,
    func_packEvtLast = (func_packEvt + PACK_EVT_COUNT),
    func_packedEventsEnd = func_packEvtLast,
    func_last
} eFuncEvt;

_Static_assert(func_last <= FUNC_EVT_MAX, "packed event space overflow");
```

**What the pack's eight ids are.** These are the module's *internal* posts and
have nothing to do with `ePackEventType`, which is the consumer-facing mask. A
consumer never sees these; `func.c` never learns what they mean. Each carries
its instance index in the event's data word.

| Internal event | Posted by | From |
|---|---|---|
| `packInt_stateCommitted` | `PackType_Publish` | any context |
| `packInt_cellsCommitted` | `PackType_PublishCells` | any context |
| `packInt_commandDone` | `PackType_CommandDone` | any context |
| `packInt_liveness` | `PackType_NoteLiveness` | any context |
| `packInt_commandSubmit` | `Pack_Command`, after it claims the slot | any task |
| `packInt_release` | `Pack_Unsubscribe` | any task, incl. a callback |
| `packInt_configChanged` | `Pack_ConfigApply`, after it commits | the HTTP task |
| `packInt_rebind` | the `mbEvt_config` handler in `pack_jkbms` | the modbus task |

Eight exactly, and `packInt_last` is what `PACK_EVT_COUNT` is defined as — so the
count cannot drift from the list. Note Part I §7 describes Zhaga sizing each
client's range by its own `_last` enum; that is precisely this, with the enum
private to `App/Pack` and only its count exported.

**Staleness and command timeouts are NOT in this list.** They are evaluated on
the tick, because the thing being detected is the *absence* of an event — which
is exactly why the tick has to exist.

Two departures from Zhaga, both from Part I §7 and both kept:

- **Static queue allocation** — `xQueueCreateStatic` with storage in `.bss`.
- **Drops are counted, not only logged — and counted per client range.** The
  post function holds the packed event id when `xQueueSendToBack` fails, and the
  id's range identifies the client, so `sFuncStats.dropped[]` attributes the
  loss. `modbus_engine.c` already proves the pattern with per-timer `missed`
  alongside `s_droppedPokes`.

Queue: 16 entries × 8 B = 128 B. Worst case is three JK txn boundaries plus a
6-frame Pylontech set plus a tick inside one scheduling gap — under 12.

**Build it with exactly one client**, one `_last` and one drop-counter slot, in
the shape that admits a second without touching the first. Not as a framework.

---

## 14. Persistence

```c
nvdbUser_packCfg,     /*  4 KB — the uploaded configuration, rarely written */
nvdbUser_packState,   /* 16 KB — per-pack learned state, often written      */
```

Appended before `nvdbUser_last`, never renumbered, with a name in `s_userNames`
and a size in `s_targetSizes`. `Shared/NvDb/nvdb.h` is explicit that a module
*should* hold separate users for separable concerns and names this exact case.

- **`packCfg`** is one `App/nv_record.h` versioned-CRC record holding the
  parsed, bounded instance array (8 × ~64 B + header ≈ 550 B). Deliberately
  **not** a record stream in the Modbus style: that machinery exists because a
  register config is unbounded and must be walked from flash, whereas this is
  eight tiny records that are RAM-resident by design.
- **`packState`** is **reserved and unused** until the estimator lands, and will
  then use the `wg_time.c` append-only slot-ring — one write per 10–15 min,
  which is what that pattern exists for.

### OTA migration — the part that is easy to miss

Adding users to `s_targetSizes` does not take effect by itself.
`nvdb_layout.c` re-applies the built-in layout only when
`nvdbDir.layoutVer < NVDB_TARGET_VER`, so:

1. **Bump `NVDB_TARGET_VER` to 2 in the same commit.** Without it a board
   already running `periphnet v1` never allocates the new users,
   `NvDb_GetSize` returns 0, and every access fails `nvdbRes_outOfBounds`.
2. **A relayout applies at the NEXT boot and only there.** So the first boot
   after the OTA that introduces packs is unprovisioned-with-no-space; the
   second has the areas. `Pack_Init` must treat `size_bytes == 0` as
   *unprovisioned*, not as a fault. **State it in the release note.**
3. **Reserve `packState` now, in the same version bump**, even though nothing
   writes it. Adding it later is a second bump and a second two-boot migration
   on every deployed board.
4. **A board carrying an operator layout (`NVDB_DIRFLAG_OPERATOR`) is not
   re-laid-out at all** — the operator's layout wins, and such a board stays
   unprovisioned for packs until a new layout is uploaded. Correct behaviour;
   `GET /api/nvdb/usage` shows it.
5. **`FwuCtl_BlContractHolds()` must still hold afterwards.** The new users are
   appended above the pinned FWU areas so the packer should not move them — but
   that is exactly what `POST /api/fwu/install`'s 409 exists to catch, and it
   must be checked on a real board before the image ships.

`NvRecord_Load` collapses absent, truncated, wrong-version and corrupt into one
answer — "nothing valid stored" — so an old image reading a new record, or the
reverse, becomes **unprovisioned and says so** rather than misreading a pack's
binding. Configuration is re-uploaded; nothing is guessed.

---

## 15. Memory and CPU budget

Current occupancy: `.bss` 91 136 B + `.data` 464 + `._user_heap_stack` 1 536 ≈
**93 KB of 128 KB** main SRAM; `.ccmram` 11 884 + `.ccmheap` 48 000 =
**59 884 of 65 536 CCM (91.4 %)**; text+rodata ≈ **397 KB of 480 KB** flash.

| Allocation | Size | Region |
|---|---|---|
| `sPackState` live + staged, 8 instances | 132 × 2 × 8 = **2 112 B** | `.bss` |
| `sPackCells` live + staged, 8 instances | 80 × 2 × 8 = **1 280 B** | `.bss` |
| Per-group tick array | 8 × 8 × 4 = **256 B** | `.bss` |
| Instance config records | 8 × 64 = **512 B** | `.bss` |
| Subscription table | 8 × 16 = **128 B** | `.bss` |
| In-flight command slots | 8 × 32 = **256 B** | `.bss` |
| `func` queue storage + control block | **208 B** | `.bss` |
| **`func` task stack** | **2 048 B** | **`.bss`** — via `xTaskCreateStatic`, deliberately not `.ccmheap` |
| `pack_jkbms` state (name→ptOrd maps, frame assembly) | 8 × ~160 = **1 280 B** | `.bss` |
| `pack_pylontech` state | 8 × 64 = **512 B** | `.bss` |
| `GET /api/pack/status` JSON | ~2 KB transient | `.ccmheap` via `pvPortMalloc` |
| **Total resident** | **8 592 B ≈ 8.4 KB** | **`.bss` — CCM untouched** |
| Flash | **12–16 KB** | ~83 KB free |
| External flash | 4 KB + 16 KB | ~7.3 MB free |

**CPU.** Per JK pack: one 8-point txn per 5 s → 8 subscriber callbacks (~1 µs
each) plus one commit. Per Pylontech pack: 6 frames/s in ISR (~2 µs each) plus
one commit/s. A commit is a 60 B `sPackRaw` memcpy under lock ≈ 0.3 µs. Tick at 4 Hz ≈
5 µs. Three packs aggregate to **well under 0.1 %** — it will round to 0‰ or 1‰
in sysmon. **Worst-case interrupt-off time introduced: ~1 µs.**

---

## 16. What this module will not do

1. **`Pack_WriteRegister()` or any generic write-through.** §11a.3 is the
   argument: two config points aimed at the wrong settings would have written a
   4.3 V under-voltage protection and a 2.0 V over-voltage protection into a
   live pack, and the BMS accepts it silently. Commands are a closed, named,
   bounded set or they are nothing.
2. **Any retry, or any "assume it obeyed", on `packErr_unknownOutcome`.**
   Retrying `chargeEnable` blind is a decision about a battery, and the module
   has no basis for it.
3. **A "disconnected" flag, or consumer-sensed disconnection detection inside a
   pack.** Part I §4: only something seeing several packs can sense it, currents
   differ by 7× between packs 6 mV apart, and a pack that has just left the bus
   still reads the right voltage. A pack claiming this would be confidently
   wrong.
4. **Anything named cluster, membership, aggregate or "the battery" in
   `pack.h`.**
5. **A second `HAL_CAN_RxFifo0MsgPendingCallback`.** `bms_reader.c:158` already
   defines the single weak symbol; a second definition is a link error and
   "whoever wins" is not a design. **`pack_pylontech` is blocked until `App/Can`
   grows one RX dispatcher** (`Can_RxSubscribe(id, mask, cb, ctx)`) that owns the
   callback and fans out to `bms_reader` and the pack type. A prerequisite, not
   a detail.
6. **Consuming `sPylonBatteryData`.** Its fields are floats and it is a display
   struct. The type parses the packed frame structs into integers directly.
7. **A `Pack_Start` / `Pack_Stop` / a per-pack task / a `Pack_GetStatePtr`.**
   Each is a knob on something that is config, or a borrowed pointer with a
   lifetime rule the copy already removed.
8. **Writing pack state to flash at update rate.** The estimator's ring at one
   write per 10–15 min is the shape.
9. **A test pack type behind a build flag.** If it is built, it is bound by
   configuration, exactly like `modbus_test_port.c`.
10. **A per-instance `caps` not confirmed against the live transport.**
    Advertising a capability with no accessor, or a command against a config
    carrying no writable point, is how a consumer learns to distrust the whole
    API.

---

## 17. Testing

**Defer the test pack type; do not defer testability.**

`pack_fsm.c` — condition, staleness, confidence decay, command validation and
bounds, per-group ages — ships as libc-only App code compiled by `tests/` **from
day one**. That is where every interesting failure lives: a stale transition
mid-command, a group never delivered, a late completion, a bound that would
widen. `App/Net/wg_conf.c` is already host-tested that way.

The type itself defers for two reasons pointing the same way:

1. **The JK path is already testable end-to-end without a battery.**
   `App/Test/modbus_test_port.c` is a registered driver in port slot 1, bound by
   configuration, fed by `modbus inject` — so `pack_jkbms` can be driven through
   the real subscription surface, real catalogue and real txn ordering, with
   staged replies and staged silence. A `sim` pack type would test the core
   against a fake that bypasses the surface most likely to be wrong.
2. **Deferring costs nothing structurally**, precisely because binding is
   configuration and not a build flag. Adding `"type": "sim"` later changes no
   image structure, no boundary rule, no CMake check and no consumer.

Leave `packType_sim` **unallocated** in `ePackTypeId` and append it later — the
enum is persisted and append-only, so this needs no reservation and no
placeholder.

---

## 18. Still open, and follow-ups

**Open — neither blocks implementation:**

1. ~~What the physical disconnect actually is.~~ **Answered: breakers** (§4).
   Manual, upstream of the BMS, no local evidence — so consumer-sensed detection
   is the case that matters, and a single-pack site cannot do it. The design is
   deliberately not breaker-specific; contactors, fuses and degrading busbars
   all reduce to the same obligation.
2. **Which Pylontech dialect the Dyness speaks**, whether it accepts any inbound
   command, and whether several packs are distinguishable on one bus. Affects
   `pack_pylontech`'s capability declaration and its `maxInstances`, not the
   contract. **Deferred by decision — a question for when the hardware
   arrives.**

**Prerequisite, tracked separately:** the `App/Can` RX dispatcher (§16 item 5).

**Follow-ups outside this document:**

- `configs/gen_jk_config.py` names plans `jk0/jk1/jk2` positionally. **Name them
  by role** so `pack_jkbms` can find them by name once `planMask` routes events.
  Until then it subscribes `MB_PLAN_ALL` and filters by `devOrd`.
- **`CLAUDE.md` will need the module and the `func` task** — a row in the
  FreeRTOS task table (512 words, priority 23, deadline 2000 ms), the two nvDb
  users in the external-flash map, and `App/Func/` + `App/Pack/` in the project
  structure. Implementation-time, not now.
- **`App/Data/telemetry.c` is not this path and should stop being described as
  it.** `CLAUDE.md` calls it "RESERVED … intended for the BMS→CAN path"; that
  path is now `pack.h` → cluster → CAN, and `sEnergyTelemetry` is
  inverter-shaped with no room for per-pack data. Either repurpose it for the
  Solis side or delete it, but do not wire packs through it.
- ~~The `mbEvt_txn` ordering assumption in
  [design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md).~~
  **Done 2026-08-25** — its §7 now carries the verified finding (txn is a
  leading marker, a transaction is a read block, no end-of-sequence event) and
  its §10 item 8 is closed.

# Modbus — the common module

**Status:** current behaviour + the design it is being rebuilt into ·
consolidated 2026-08-08 · **§2 rewritten 2026-08-10 after review**, and the
**v2 record format settled the same day** (§2.6 cardinalities, §3.2 layout) ·
**open decisions are being closed one at a time before implementation starts** —
Q2/Q3/Q4/Q6 closed 2026-08-11, and the config **split into capabilities and
plans** the same day ([§2.6](#26-capabilities-plans-devices-and-parameters));
the queue is [§2.15](#215-open-questions) — see [§8](#8-provenance)

Single source of truth for Modbus in PeriphNet.

**Scope discipline:** §2 is the design — the API, the port contract, the device
model and the scheduler. §3 is what is on the board today, which is none of it
yet. Anything past the module's own boundary stays out: a reworked write path,
MQTT-side rate policy and further dialects are named in
[§7](#7-known-limits) as limits, without proposed solutions. Earlier drafts
carried all of it, which made the document long and the design under-decided at
the same time.

| Section | Read it when |
|---|---|
| [§1 Purpose and module boundary](#1-purpose-and-module-boundary) | orienting |
| [§2 **The design**](#2-the-design) | **now — this is the open work** |
| [§3 What exists today](#3-what-exists-today) | implementing against it |
| [§4 Config JSON reference](#4-config-json-reference) | authoring a config |
| [§5 Operator reference](#5-operator-reference) | driving the board |
| [§6 Testing](#6-testing) | changing anything |
| [§7 Known limits](#7-known-limits) | planning what comes after the API |

Related, separate: [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
(why autonomy constrains this) and [pylontech_can_protocol.md](pylontech_can_protocol.md)
(a future consumer).

---

## 1. Purpose and module boundary

### 1.1 What the module is for

Poll Modbus RTU slaves described by an uploadable config, decode their
registers, and **hand every reading to whoever subscribed to it**. It does not
know what MQTT is, what Home Assistant is, or what CAN is.

Its whole job, stated as narrowly as it will go: **know what to read and how to
read it, translate registers to values and values back to registers, and hand
the result on.** Nothing else — no per-consumer policy, no memory of values
already seen. [§2.2](#22-the-module-reads-and-translates-it-does-not-remember)
is that rule and its consequences.

Today the boundary is untrue in both directions: `modbus_walker.c` includes
`App/Mqtt/mqtt_bridge.h` and calls `MqttBridge_Publish()` from inside its poll
loop (`modbus_walker.c:183`), and `mqtt_bridge.c` reaches back into
`Shared/Modbus/modbus_config_store.h` to walk the record stream itself for HA
discovery and set-topic resolution.

### 1.2 The boundary

```
                    ┌──────────────────────────────────────────┐
   App/Cmd ────────►│  App/Modbus/modbus.h   (the only header  │
   App/Http ───────►│                         anyone includes) │
   App/Mqtt ───────►│                                          │
   App/Can  ───────►│  ┌────────────────────────────────────┐  │
   App/Data ───────►│  │ scheduler   timers → events        │  │
        ▲           │  │ engine      FSM, sequences, decode │  │
        │ events    │  │ ports       uart · test  (§2.5)    │  │
        └───────────┤  │ config      facade over Shared/    │  │
                    │  └────────────────────────────────────┘  │
                    └──────────┬───────────────────────────────┘
                               │ may depend on
                               ▼
             HAL · CMSIS-OS/FreeRTOS · Shared/Modbus · w25q128 · trice
```

**Rules:**

- Files in `App/Modbus/` must not include `App/Mqtt`, `App/Http`, `App/Can`,
  `App/Data`, or any lwIP header.
- Everything outside the module includes only `App/Modbus/modbus.h`. Ports,
  the scheduler, the engine and the framing layer are all module-internal —
  in particular there is no way to select or configure a port from outside,
  because a port is where a device lives and that is config (§2.5, §2.6).
- **No device-specific code lives here.** A device is described by config, not
  by a driver — see §1.3.
- Consumers may use `Shared/Modbus` **types** (`modbus_records.h` — enums, unit
  codes, bounds; it is a layer below everyone). They must not call its **flash
  accessors** (`MbCfg_*`, `MbCfgStore_*`) — that is the coupling this API
  removes.
- All rules are greppable; enforce them in CMake, in the spirit of the existing
  check that keeps `bootloader/secrets*.c` out of the application.

`Shared/Modbus/` stays pure (libc + w25q128), host-testable, and out of
`SHARED_SOURCES` so it never links into the 32 KB bootloader.

### 1.3 A device is a config, not a driver

`jk_bms.c/h` is **not part of this module** and does not survive the refactor.
It exists because JK BMS support arrived before per-device baud did; everything
it does is expressible without device-specific firmware:

| `jk_bms.c` does | replaced by |
|---|---|
| a hardcoded FC03 read of block `0x1400` | a transaction in a device type, `startAddr: 5120` |
| ASCII decode of model / hw / sw version | `decodeType: "ascii"` points — already supported |
| its own USART2 init at 115200 | the device's `baud` parameter ([§2.6](#26-capabilities-plans-devices-and-parameters)) |
| byte-addressed registers | the type's address stride — data, not code (§2.6) |
| "is a JK actually there?" for bring-up | `Modbus_Probe` — generic, config-independent |

**The JK BMS becomes a device type, on the same footing as the Solis config.**
That is what makes the module worth extracting at all: the next device after JK
must cost a JSON file, not a `.c` file. The rule that keeps it that way —
**types carry data, never behaviour** — is stated in §2.6, because a type table
that may hold code is just `jk_bms.c` with extra steps.

### 1.4 Why the API first

Everything §2 describes past the API — the port contract, the device model, the
event-driven scheduler — changes the module's *insides*. While consumers reach
into those insides, each of those changes also churns `mqtt_bridge.c` and
`http_server.c`. Freeze the surface, then rebuild behind it. That is why
[§2.16](#216-implementation-order) puts the whole engine rewrite after the API
extraction, with a line drawn between the steps that hold behaviour constant
and the steps that do not.

The visible milestone is deliberately small: **a Trice subscriber that dumps
every decoded reading**, proving data flows out through the API with no MQTT
involved. MQTT then becomes just another subscriber.

---

## 2. The design

Nothing here is implemented. §2.1-§2.4 and §2.8-§2.15 are the module's
**surface** — what a consumer sees and may rely on. §2.5-§2.7 are what sits
**behind** it: peripherals, the device model, and scheduling. The split is the
point: the surface is what §2.16 lands first and holds constant while
everything behind it is replaced.

`App/Modbus/modbus.h` is written, compiles clean, and carries the contracts as
comments. This section is the rationale behind it, not a duplicate of it.

**As of 2026-08-11 this document leads the header.** Closing Q2/Q3/Q4 (§2.15)
changed the surface, and the header is edited by the §2.16 step that first acts
on each answer, so until then it is stale in exactly four places — listed here
so nobody reads it as current:

| `modbus.h` today | This document | Lands at |
|---|---|---|
| `Modbus_Subscribe(const char *deviceId, …)` | `(uint8_t deviceMask, …)`, `MB_DEV_ALL` | step 2 |
| `sModbusCompileError` mirror struct | `sModbusCompileResult` in `modbus_records.h` | step 1 |
| contract 6: ordinals valid within one generation | both ordinals are authored identities | step 2 |
| `OPEN Q2` / `Q3` / `Q4` comments | closed, §2.15 | steps 1-2 |
| `Modbus_SubmitWrite(sModbusWriteReq*, id)`, one point, async id | `Modbus_Request(devOrd, ids, values, count, timeout_ms, cb, ctx)`, §2.9a | step 1 shape, step 11 engine |
| `mbEvt_writeResult`, `MB_WRITE_NO_POINT` | deleted — the outcome goes to the requester's callback | step 11 |
| `Modbus_ConfigReset`, and `Modbus_Init` "provisions the built-in default" | `Modbus_ConfigErase`; no built-in exists, invalid regions are erased, §2.15 Q6 | step 6 |
| `sModbusPointDesc`: `MB_PT_WRITABLE`, `int16_t` bounds, no period | access bits (`MB_PT_READ`/`MB_PT_WRITE`), `int32_t` bounds, `period_sec` (0 = unmonitored), §2.6/§2.11 | step 6 |
| `Modbus_SubmitRawWrite`, `Modbus_ForceRefresh` | both deleted, §2.9a / §2.7 | step 11 |

### 2.1 Shape

| Group | Calls |
|---|---|
| Lifecycle | `Modbus_Init` — that is all of it, [§2.8](#28-lifecycle--set-it-and-forget-it) |
| Subscriptions | `Modbus_Subscribe` · `Unsubscribe` · `RequestCatalogue` |
| Commands | `Modbus_Request` ([§2.9a](#29a-requests--two-arrays-in-three-arrays-back)) · `Probe` |
| Configuration | `Modbus_ConfigVerify` · `Compile` · `Apply` · `Erase` · `Export` · `Status` |
| Diagnostics | `Modbus_Stats` · `LogStatus` · `SetMonitor`/`GetMonitor` |

Events: `mbEvt_sample`, `mbEvt_pointDesc`, `mbEvt_deviceState`,
`mbEvt_config`, `mbEvt_txn` — five, not six. `mbEvt_writeResult` is gone:
a write's outcome goes to the requester that asked for it, through the
completion callback it supplied (§2.9a), not to every subscriber.

There is **no** `SetBaud`/`GetBaud`, no `SetPort`/`GetPort`, no
`Start`/`Stop`/`IsRunning`, and no `InjectResponse`. Every one of them was a
knob on something that is now either config (§2.5, §2.6) or nobody's business
outside the module (§2.8). `eModbusErr` moves here from `modbus_rtu.h` because
it appears in events; `eModbusPortId` does **not** — ports are internal.

### 2.2 The module reads and translates; it does not remember

The rule, and the one most likely to be eroded by a convenient exception:

> Modbus emits **everything it reads**, as soon as it has decoded it, to
> whoever subscribed to that device. It does not deduplicate and keeps no copy
> of any value. Consumers decide what is worth acting on.

Three reasons, in the order they bite:

1. **Suppression policy differs per consumer and cannot be shared.** MQTT might
   want change-plus-heartbeat to keep the broker quiet. A CAN fusion path wants
   the freshest value every CAN frame and no suppression at all. A Trice dump
   wants literally everything. Serving all three from inside the module means
   the module carries every consumer's policy.
2. **Suppression forces the module to store values.** Change detection needs the
   previous value of every point — `s_lastValue[192]` + `s_lastPublishTick[192]`
   in CCM today. Doing it per consumer would need one such set *per consumer*,
   and each consumer that also caches for its own reasons pays for the same
   memory twice.
3. **It is not the module's job, and it has no basis to judge.** A module that
   knows what to read, how to read it, and how to convert registers ↔ entities
   is small enough to test exhaustively on the host. Whether a reading is worth
   forwarding depends on the transport and the audience, neither of which the
   module can see — which is why the publish policy leaves the config
   altogether (§2.4), rather than being carried through it.

**This is a rule about memory, not about routing.** Delivering a device's
readings only to the consumers that asked for that device stores nothing and
costs nothing — see §2.3. The module does keep **scheduling and bus state**,
which is about the wire rather than the data, and which no consumer could own:
per-timer due state (§2.7), missed-event counters, consecutive-failure counts,
and one frame buffer per port.

### 2.3 Subscriptions scope by device mask

```c
#define MB_DEV_ALL  0xFFu

int Modbus_Subscribe(uint8_t deviceMask, uint32_t eventMask,
                     fModbusSubscriber cb, void *ctx);
```

A bus carries several devices with unrelated configs and unrelated consumers. A
CAN-fusion consumer wants the battery packs and should not be handed an
inverter's registers on every sequence; a Trice dump wants everything.

**The scope is a bitmask, not a name, because the interesting consumer wants
several devices at once.** A BMS module fusing four JK packs subscribes once
with `0x0F` rather than holding four subscriptions — which is what collapses
the table from "one entry per device per consumer" to one entry per consumer,
and is why `MB_MAX_SUBS` is small (§2.15 Q3). `MB_MAX_DEVICES` is 8, so the mask
is a `uint8_t`; raising the device cap widens it.

Scoping by device is **routing, not suppression**, and none of §2.2's three
objections applies to it: dispatch is a bit test, it stores nothing about
values, and the answer is the same for every consumer of that device, so there
is no per-consumer policy to carry. What it buys is that each consumer's scope
is explicit at its registration site instead of buried in a filter inside its
callback.

Within that scope the subscription gets **every read** — that is where §2.2
binds.

**Bit N is the device at position N of the config's `devices[]` array, and that
position is the device's identity.** There is no separately authored id: the
array index and a device enum are the same thing under two names, and the module
owns the mapping. Two consequences, both deliberate:

- A bit with no device behind it is legal and receives nothing; if a later
  config defines that position, delivery starts. Consumers never re-register
  across a config swap, which is the property §2.3 exists to protect.
- **Reordering `devices[]` reassigns identity**, exactly as renumbering an enum
  in a header does. Appending is free; inserting in the middle re-points every
  observer scoped to a position after the insertion. That is the config author's
  discipline to keep, and it is the same discipline any enum demands.

`eventMask` is the same kind of thing one level up: a stateless 6-bit predicate
on the event *type*, which lets a consumer say "descriptors and samples, never
transaction diagnostics" without a branch in its own callback.

The device's `topicPrefix` still travels **in events** — MQTT renders it into
topics, and the engine already holds it in the device record it is servicing, so
the pointer is free. What leaves is the *retained* copy: a subscription stores a
bitmask, not a string.

### 2.4 Publish policy leaves the config entirely

`publish.threshold` and `publish.heartbeatS` are **deleted** — from the JSON,
from `sModbusPointRecord`, and from the event descriptor. They are not moved
into the module's API and not carried through it.

They were never Modbus's business. How often a reading is worth *forwarding* is
a property of the thing doing the forwarding — its transport cost, its
subscriber's tolerance for staleness, its database. A register has no opinion on
it, and the module has neither the information to judge nor anything to do with
the answer. Keeping the numbers in the Modbus config only made them look like
device facts.

**This costs nothing, because nothing uses them.** `publishThreshold == 0` means
"publish on every successful read" (`modbus_walker.c:156`), and the built-in
Solis config authors **no `publish` block on any of its 27 points** — so the
shipped behaviour today already is "every read, every point". Deleting the
feature reproduces it exactly.

Consequences, all of them simplifications:

- `MB_SAMPLE_CHANGED` and `MB_SAMPLE_FIRST` are gone; a sample event has no
  `flags` field.
- **The tracking arrays are deleted, not relocated.** `s_lastValue[192]`,
  `s_lastPublishTick[192]` and `s_hasPublished` (~1.9 KB of CCM) do not
  reappear in the MQTT bridge, because a bridge that publishes every sample
  needs no memory of the last one.
- The MQTT bridge's `mbEvt_sample` handler is `format → MqttBridge_Publish`,
  with no decision in it.
- ASCII loses its CRC32 dedup — an ASCII point republishes its unchanged string
  on every read. **No shipped config has an ASCII point**, so nothing observes
  it today. A consumer that wants dedup hashes the `text` it was handed.

**Formatting stays in the module's world; throttling does not.** Turning a
register into `-1234` or `23.7` is translation — the config says what the bits
mean, so `MbFormat_Scaled` and the bitfield-as-decimal rule remain the single
correct answer and stay in `Shared/Modbus` for consumers to call. Deciding
whether that string is worth sending is judgement, and belongs to whoever sends
it.

If a future consumer genuinely needs throttling, it configures it on its own
side — an MQTT-side rate policy is an MQTT design question, listed in
[§7](#7-known-limits) and not designed here.

### 2.5 Peripherals — the port contract

The module owns a **set of peripherals** and services them. A peripheral is not
a UART; it is anything that can carry a Modbus frame out and bring one back.
Two exist: the RS485 UART port, and a **test port** whose peer is a host-side
harness.

A port is a small record, not a class hierarchy:

```
  tx buffer + length      frame-sized; the frame to send lives here
  rx buffer + length      frame-sized; the reply lands here
  send handler            hand off the tx buffer, return immediately
  tx-result callback      sent / failed
  rx callback             frame complete / timeout / error
  buffer sizes
```

Three properties make this work, and each removes something from the engine:

- **The unit is a frame, not a byte.** The buffer is sized to hold a whole
  frame, so "send" means *send what is in the buffer, wait out the inter-octet
  timeout, then tell me the result*. End-of-frame detection — the 3.5-character
  silence that defines an RTU frame boundary — lives in the port, which is the
  only place that knows the line. The engine never runs a character timer.
- **Timing and line parameters are per-transaction, from device parameters**
  (§2.6). Baud, inter-octet timeout and response timeout are handed down with
  the frame rather than configured into the port, so a port is stateless with
  respect to any particular device and a device can move between ports without
  carrying anything with it.
- **The port completes asynchronously.** Send hands off to the driver (DMA where
  available) and returns; the engine goes on to form the next port's request.
  Completion arrives as a callback, and that callback — which may be in ISR
  context — **only posts an event to the modbus task**. No decode, no dispatch,
  no work of any kind. The RX buffer stays owned by the port until the task has
  consumed it, so nothing is copied.

The last property is why several ports genuinely overlap: the engine is bounded
by the CPU it takes to form requests and decode replies, not by the wire speed
of any one line. A second RS485 bus buys real throughput rather than just
address space.

**The test port replaces every test hook.** `Modbus_InjectResponse`, the
`modbus inject` command and `mbPort_disabled` all go. Where injection fed
a *response* in below the port and could never show what the engine
transmitted, the test port is the peer: the harness sees the request the engine
actually formed — address, function code, start, count, CRC — and answers it on
the same channel. That is strictly more coverage from strictly less firmware,
and it exercises the real framing, the real timeouts and the real state machine
instead of stepping around them.

Whether a test port is present is a **configuration** question, not a build
question: a device names the peripheral it lives on, so the same image serves a
bench board and a real one.

### 2.6 Capabilities, plans, devices and parameters

Four things, deliberately separated — and the **cardinalities are the design**,
not an implementation detail:

| | Cardinality | Holds | Example |
|---|---|---|---|
| **Port** | 1 : many devices | how frames get out and back | the RS485 UART; the test port |
| **Capability** | 1 : many plans | what the hardware *can do*: a flat list of points — address, decode type, unit, scale, **access**, write bounds — plus the address model | "JK PB BMS", "Solis inverter" |
| **Plan** | 1 : many devices | what we *watch*: periods, each naming points of one capability | "fast: SOC+pack every 5 s, cells every 60 s" |
| **Device** | 1 : 1 | its position in `devices[]` (= its identity, §2.3), slave address, baud, its port, its **plan**, `topicPrefix` | slave 1 @ 115200 on RS485, plan "fast" |

**Capability and plan are separated because they are facts about different
things.** A register map is a fact about JK BMS silicon and never varies. How
often to read cell voltages is a fact about this deployment. Welding them
together — which is what `readPeriodS` on a transaction did — forces every
device sharing a map to share its periods, so a spare pack cannot be polled
lazily while the primary is polled hard. It is the same category error as
`publish.*` living in the config (§2.4) and `topicPrefix` being the device
identity (§2.14), and it is fixed the same way: move the deployment decision out
of the thing that describes hardware.

**A device names a plan; the plan names its capability.** One reference on the
device, and a plan/capability mismatch is impossible by construction. A device
that is never polled — a register bank only ever written through
`Modbus_Request` — is expressed by a plan with no periods in it. An **empty plan
is legal**, and it is how "capable but unmonitored" is said.

**Only the communication data is per-device.** Port, capability and plan are all
shared, so four JK packs in parallel are four devices, one capability, one or
two plans, one port. That is why both are *referenced* on flash rather than
copied into each device (§3.2): the compiler's caps count records, so copying
would spend the 192-point budget on duplicates — a 50-point capability across
four packs would need 200 points and be rejected for a config holding 50
distinct ones.

The split is also what makes the engine port-agnostic. Moving a device to a
second bus because the first one is saturated is a change to one field. Nothing
about its register map, its identity, or any consumer's subscription moves with
it.

**Access is capability, and it means what the silicon supports:** `r` readable,
`w` writable, `rw` both. Write-only registers become expressible for the first
time — the v1 schema had no way to say it. Two rules follow and are enforced at
compile: a **plan may only monitor `r` or `rw` points** (a `w` point cannot be
read, so watching it is incoherent), and a **`w`/`rw` point must be a
1-register type on a `holding` function code**, since FC06 reaches the holding
space only.

**Transactions are not authored any more.** A capability is a flat list of
points and a plan is a list of (point, period); the compiler derives the read
blocks — grouping a period's points by function code and ascending address,
starting a new block at the 125-register ceiling. The **gap rule is the
interesting part**: a block may span points the plan did not select *if those
addresses are declared in the capability*, and may never span addresses the
capability does not mention. Bridging a declared gap costs two bytes per
register (~2 ms at 9600) against a whole extra round trip (~25 ms), so it is
usually worth it; bridging an *undeclared* gap risks the slave rejecting the
entire block with exception 2 (illegal data address), which is not worth
anything. Both facts come from data the config already carries.

**A port is an enum, not an authored record.** `portId` on the device record
indexes the module's static port table — the handles, buffers and state for a
peripheral the firmware was built with. Ports therefore carry no per-deployment
parameters, and a config naming a port this firmware does not have is rejected
at compile time rather than failing silently at run time. `eModbusPortId` lives
in `modbus_records.h` (the compiler and exporter need the name↔code table) and
deliberately **not** in `modbus.h` — nothing outside the module selects a port.

**Baud is a device parameter, not a bus property.** It belongs to the
device↔peripheral binding, which is why one wire can serve a Solis at 9600 and a
JK at 115200: the master owns every transaction, so the line is time-multiplexed
by construction, and a slave that sees traffic at the wrong rate drops it on a
framing or address mismatch exactly as it drops traffic addressed to someone
else. Because parameters travel with the frame (§2.5), a rate change is just the
next transaction's parameters.

Framing gaps derive from baud rather than being authored — 3.5 character times
at 11 bits per character, with the RTU floors of 1.750 ms / 750 µs above 19200
baud. At 9600 that is 4.01 ms, which is exactly the constant hardcoded today
(`modbus_rtu.c:122`), and the floors keep every value ≥2 ms so millisecond
scheduling still suffices at 115200.

**Capabilities carry data, never behaviour.** The first real dialect is already
on the bench: JK register addresses are *byte*-offset from the block base, so
the wire address is `base + index × 2` where a standard slave wants
`base + index`. Registers are still 16-bit and counts are still in registers —
only the address stride differs. That is one field on the capability and one
multiply in the engine.

The rule matters more than the field. If a capability may hold a function
pointer or a per-capability code path, then `jk_bms.c` walks back in through
the capability table, which is exactly what deleting it was for. Any future dialect must first be expressed
as data — an address transform, a function-code choice, a read cap — or be
refused.

It also explains a live bug: at offset 0 the stride is invisible, which is why
JK reads from a block base work today, while writes compute `startAddr + offset`
(`modbus_config_store.c:334`) and are wrong at any non-zero offset. §2.9a closes
it — a request's address comes from the same stride rule as a read's.

### 2.7 Scheduling — timers, sequences, and slipping

There is **no polling loop**. Scheduling is an independent, event-driven
instance that emits events at the periods the config asks for; the engine
services them. Nothing walks the config looking for work.

**Timers belong to a device and a period, and the plan is the timer list.** A
device's plan names some set of distinct periods — 30 s, 5 min, 1 h, 24 h — and
the device gets one timer per distinct period in its plan, not one per derived
read block. A device whose plan is empty gets no timers at all (§2.6). Deduplicating by period is what keeps the count
negligible: the shipped Solis config has two (5 s and 60 s), a board with a JK
alongside it maybe four to six. Left un-deduplicated the worst case is 8 devices
× 16 transactions = 128 timers off the CCM heap, which is the difference between
free and noticeable. FreeRTOS software timers are cheap enough that there is no
reason to build something else; the only discipline is that the timer callback
runs in the timer service task, so like the ISR case it **only posts an event**.

**A sequence is what one timer fires:** the transactions of one device that
share one period, run back to back on that device's port. Two useful things fall
out. A device's transactions all share its baud, so a sequence costs **one line
reconfiguration**, not one per transaction. And a pending request has an obvious
injection point — between transactions *within* a sequence — which keeps
requests responsive without ever interleaving on a half-duplex wire.

**A request batch runs whole, at one injection point.** §2.9a promises the batch
"belongs to one sequence on one port at one baud", and splitting it across
openings would make that untrue and complicate abandoning it on timeout. The
cost is stated rather than hidden: an 8-item batch is 8 round trips, ~0.5 s at
9600 baud, and that time comes out of the device whose sequence it interrupted —
so a large batch on a short period can show up in the missed counter. That is
the counter doing its job, not a fault.

**Timers free-run; service time never feeds back.** Because the scheduler is
independent of the servicer, nothing rearms a timer on completion, so a period
is a period and drift has nowhere to accumulate. There are no absolute-deadline
computations and no tick-wrap comparisons anywhere in the engine.

**Stacking is dropped and counted.** Event identity is the event plus its
argument — the device and the period. If the engine is handed a 5 s event for a
device whose previous 5 s event is still unserviced, the new one is **dropped
and recorded**. The same device's 60 s event is a different identity and is
unaffected. This is coalescing with an observable, and it makes the queue depth
trivial: it need only match the timer count, and a post that fails because the
queue is full is just another dropped event through the same counter.

**The missed counter is a capacity signal, not a curiosity.** A non-zero count
means the config is asking for more than the wire can deliver — which is exactly
the fact that says "this line needs splitting", the reason devices are
peripheral-agnostic in the first place. Per-timer granularity says *which* read
is starving. Exposed through `Modbus_Stats` and the config status JSON, plus a
Trice line.

**Failure backoff is an event filter, not a timer change.** A free-running timer
does not know a slave is dead and will keep firing its 5 s event forever, so the
existing "3 consecutive failures ⇒ offline, throttle to ≥30 s" rule cannot live
in the scheduler. It lives in the engine, which drops events for an offline
device until a retry window opens. Same rule, different home — and it has to be
deliberate, or a dead slave quietly eats bus time again.

**Config and device lifecycle drive timers directly.** Setting a device's
parameters starts its process; destroying a device, changing its peripheral, or
swapping the config kills its timers. That replaces the old "commit the swap at
a traversal boundary" rule with something simpler and per-device: stop the
timers, let anything in flight land, construct the new devices. A completion
that arrives for a device that is being torn down is discarded on a per-device
generation counter — no waiting, no locks.

**Not staggered, and first-come-first-served.** All of a device's timers start
at device init, so everything is due at once at startup and stays phase-locked;
sequences that come due together are serviced in arrival order. Both are
accepted as-is. They are latency effects on a line that serialises anyway, they
are visible through the missed counter if they ever stop being acceptable, and
neither is worth pre-solving.

### 2.8 Lifecycle — set it and forget it

`Modbus_Init` is the entire lifecycle. It initialises the flash store, recovers
the A/B regions, **erases anything that fails validation**, constructs whatever
devices the config describes and starts their timers. From then on the module
runs: it owns its peripherals, reads what the config tells it to read, and
services a request at the first opening between reads.

**There is no built-in default config, and none is provisioned** (§2.15 Q6). A
register map describes hardware the board may not have, so a fallback config is
a guess about the deployment — and a wrong guess polls a slave that answers
nothing while looking configured. The board is told what it is for; until then
it is not for anything.

**Zero devices is a valid, first-class state.** No devices, no timers, no bus
traffic, an empty catalogue (`last = 1` with `pt == NULL`, §2.11), no HA
discovery and no set-topic subscriptions. `modbus status` and
`/api/modbus/config/status` report it as **unprovisioned** rather than as an
error, because it is not one.

**Invalid is erased, not repaired.** A region that fails magic, version or CRC
is erased at init, so "invalid" collapses to exactly one observable state
instead of a spectrum of partially-readable ones, and the region is immediately
reusable by the next upload. This does not touch §3.4's invariant — nothing can
be walking a region before `Modbus_Init` has run.

Everything that used to let an outsider steer it is gone —
`Start`/`Stop`/`IsRunning`, `SetBaud`/`GetBaud`, `SetPort`/`GetPort`,
`InjectResponse`. None of them described anything a consumer needs. A port is
not a mode to be selected; it is where a device lives, which is config. A baud
is not a global; it is a device parameter. "Stopped" is not a state worth
having when "no devices configured" and "port disabled" already say everything
it said, and both are config.

The one call this reshapes is `Modbus_Probe` (§2.9).

### 2.9 Probe — the one direct-access hole

Every other path works off the compiled config. `Modbus_Probe` is the only way
to ask about an address that is **in no config**, which is what bring-up on an
unknown slave means: you do not yet know its address, its baud, whether it
answers FC03 or FC04, or whether a register block exists. It is also the only
API that hands out raw registers, deliberately — there is no config entry to say
what they mean. Telling exception 1 (illegal function) from 2 (illegal data
address) from 3 (illegal data value) apart is most of the diagnosis, which is
what `jk probe` was doing by hand.

It no longer takes the port. A probe is a one-shot transaction whose parameters
come from the caller instead of a device record, submitted to the engine and
serviced like anything else — which the port contract already supports, since
parameters travel with the frame anyway. The engine keeps running throughout.

Its result comes back by blocking the calling task on a completion semaphore.
The alternative — return an id, deliver the result as an event — is more
consistent with the rest of the engine, but `modbus probe …` printing a table on
one CLI line is the entire point of the call, and the bounded wait is on a task
that has nothing else to do.

### 2.9a Requests — two arrays in, three arrays back

The write path was §7's largest undesigned cluster. What settled it was framing
it as **the contract between the module and a requester** rather than as a
function-code question: the config already describes every accessible register,
so the module publishes that set with its data types and access, keeps
addresses, offsets and function codes inside, and the requester submits a
selection.

It is `Modbus_Request`, not `Modbus_Write`, because **the config decides what
each item means**, not the caller:

| Capability access | What a request does with it | `values[i]` on return |
|---|---|---|
| `r` — readable | reads the current value; the supplied value is ignored | the value just read |
| `w` — writable | writes the supplied value; no read-back is possible | unchanged |
| `rw` — both | writes, **then reads the register back** | what the register actually holds |

That is the protection, and it is stronger than a caller-side rule: a requester
cannot write a read-only register by asking harder, because asking is not how
the decision is made. It also collapses "read one register on demand" and "set a
register" into one call instead of two. If separate read and write entry points
are ever wanted, they are a narrowing of this one with the same guarantees, not
a different mechanism.

**Read-back is automatic, not requested.** Any write to an `rw` point is
followed by a read of it, so `values[]` always reflects what the register
actually holds and no caller can forget to verify. The cost is a second round
trip per written `rw` point, paid on every write including high-rate ones — a
per-request opt-in was weighed and rejected as a knob whose default would have
been "on" anyway.

**Access is capability, not policy.** It says what the silicon supports, so it
is a property of the capability (§2.6) and identical for every device using it.
What a device actually *reads on a schedule* is its plan, which is a separate
question with a separate answer.

**A request reaches the whole capability, not just the plan.** `ids` may name
any point the device's capability declares, monitored or not — the plan governs
what is polled, never what may be asked for. So a diagnostic register can be
read on demand without being added to anyone's plan, and a setpoint can be
written without being watched. This is the practical payoff of the split, and it
is why the catalogue publishes capability points with `period_sec` attached
rather than publishing the plan (§2.11).

```c
typedef void (*fModbusReqDone)(const sModbusReqReply *rep, void *ctx);

int Modbus_Request(uint8_t devOrd, const uint16_t *ids, const int32_t *values,
                   uint16_t count, uint32_t timeout_ms,
                   fModbusReqDone cb, void *ctx);
```

**Two parallel arrays in.** `ids[i]` is a `ptOrd`; `values[i]` is the value to
write, in the scaled-integer domain — the same domain `mbEvt_sample.value`
arrives in and `writeMin`/`writeMax` are authored in. One device per call:
`devOrd` is a parameter, the ids are points of that device's capability, and the whole
batch belongs to one sequence on one port at one baud, so "the module finished"
is a statement about one slave.

**A `ptOrd` is an identity, not a cursor** — the point's position in the config,
exactly as `devOrd` is the device's position in `devices[]` (§2.3). The module
owns the mapping and nothing internal leaks: a requester never sees a
`startAddr`, an `offset`, a stride or a function code. It carries the same price
as §2.3, and it is the same price an enum carries: **reordering points in the
JSON reassigns identity.**

**Three parallel arrays back**, delivered to `cb`:

```c
typedef struct {
    const uint16_t *ids;        /* as submitted                             */
    const int32_t  *values;     /* reads and read-backs land here           */
    const int16_t  *results;    /* per item: eModbusErr, incl. _timedOut
                                   and _notAttempted                        */
    uint16_t        count;
    uint8_t         devOrd;
    uint8_t         exc;        /* Modbus exception of the last failure     */
} sModbusReqReply;
```

*(Members are ordered largest-first so the struct carries no alignment padding —
the coding standard's rule, and worth following even on a struct this small
because it is the shape every requester copies.)*

**`results[]` holds one enum, not two.** *Timed out* and *not attempted* are
members of `eModbusErr` rather than a separate `MB_REQ_*` family — a caller
switching on a result should not have to know which of two namespaces a value
came from, and an `int16_t` carrying values from two enums is exactly the kind
of thing that survives review and then surprises someone.

The ids come back so a requester handling several batches can tell them apart
without keeping a correlation table. The values come back because reads and
read-backs are half the point. The results are per item because a batch is per
item.

**Every item is attempted.** A failure on item 3 does not stop items 4-8: the
module works the whole list and records what happened to each. There is no
"how far it got" summary, because `results[]` already says it per item, and no
rollback, because FC06 lands one register at a time and the wire cannot offer
one. **Interpreting the mix is the caller's job** — it asked for eight things
and it is told eight outcomes.

**`timeout_ms` bounds the whole request, and the module must respect it.** When
it expires the module stops, marks the unfinished items *timed out* (and any
never started *not attempted*) and calls `cb`. That is what makes the
memory rule safe rather than merely stated:

> **The completion callback always fires, within `timeout_ms`, and it is the only
> moment at which the caller may free or reuse the three arrays.**

Until then all three belong to the module — contract 3 pointing the other way,
and the same principle as §2.12a: the requester supplies the memory, the module
allocates nothing. A timed-out request is *abandoned*, not merely reported: a
response arriving for it afterwards is discarded and never written into arrays
the caller has been told it may reclaim. `cb` runs in the modbus task under the
same non-blocking rule as any subscriber.

**The deadline runs from submission, not from first service.** A request that
waits behind others in the FIFO spends its own timeout waiting, and may complete
with every item *not attempted*. That is the reading that actually bounds
the caller's memory: a deadline measured from the start of service could be
deferred indefinitely by traffic ahead of it, and the borrow with it.

**A config swap completes outstanding requests; it never drops them.** §2.7
tears devices down on a generation counter, and a request submitted against the
old generation is finished immediately — remaining items *not attempted*, a
config error on the ones whose ids no longer resolve — and `cb` fires.
It has to: "the callback always fires" is what lets a caller reclaim its arrays,
and a swap is not permitted to be the exception that strands them.

**In flight: a small FIFO** of submissions, drained in arrival order. Callers
rarely meet `mbErr_full`, which matters once several consumers can write —
and it is why the borrowing rule above is stated as loudly as it is, since the
module may be holding more than one requester's arrays at a time. Each entry
carries its own deadline.

Consequences worth naming:

- **`mbEvt_writeResult` is deleted.** An outcome belongs to whoever asked for
  it, not to every subscriber; the event enum drops to five types.
- **`writable: true` becomes `access`** in the JSON and two bits in the point
  record's existing `flags` byte (`MB_PT_READ`, `MB_PT_WRITE`), so it costs no
  record space. Record-shaped all the same, so it lands in v2 at step 6.
- **`writeMin`/`writeMax` must widen to `int32_t`**, matching the value domain.
  Also v2, or the format moves twice.
- **A point with `w` or `rw` access must sit on a `holding` transaction**, and
  the compiler now rejects otherwise (§7's silent wrong-space write). Access
  modes decide what the module *does*; this rule keeps what it does coherent
  with where the point lives, since FC06 reaches the holding space only.
- **FC16 becomes an internal optimisation, not a config knob.** Given ids and
  values, the module owns the address model and may coalesce contiguous
  registers into one frame. Whether a slave tolerates that is a dialect fact and
  would be a field on the capability, exactly as §2.6 requires — never a code path.
- **`Modbus_SubmitRawWrite` is deleted.** It was a hole straight through the
  protection this section is built on: the config decides what may be written,
  and a raw write decides it does not. The asymmetry with `Modbus_Probe` is the
  point — reading an unknown register cannot damage a battery, writing one can.
  It costs one real case, stated so it is not rediscovered as a surprise:
  changing a slave's own address or baud, where the config that names the device
  is invalidated by the very write it is asking for. The workaround is to
  declare that register `w` in the config, request it, then re-upload a config
  naming the device at its new address.

### 2.10 Configuration — verify without writing

Today's upload compiles straight into the inactive region, and "compile is
validation" was treated as making a dry run unnecessary. That conflates two
different things:

- **not activating** — already true; `apply` is a separate call.
- **not writing flash** — not true, and this is what a dry run is for.

An upload consumes the inactive region unconditionally, on success *and* on
failure. That region holds the previous config — the only other copy besides the
active one. So a fat-fingered upload today destroys the fallback while telling
you it failed.

```c
int Modbus_ConfigVerify(fModbusByteSource src, void *srcCtx,
                        sModbusCompileResult *err);
```

Same signature as `Modbus_ConfigCompile`, same compiler, same single pass —
**the record writes go to a counting sink instead of flash.** There is no second
validator to keep in sync, which is the property that made "compile is
validation" worth having in the first place; verify just swaps the output.

`sModbusCompileResult` is the compiler's own result type, not a copy of it —
see the Q2 resolution in [§2.15](#215-open-questions).

### 2.11 The catalogue

A consumer needs the point *list* before any value arrives. HA discovery is the
forcing case: it publishes one discovery message per point at MQTT connect time
and cannot wait for samples — a 60 s point would take a minute, and a point on
an offline device would never appear.

So the module replays a **catalogue**: a burst of `mbEvt_pointDesc`, one per
point in the subscription's scope, `last = 1` on the final entry. Delivered
after `Modbus_Subscribe`, after every config swap, and on demand via
`Modbus_RequestCatalogue` — which is what MQTT calls on broker connect, since
that moment is unrelated to when the config loaded.

The catalogue is not a violation of §2.2: it describes the *config*, which the
module owns and reads from flash on demand, not values, which it does not keep.

**A catalogue burst is the heaviest thing the dispatcher does**, and §2.12a
applies to it unchanged: 27 points on the shipped config means 27 back-to-back
callbacks, each borrowing a descriptor that dies on return. An observer that
copies-and-posts therefore allocates 27 times in a row, at config swap and at
every broker reconnect. That is bounded and infrequent, and it is the observer's
call whether to take it — HA discovery may equally publish inline, since nothing
in the contract forbids it and the burst is not on a latency path. The point is
that it is a *choice made knowingly*, not a surprise found later.

This replaced an earlier `Modbus_ConfigEnumerate(ops, ctx)` idea — a synchronous
walk the consumer would call itself. Catalogue-as-events wins on three counts:
one data path and one callback contract instead of two shapes for the same
information; every delivery comes from the modbus task, so flash records are
never read from a consumer's task alongside the engine; and "when do I re-read
the config?" stops being the consumer's problem, which today `mqtt_bridge.c`
answers by polling `MbCfgStore_ActiveBase()` on every loop.

Either way the config is walked from flash and never held in RAM — that property
is load-bearing and must not leak away through the API.

**The catalogue is also the requestable-set source.** `sModbusPointDesc` carries
the access bits, `writeMin`/`writeMax`, `decodeType`, `scalePow10` and the
ordinals, and deliberately no address, offset or function code — so "the full
set with its data types and access, internals withheld" (§2.9a) *is* the
catalogue. A requester reads the access bits to know whether an id will be read,
written or written-and-read-back. No second enumeration API exists or is needed.

**It carries `period_sec` as well, because capable is not the same as monitored.**
A device's catalogue is its **capability's** points — every point it can do —
while its plan decides which of them arrive as samples (§2.6). `period_sec` is the
plan's period for that point, or **0 for a point the plan does not watch**.
Without it a consumer cannot tell the two apart: HA discovery would create a
sensor for an unmonitored point and that entity would sit unavailable forever,
waiting for a sample nothing is scheduled to produce. With it, the bridge
creates sensors for `period_sec > 0` and number entities for writable points
regardless — a setpoint you can change but do not watch is a coherent thing.

That is what lets `MbCfg_FindWritablePoint()` disappear: MQTT builds its
`name → {devOrd, ptOrd}` map from the catalogue burst it already consumes for HA
discovery, and resolves an inbound `<topicPrefix>/<name>/set` against that
instead of walking flash from `mqttTask`. The table is small — the shipped
config has two writable points — and it is consumer memory, which is the correct
side of the boundary for it.

### 2.12 Contracts

Eight of them, in the header, and they matter more than the struct shapes.
Summarised: callbacks run **in the modbus task, synchronously**; they **must not
block**; **every pointer in an event is borrowed** and dies when the callback
returns; a subscriber cannot fail a sequence; and **both ordinals are authored
identities** — `devOrd` is the device's position in `devices[]` (§2.3), `ptOrd`
the point's position in its capability (§2.9a), and each survives a config swap for
exactly as long as the author leaves that array order alone.

That last one replaced the original contract 6, which said ordinals were valid
only within one config generation. It stopped being true when scope and write
addressing both became ordinals: an id that expires every swap cannot be the
thing a requester names. The residual risk moved rather than vanishing — a
requester that caches ordinals across a **reordered** config writes or reads the
wrong entry, which is the price §2.3 already documents and the reason
`mbEvt_config` is followed by a fresh catalogue.

Four of them are load-bearing under the event-driven engine:

- **Nothing dispatches from ISR or timer context.** Port completion callbacks
  and timer callbacks post events and return. Decode and subscriber dispatch
  happen in the modbus task, which is what keeps `MqttBridge_Publish` (and its
  `LOCK_TCPIP_CORE`) legal in a subscriber and Trice legal in a callback.
- **"Must not block" is the only rule**, and it is stricter than it looks:
  samples arrive per read rather than per change, so a subscriber's cost is
  multiplied by config size, not by how much the plant is moving.
- **Borrowed pointers are what make the whole dispatch model work** (§2.12a) —
  an observer that keeps anything copies it before returning.
- **Borrowing runs both ways.** A requester's `Modbus_Request` arrays are
  borrowed by the module until its completion callback returns (§2.9a); freeing
  or reusing them earlier corrupts a transaction still on the wire. The callback
  is guaranteed to fire within the request's `timeout_ms`, which is what makes
  that rule livable rather than an open-ended loan.

#### 2.12a Dispatch — the module hands over, the observer owns what happens next

The module calls the observer's callback synchronously and returns. **Delivery
is the call.** There is no module-side queue, no per-subscriber buffer, no
delivery-success notion and no drop counter: from the module's side a callback
that was invoked was served, and what the observer did with it is the observer's
business.

The pattern for an observer that does real work — MQTT publishing, CAN frame
assembly — is:

```
modbus task        observer cb: allocate, copy the fields it needs,
                                post the pointer to its own queue, return
observer task      pop pointer -> do the work on its own stack -> free
```

Allocation happens in the callback rather than from a static per-observer ring
because this memory is used rarely at runtime and by more observers than should
each reserve a worst case. The FreeRTOS heap is **heap_4**
(`configTOTAL_HEAP_SIZE 48000`, `ucHeap[]` in `.ccmheap`), which coalesces
adjacent free blocks, so cycling equal-sized event copies recycles cleanly.
`pvPortMalloc` briefly suspends the scheduler — bounded, legal from the modbus
task, and now on the sequence path, which is part of what "must not block"
covers.

**Nothing above is a contract.** What an observer does in its callback is
entirely its own discretion; the module imposes non-blocking and nothing else.
An observer that only needs to count, or to emit one Trice line, does that
inline and allocates nothing.

Two consequences worth stating rather than discovering:

- **A wedged observer is invisible from `modbus status`.** The `missed` counter
  (§2.7) reports bus capacity, which is the module's business; observer health
  is the observer's to expose.
- **The MQTT bridge publishes from `mqttTask`, not from the modbus task.** That
  keeps `LOCK_TCPIP_CORE` — which can be held by `tcpip_thread` running
  WireGuard crypto at the same priority as the modbus task — off the sequence
  path, and keeps `s_topic`/`s_payload` (`mqtt_bridge.c:55-56`, documented
  mqttTask-only) single-task-owned once HA discovery moves onto the catalogue.

### 2.13 What the consumers become

```c
Modbus_Subscribe(MB_DEV_ALL, mbEvt_all, trice_sink, NULL);

Modbus_Subscribe(MB_DEV_ALL, mbEvt_sample | mbEvt_pointDesc |
                             mbEvt_deviceState | mbEvt_config,
                 mqtt_modbus_cb, NULL);

Modbus_Subscribe(0x0F, mbEvt_sample | mbEvt_deviceState,
                 bms_fusion_cb, NULL);      /* four packs, one subscription */
```

- `mbEvt_pointDesc` → one HA discovery message per point;
  `Modbus_RequestCatalogue()` on broker connect re-drives it.
- `mbEvt_sample` → copy into an allocation, post to `mqttTask`, and format +
  `MqttBridge_Publish` there (§2.12a). No decision, no state.
- `mbEvt_deviceState` → the retained `<topicPrefix>/availability` topic.
- `mbEvt_config` → nothing to do; the catalogue that follows carries the new
  point set.
- Inbound `<topicPrefix>/<name>/set` → resolve the name to `{devOrd, ptOrd}`
  against the map built from the catalogue, then `Modbus_Request` with `count = 1`
  (§2.9a), keeping the deferral out of `tcpip_thread` exactly as it is now. The
  batch API subsumes the single-point case; there is no second entry point for
  it.

`http_server.c` keeps its endpoints and calls only `Modbus_Config*`.
`cmd_parser.c` calls only `Modbus_*`, and loses its `jk` command tree.

### 2.14 Device identity

**The module's device identity is the device's position in `devices[]`** — bit N
of a subscription mask, `devOrd` in an event (§2.3). It is not a string, and the
module retains no string per subscription.

That was not true in the first draft, where the identity was the config field
`topicPrefix` — a device named after one consumer's transport, harmless while
MQTT was the only consumer and wrong as soon as a second one subscribed by name.
The mask removes the problem rather than renaming it: nobody subscribes by name,
so there is no name to get wrong.

`topicPrefix` survives as exactly what it says it is — **MQTT's display string**,
carried in events for the one consumer that renders it into topics, HA
`unique_id`s and the device grouping. Whether the JSON eventually gains `"name"`
as a neutral spelling (§2.15 Q1) is now cosmetic: the forcing case that made it
an API question is gone.

**Writes are ordinal-addressed too** (§2.9a), so the module has no name-based
entry point at all. An inbound MQTT `<prefix>/<name>/set` topic carries strings
and nothing else, but resolving them is now MQTT's job, against the
`name → {devOrd, ptOrd}` map it builds from the catalogue. The string stays on
the side that owns the transport, and the module's whole addressable surface is
two ordinals.

### 2.15 Open questions

Numbered to match the `OPEN Qn` markers in `modbus.h`, which carries the same
list. **This section is where a question is closed**; the header's marker is
deleted by the §2.16 step that first acts on the answer, so a `Qn` present there
and closed here means "decided, not yet written into the surface". They are
listed in the order the [§2.16](#216-implementation-order) step that needs each
answer arrives, which is what makes "the next open decision" unambiguous.

- **Q1 — should `topicPrefix` get a transport-neutral spelling in the JSON?**
  Deferred, see §2.14, and **downgraded to cosmetic** by the Q3 answer: identity
  is now the device's position in `devices[]`, so nothing subscribes by name and
  `topicPrefix` is honestly named for the one consumer that renders it. (The
  question was originally "`deviceId` vs `topicPrefix`"; there is no `deviceId`
  in the JSON, and in the API it is now an ordinal.) Decided by step 6, where
  the schema moves anyway.
- **Q5 — Who owns device availability?** Listed last because it is answered
  rather than pending, and kept because its answer is contested: the module
  owns it, because the retry
  throttle is a bus decision only the module can make and two consumers deriving
  "offline" from `mbEvt_txn` independently would disagree with each other. The
  strict reading of §2.2 would push it out.

**Q6 closed 2026-08-11 — the firmware carries no built-in config at all.** The
question was "how many defaults", and both answers on offer — one config, or a
named set — assume a fallback is desirable. It is not. **Everything must be
configured for its purpose**: a register map describes hardware this board may
not have, so any built-in is a guess about the deployment, and a wrong guess is
worse than none because it polls a slave that answers nothing while reporting
itself configured.

So: no `g_modbusDefaultConfigJson`, no `ModbusConfig_EnsureDefault()`, and
`modbus_default_config.c/h` deleted — **~3.25 KB of `.rodata` returned** (the
measured size; §2.15 previously guessed 1–2 KB). On boot the module validates
its regions and **erases** whatever fails, and an empty config is a valid state
that reports as *unprovisioned* (§2.8). `Modbus_ConfigReset` becomes
`Modbus_ConfigErase` — with nothing to reset *to*, "reset" named the wrong
operation. *(The rename is the one judgement call here; the behaviour is not.)*

A v1→v2 migration was weighed against this and refused for the same reason
plus one more: it is one-shot code, tested against streams nobody will produce
again, and the device is in development with its configs living as files in the
operator's hands. Re-uploading is cheaper than carrying a translator (§3.2).

The rule is stated as a Modbus rule and claims nothing beyond this module. A
future unified parameter store may reasonably decide differently for parameters
whose defaults are harmless; a register map's is not.

**Q3 closed 2026-08-11 — scope is a device bitmask, and `MB_MAX_SUBS` stays 8.**
The question was "is 8 enough", and it was the wrong question: at one entry per
device per consumer, a fusion path over four JK packs spends four of them and 8
is marginal. The answer is that a subscription covers **several** devices — a
`uint8_t` mask over `devices[]` positions, `MB_DEV_ALL` = `0xFF` (§2.3) — so the
four-pack consumer spends one entry and the census is MQTT + Trice + fusion ≈ 3.
The entry is then `{deviceMask, eventMask, cb, ctx, inUse}` = 16 B padded, so
the table is **128 B**, in main SRAM (~60 KB free) rather than CCM (~6 KB) since
nothing about it is latency-critical. The retained `char[16]` device name is
gone with the name-based scope that needed it.

The identity question underneath it closed the same way: **the array position is
the id**, and the module owns that mapping — an authored `"id"` field was
proposed and rejected as a second name for the same thing. §2.3 records the
price (reordering `devices[]` reassigns identity, exactly as renumbering an enum
does) and §2.14 records the payoff (the module's identity stops being a
transport's string).

**Q4 closed 2026-08-11 — synchronous dispatch, and the module never queues.**
Not "synchronous for now, revisit when a consumer needs to block": the observer
is what blocks, in its own task, on its own memory. The module calls the
callback, and **delivery is the call** — there is no module-side queue, no
per-subscriber buffer, no delivery-success notion and no drop counter. An
observer that needs to do work allocates in its callback, copies what it needs,
posts the pointer to its own queue and returns; it handles and frees in its own
task scope. Allocation beats a static per-observer ring because the memory is
used rarely and by more observers than should each reserve a worst case.

**No new contract came with it.** What a callback does is entirely the
observer's discretion; the module imposes non-blocking and nothing else, so the
Trice sink of step 3 stays a file rather than a task plus a ring. §2.12a is the
mechanism and its two consequences — a wedged observer is invisible from
`modbus status`, and MQTT's publish moves off the sequence path into `mqttTask`.

**Q2 closed 2026-08-11 — the compile outcome is one shared type, not a mirror.**
The question was posed as a choice between mirroring `sMbCompileResult` in
`modbus.h` and re-exporting `modbus_config_compiler.h`, and it was called the
weakest call in this section. Both options are wrong for the same reason: the
result is **data**, `MbCfgCompile()` is a **flash accessor**, and §1.2 already
separates those — they were merely in one header. So the type moves down to
`Shared/Modbus/modbus_records.h`, the layer everyone may include, and is renamed
`sModbusCompileResult` to match that header's `sModbus*` prefix and the API's;
`sMbCfgCounts` moves with it (the result embeds it, so it is in consumer view
too) as `sModbusConfigCounts`. `modbus_config_compiler.h` keeps `MbCfgCompile()`
and `fMbByteSource` and stays un-includable by consumers, which is what the
step 14 CMake check enforces.

One struct, no copy, no translation layer, and §1.2 holds by construction rather
than by duplication. The deciding cost was not the ~30 lines: **v2 forces a
field change**, because a failure inside `capabilities[0].points[2]`
(§3.2, §4) has no device index, so the error location must grow — under a mirror
that edit lands in two structs plus the translation plus the 422 renderer.
`fMbByteSource` and the API's `fModbusByteSource` were deliberately left as two
typedefs: C treats them as the same type, so the facade passes callbacks through
with no cast.

The move itself is a rename over ~30 sites (compiler, store, `http_server.c`,
`modbus_default_config.c` (until step 6 deletes it), `modbus_walker.c`, three
host tests) and lands with
**step 1**, which is the first step that needs the type. Until then `modbus.h`
still carries the `sModbusCompileError` mirror and its `OPEN Q2` comment.

**Closed by the 2026-08-09/10 review**, recorded so they are not re-opened: baud
accessors, port accessors and `Start`/`Stop`/`IsRunning` (all removed, §2.8);
`InjectResponse` (replaced by the test port, §2.5); writes while stopped and
catalogue-before-first-sequence (both moot — no stopped state); ASCII `value`
(unspecified, because nothing in the module computes a hash); scheduler drift
and overrun (structurally answered by a free-running scheduler plus
drop-and-count, §2.7).

**Record layout for the device/capability split — closed 2026-08-10, extended
2026-08-11 by the capability/plan split (§2.6).** It was listed
here as premature; the cardinalities of §2.6 decide it. Maps are **shared
records referenced by ordinal**, not expanded per device, because the caps count
records and duplication would spend the point budget on copies. Ports are an
**enum byte**, not authored records, because a port carries no per-deployment
parameters. That makes the whole of v2 specifiable now, so the format moves
**once** — the point record's `publish` deletion (§2.4), the capability and
plan records, the device record's `portId`/`planOrd`/`baudCode`, and the period
widening to `uint32_t` all land together. The layout is §3.2; the consequence for `ptOrd` is
below.

**`ptOrd` is capability-relative, and identity is `{devOrd, ptOrd}`.** A shared capability read
from four packs yields four distinct samples from one point record, so a point
ordinal alone no longer identifies a reading. Both fields are already in
`sModbusPointDesc`; what changes is the meaning of one comment in `modbus.h`, and
that no consumer may key on `ptOrd` by itself.

### 2.16 Implementation order

The engine rewrite and the API extraction are separable, and the API goes first
so that consumers stop moving while the insides change.

1. **`modbus.h` as a thin facade** over the existing `ModbusWalker_*`,
   `Modbus_*` (rtu) and `MbCfg*` calls. The header exists; the facade behind it
   is this step. Carries the Q2 move (§2.15): `sMbCompileResult` and
   `sMbCfgCounts` become `sModbusCompileResult` / `sModbusConfigCounts` in
   `modbus_records.h`, a rename over the compiler, the store, `http_server.c`,
   `modbus_default_config.c`, `modbus_walker.c` and three host tests. The
   structs keep their shape, so `res.counts.devices` and friends are untouched
   and `ctest` is the whole check.
2. **Subscription table + dispatch** — 8 entries of 16 B in main SRAM, scoped by
   device mask, dispatched synchronously (§2.15 Q3/Q4). `publish_point()` stops
   calling `MqttBridge_Publish` and raises an event for every decoded point.
3. **`modbus_trice_sink.c`** — the first subscriber, plus `modbus dump on|off`.
   *The visible milestone: readings in Trice with no MQTT in the picture.*
4. **Catalogue replay** — on subscribe, on config swap, on request.
5. **MQTT bridge becomes a subscriber** — its callback copies and posts to
   `mqttTask`, which formats and publishes there (§2.12a); HA discovery onto the
   catalogue; set-topic writes onto `Modbus_Request` (§2.9a) with names resolved
   from the catalogue, which is what deletes `MbCfg_FindWritablePoint()`.
   `mqtt_bridge.c` drops
   every `Shared/Modbus` include and its `MbCfgStore_ActiveBase()` poll. **This
   is the step where publishing leaves the modbus task**, so it is also where
   `s_topic`/`s_payload` stop being touched by two tasks.
6. **The v2 record format, in one move** (§3.2). Everything the on-flash layout
   will ever need for §2 lands here, so it is never moved twice: the `publish`
   fields out of the point record and out of the JSON schema, the new
   capability and plan sections, the transaction record deleted,
   `portId`/`planOrd`/`baudCode` on the device record, periods widened to
   `uint32_t` on the plan entry, **`writeMin`/`writeMax` widened to `int32_t`
   and `fc` moved onto the point (§2.9a, §2.6)**, `MODBUS_LUT_VERSION` → 2. Compiler,
   exporter, store cursor and the host-test vectors move with it; the walker
   gains capability/plan resolution and derived read blocks (a device's reads
   now come from its plan over its capability, not from authored transactions)
   and loses its tracking arrays. **The new fields are stored, exported
   and validated here but not yet acted on** — one baud, one port and stride 1
   still, so behaviour is unchanged. Separate from step 5 so a regression
   bisects cleanly, and after step 5 so the MQTT bridge is no longer walking
   the config when the stream shape changes. **The built-in default dies here**
   (§2.15 Q6): `modbus_default_config.c/h` and `ModbusConfig_EnsureDefault()`
   are deleted, `Modbus_ConfigReset` becomes `Modbus_ConfigErase`, invalid
   regions are erased at init and *unprovisioned* becomes a reported state.
   Landing it in this step is deliberate — this is the step that invalidates
   every region, so the change that decides what happens next arrives with it
   rather than after it, and ~3.25 KB of `.rodata` goes back.
7. **`Modbus_ConfigVerify`** + `POST /api/modbus/config/verify`.

   *Everything above holds behaviour constant. Everything below changes the
   engine.*

8. **The port contract** — frame-in/frame-out, per-transaction parameters,
   completion callbacks that only post. The UART port first, behind the existing
   synchronous engine, so the contract is proven before anything depends on its
   asynchrony.
9. **The test port**, and the integration suite rebuilt on it: the harness
   becomes the peer and asserts on requests as well as replies. `modbus inject`,
   `modbus port` and their Trice contract lines go.
10. **Event-driven scheduling** — per-device timers, sequences, drop-and-count,
    failure backoff as an event filter, per-device teardown on config swap. The
    100 ms tick, the traversal and `s_lastPollTick` go with it.
10a. **The write path becomes real** (§2.9a). The *shape* lands at step 1 — the
    facade offers `Modbus_Request` and satisfies it with `count = 1` over the
    existing single slot, so consumers are written against the final signature
    from the start. The *engine* lands here: batch execution, stop-at-first-
    failure, the three-array reply, the submission FIFO, and the address taken
    from the capability's stride so a byte-addressed slave is written where it
    is read.
    `MbCfg_FindWritablePoint()` is deleted at step 5, when MQTT starts
    resolving names from the catalogue instead of from flash.
11. **Act on the device model** — §2.6 becomes behaviour rather than stored
    fields: `baudCode` drives the line parameters handed down with each frame,
    `portId` selects the port from the module's table, `addrStride` enters the
    address computation, and several devices share one capability. No record change
    here; step 6 already wrote the format. Host tests first.
12. **Delete `jk_bms.c/h`**, drop the `jk` CLI tree, land the JK config, and
    re-run the DeviceInfo read as `modbus probe` at 115200.
13. **`http_server.c` / `cmd_parser.c`** onto `Modbus_*`; `Modbus_Init()` moves
    to `App_DefaultTaskEntry`.
14. **Dependency check in CMake.**

**Acceptance for steps 1–7:** MQTT topics, payloads, retain flags, HA entity set
and every Trice string in [§6](#6-testing) unchanged — the integration suite is
the check; `ctest` stays green; `modbus dump on` prints every decoded point with
the broker down.

**Acceptance for steps 8–14:** the same MQTT and HA surface, reached through a
different engine. The suite is rewritten once, at step 9, and is the check for
everything after it.

---
## 3. What exists today

Shipped, working against the Solis inverter, host tests green.

### 3.1 Files

```
App/Modbus/modbus_rtu.c/h            RTU master: FC03/04/06, CRC, DE pin, monitor, inject
App/Modbus/modbus_walker.c/h         poll task, one traversal per 100 ms tick, publish pipeline
App/Modbus/jk_bms.c/h                JK DeviceInfo probe — TO BE DELETED, see §1.3
Shared/Modbus/modbus_records.h       record structs, enums, sentinels, bounds
Shared/Modbus/modbus_config_store.c  A/B selector + region validity + record cursor
Shared/Modbus/modbus_config_compiler.c  streaming JSON→records compiler
Shared/Modbus/modbus_config_export.c    active region → JSON
Shared/Modbus/modbus_decode.c        decode / quantize / format
Shared/Modbus/modbus_units.c         DLMS unit table + HA class mapping
```

### 3.2 Record stream

Flat, sentinel-terminated, no stored pointers or offsets — a linear scan is
self-describing, and nothing is ever RAM-resident.

```
[Device][Txn][Point]…[Point{decodeType=0}]   <- point sentinel
        [Txn][Point]…[Point{decodeType=0}]
        [Txn{count=0}]                        <- transaction sentinel
[Device]…
[Device{slaveAddr=0}]                         <- device sentinel (end)
```

Sentinels are full-size all-zero records, so readers always consume whole
records. `slaveAddr` 0 is the Modbus broadcast address (never a real slave),
`count` 0 is not a valid read length, `decodeType` 0 is reserved.

**As built — LUT v1, what is on the board today** (`modbus_records.h`):

```c
typedef struct __attribute__((packed)) {
    uint8_t  decodeType;      /* eModbusDecodeType; 0 = end-of-points      */
    uint8_t  flags;           /* MB_POINT_FLAG_WRITABLE                    */
    uint8_t  length;          /* ASCII register length; unused otherwise   */
    uint16_t offset;          /* relative to the txn's startAddr           */
    int8_t   scalePow10;      /* value = raw * 10^scalePow10               */
    uint8_t  unit;            /* DLMS/COSEM physical-unit code             */
    uint16_t publishThreshold, publishHeartbeatS;   /* deleted in v2, §2.4 */
    int16_t  writeMin, writeMax;  /* scaled-int domain                     */
    char     name[24];        /* MQTT topic suffix, NUL-terminated         */
} sModbusPointRecord;                                        /* 39 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  count;           /* register block length; 0 = end-of-txns.
                                 Compiler-derived, never authored.         */
    uint8_t  functionCode;    /* mbFc_holding(3) | mbFc_input(4)         */
    uint16_t startAddr;       /* wire register address                     */
    uint16_t readPeriodS;     /* caps at 18h12m — see §7                   */
} sModbusTransactionRecord;                                   /* 6 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;       /* 1-247; 0 = end-of-devices                 */
    char     topicPrefix[16];
} sModbusDeviceRecord;                                       /* 17 bytes */
```

#### v2 — the target layout (settled 2026-08-10; capability/plan split 2026-08-11, §2.6)

Not written yet. One format change, `MODBUS_LUT_VERSION` 1 → 2, carrying
everything §2 needs; the version field is what makes it safe, since a region
written by older firmware fails validation rather than being misread.

The stream gains **two sections ahead of the devices** — capabilities, then
plans — and **loses the transaction record entirely**, because read blocks are
derived rather than authored (§2.6):

```
[Header]
[Capability][Point]…[Point{decodeType=0}]     <- point sentinel
[Capability]…
[Capability{name[0]=0}]                       <- capability sentinel
[Plan][PlanEntry]…[PlanEntry{period_sec=0}]   <- entry sentinel
[Plan]…
[Plan{name[0]=0}]                             <- plan sentinel
[Device]…[Device{slaveAddr=0}]                <- device sentinel (end)
```

```c
typedef struct __attribute__((packed)) {
    char     name[16];        /* identity; name[0] == 0 = end-of-caps      */
    uint8_t  addrStride;      /* 1 = word-addressed, 2 = byte-addressed    */
    uint8_t  reserved;        /* future dialect bits (§7)                  */
} sModbusCapabilityRecord;                           /* 18 bytes — NEW */

typedef struct __attribute__((packed)) {
    uint8_t  decodeType;      /* eModbusDecodeType; 0 = end-of-points      */
    uint8_t  flags;           /* MB_PT_READ | MB_PT_WRITE  (access, §2.6)  */
    uint8_t  functionCode;    /* MOVED HERE from the txn record            */
    uint8_t  length;          /* ASCII register length; unused otherwise   */
    uint16_t addr;            /* ABSOLUTE wire address, not an offset      */
    int8_t   scalePow10;
    uint8_t  unit;
    int32_t  writeMin, writeMax;   /* scaled-int domain (§2.9a)            */
    char     name[24];
} sModbusPointRecord;                          /* 40 bytes (v1 was 39) */

typedef struct __attribute__((packed)) {
    char     name[16];        /* identity; name[0] == 0 = end-of-plans     */
    uint8_t  capOrd;          /* the capability its points resolve against */
    uint8_t  reserved;
} sModbusPlanRecord;                                 /* 18 bytes — NEW */

typedef struct __attribute__((packed)) {
    uint32_t period_sec;      /* 0 = end-of-entries; uint32 for 24 h+      */
    uint16_t ptOrd;           /* ordinal into the plan's capability        */
} sModbusPlanEntry;                                   /* 6 bytes — NEW */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;       /* 1-247; 0 = end-of-devices                 */
    uint8_t  baudCode;        /* rate-table index; 0 = 9600                */
    uint8_t  portId;          /* eModbusPortId — index into the port table */
    uint8_t  planOrd;         /* ordinal into the plan section             */
    char     topicPrefix[16];
} sModbusDeviceRecord;                             /* 20 bytes (was 17) */
```

**The plan is flat, not nested.** An entry is one (period, point) pair and the
distinct periods are found by scanning, rather than a period record owning a
count and a list. One sentinel rule instead of two, no count to keep consistent
with the entries that follow it, and 27 monitored points cost 162 bytes. An
**empty plan** — the sentinel entry immediately — is the legal way to say
"capable but never polled" (§2.6).

**`functionCode` moved onto the point** because there is no transaction record
left to hold it, and it was always a property of the register anyway: holding
and input are different address spaces. It also puts the `w`/`rw`-requires-
holding rule (§2.9a) next to the field it constrains.

**`offset` became an absolute `addr`.** Offsets existed to be relative to a
transaction's `startAddr`; with transactions derived, there is nothing to be
relative to. The stride multiply of §2.6 applies when the compiler computes a
point's wire address from the capability's own base convention.

**References are ordinals, never byte offsets.** That keeps two properties the
format was built for: the compiler stays single-pass with no backpatching (it
emits capabilities, counting them; then plans, resolving each `"capability"`
name against a ≤8-entry in-RAM table and each point name against the capability
it just emitted; then devices, resolving `"plan"` the same way), and nothing in
the stream is position-dependent. Opening the device section is open-plus-drain
of the two sections ahead of it — ≤8 each, cheap flash reads. If that ever
matters the header may carry section lengths; that is header metadata like
`streamLen`, not a pointer in the stream.

**Section order is forced by the single pass**: capabilities, then plans, then
devices. Each references only what precedes it, so a name is always resolvable
when it is met and nothing needs a second visit.

**The compiler derives read blocks at plan-emit time**, not at run time —
grouping each period's points by function code and ascending address under the
125-register ceiling and the declared-gap rule (§2.6). Blocks are not stored:
the engine re-derives them when it builds a device's sequences, from the same
records, by the same rule. Storing them would be caching a pure function of the
stream inside the stream.

**Migration is a wipe, and that is worth knowing before the update.**
`MbCfgStore_RegionValid()` gates on `hdr.version` by strict equality
(`modbus_config_store.c:123`), so on the first boot of v2 firmware both regions
are invalid, both are erased (§2.8), and the board comes up **unprovisioned** —
it does not fall back to anything. **The uploaded config is lost.** Download it
with `GET /api/modbus/config/download` *before* the update and re-upload after;
afterwards the JSON exists nowhere on the board.

**No v1→v2 translator will be written**, deliberately (§2.15 Q6). One-shot
migration code has to be written, tested against v1 streams nobody produces any
more, and then either carried forever or deliberately deleted — a poor trade
while the device is in development and its configs are a file in the operator's
hands. The consequence is stated loudly here precisely because the answer is
"re-upload", not "it is handled".

The move lands in the host tests first (§6): both `_Static_assert`s plus the new
one, compiler, exporter, store cursor and the hand-built stream vectors.

Each 16 KB region starts with a 256-byte header page,
`sModbusLutHeader{magic "MBCF", version, streamLen, crc32}` — **written last**
by the compiler, so a torn upload never yields a valid region.

Point `name` is `char[24]`, not `char[16]`: the Solis suffixes
(`overdischarge_soc_set`, 21 chars) do not fit 15 usable characters.

### 3.3 Compiler

One streaming pass, hand-rolled pull parser over a byte-source callback (no
heap, ~1.3 KB of static state in CCM, runs on the 4 KB HTTP task stack).
**The compile pass is the validation** — and `Modbus_ConfigVerify` (§2.10) is
that same pass with the record writes discarded, so there is never a second
validator to keep in sync.

The outcome it reports (`sModbusCompileResult`) is a **type**, so it lives in
`modbus_records.h` and consumers may name it; `MbCfgCompile()` is a **flash
accessor**, so it stays here and they may not call it (§2.15 Q2).

Derivation: register width per `decodeType`; transaction `count` =
`max(offset + width)` across points; `scalePow10` = log10 of the JSON `scale`
(rejected unless an exact power of ten); `unit` = DLMS table lookup.

Rejection covers: bounds (8 devices / 64 transactions / 192 points, soft caps
16 per device and 24 per transaction), `count` ≤ 125, unknown keys, unknown
units, non-power-of-ten scales, `writeMin`/`writeMax` without write access,
`length` outside ASCII, name and prefix lengths and charset, `slaveAddr`
1-247, `fc` ∈ {holding, input}, plan periods ≥ 1, and integer
overflow on write bounds. With §2.6 it also rejects a `baud` outside the rate
table, with §2.9a an unknown `access` value and `w`/`rw` on anything but a
1-register point of a `holding` transaction, and with §2.4 the `publish` object becomes an unknown key like any other
— which is the useful failure mode, since an old config is rejected by name
rather than silently losing a setting. Failures report the first offending
`{device, transaction, point, field, reason}` — that is what makes the HTTP 422
useful.

### 3.4 Flash layout

```
0x000F_9000  Modbus LUT A   16 KB  ─┐ roles, not fixed addresses: the selector
0x000F_D000  Modbus LUT B   16 KB  ─┘ says which is active; uploads compile
0x0010_1000  Selector        4 KB    into the inactive one; apply flips it
```

Selector follows the `boot_status.c` NOR pattern: CRC'd header
`{magic "MBSL", version, activeRegion, header_crc32}` plus a flags word
**outside** the CRC so `swap_pending` can be bit-cleared without an erase.
Recovery from a blank or corrupt selector prefers whichever region holds a valid
config.

**Invariant: a region is never erased while it may still be walked.** The
retiring region is only overwritten by the *next* upload, so a reader that races
a swap still sees coherent (old) data. Note the corollary that motivates §2.10:
the retiring region *is* the previous config, and any upload — successful or
not — destroys it.

### 3.5 Walker

One lap per 100 ms tick over the active region. Per lap: check for a pending
swap at the boundary and commit it (resetting all per-ordinal state); walk
devices → transactions → points; issue any transaction whose `readPeriodS`
elapsed; decode each point; apply the threshold/heartbeat policy; publish.
A pending write is drained *between* transactions so writes stay responsive.

Per-ordinal state is fixed-size, in CCM, and reset wholesale on swap:
`s_lastPollTick[64]`, `s_lastPublishTick[192]`, `s_lastValue[192]`,
`s_hasPublished` bitmap, `s_consecFails[8]`, `s_regBuf[125]` — ~2.1 KB
regardless of actual config size.

**None of this survives §2.7.** The 100 ms tick, the traversal and
`s_lastPollTick` are replaced by per-device timers that emit events; the
value-tracking arrays (`s_lastPublishTick`, `s_lastValue`, `s_hasPublished`,
~1.9 KB) are **deleted** rather than moved to a consumer, since none of them
needs one (§2.4). What survives into the engine is bus state: `s_consecFails[8]`
— now driving an event filter rather than a poll skip — plus one frame buffer
per port instead of the single shared `s_regBuf[125]`.

The word this document used for one traversal was "lap". It is retired: the
engine has no traversal to bound. The unit of work is a **sequence** (§2.7),
which is one device's transactions at one period.

Per-device availability: 3 consecutive failed transactions mark a device offline
(retained `<topicPrefix>/availability`); the first success brings it back. Offline
devices are throttled to ≥30 s so a dead slave cannot starve the bus. This stays
in the module (§2.15 Q5): the throttle is a bus decision, and only the module
owns the bus. It is deliberately **not** an MQTT LWT — LWT is a property of the
one TCP session for the whole bridge and cannot express "this slave stopped
answering while the bridge is fine". The suffix is `/availability` rather than
`/status` because the Solis device identity equals the bridge prefix, so
`/status` would collide with the bridge-wide LWT topic; it also matches HA's
`availability_topic` convention.

**Float never propagates.** A `float32_*` point is decoded as IEEE754 at the
wire and immediately quantized into the same scaled-integer domain as everything
else. Nothing downstream — the event, the formatting, any consumer comparison —
ever sees a float.

### 3.6 Units

`unit` is the `uint8` DLMS/COSEM physical-unit code from IEC 62056-6-2 — the
same `{scaler, unit}` shape as this design's `{scalePow10, unit}`. One table
serves both directions (string→code, code→(unit, HA device_class, state_class)),
which is what makes the export round trip exact.

**kWh has a private code (130).** COSEM has no distinct kWh code (kWh = Wh plus
a scaler), but `scalePow10` here places the decimal point rather than converting
units, and export/HA must round-trip kWh distinctly from Wh. Codes 73..252 are
reserved by the spec; 130 is documented in `modbus_units.h`.

### 3.7 Resource usage

Measure with `arm-none-eabi-size -A build/application.elf` — plain `size` sums
`.bss` + `.ccmram` + `.ccmheap` into one column and makes main SRAM look nearly
full when it is not.

| Resource | Usage |
|---|---|
| Flash | ~263 KB of 480 KB |
| Main SRAM | ~67 KB of 128 KB |
| CCM | ~58 KB of 64 KB — **~91 %**: 48 KB FreeRTOS heap (`.ccmheap`) + ~11.4 KB `.ccmram` |
| Ext flash | 2×16 KB LUT + 4 KB selector at 0xF9000-0x101FFF |
| USART2, PD5/PD6/PD7 | exclusive to the walker, except while a probe holds it or the port is disabled |

**CCM is the binding constraint — ~6 KB free.** Anything that adds a task stack
(from the CCM FreeRTOS heap) must be measured with `size -A` before and after.

**§2.4 returns ~1.9 KB of it.** Deleting `s_lastValue` + `s_lastPublishTick` +
`s_hasPublished` frees CCM outright rather than relocating it, since no consumer
takes the tracking over. It also removes the `MB_MAX_POINTS_TOTAL` → CCM
coupling entirely: raising the point ceiling used to cost ~8 B per point, and
now costs nothing but flash. Measure it anyway across step 6 of §2.16 — a
claimed saving that never shows up in `size -A` means something else grew.

The walker's remaining ~0.5 KB is in CCM only for historical reasons — none of
it is latency-critical, main SRAM has ~60 KB free, and `s_regBuf` **must** leave
CCM if DMA is ever pointed at it.

**What §2.3/§2.12a add, in full:** a 128 B subscription table (8 × 16 B), placed
in **main SRAM** rather than CCM for the same reason — nothing about it is
latency-critical. Observer event copies are not module memory at all, but they
do transit the 48 KB FreeRTOS heap in `.ccmheap`, so a misbehaving observer that
leaks its allocations exhausts the same heap task stacks come from. That is the
observer's bug to find; the instrument is `xPortGetFreeHeapSize()`, not
`modbus status`.

---

## 4. Config JSON reference

Three arrays, in dependency order: **capabilities** describe hardware,
**plans** describe what to watch, **devices** bind a plan to a slave.

```jsonc
{
  "capabilities": [
    {
      "name": "solis",
      "points": [
        { "addr": 3132, "fc": "input", "decodeType": "u16", "scale": 0.1,
          "unit": "V", "name": "battery_voltage" },
        { "addr": 3138, "fc": "input", "decodeType": "u16", "scale": 1,
          "unit": "%", "name": "battery_soc" },
        { "addr": 3009, "fc": "holding", "decodeType": "u16", "scale": 1,
          "unit": "%", "name": "max_charge_soc",
          "access": "rw", "writeMin": 70, "writeMax": 100 }
      ]
    }
  ],

  "plans": [
    {
      "name": "inverter_normal",
      "capability": "solis",
      "periods": [
        { "everySec": 5,  "points": ["battery_voltage", "battery_soc"] },
        { "everySec": 60, "points": ["max_charge_soc"] }
      ]
    }
  ],

  "devices": [
    { "slaveAddr": 1, "baud": 9600, "port": "rs485",
      "plan": "inverter_normal", "topicPrefix": "periphnet" }
  ]
}
```

- **Capability** — `name` (≤15 chars, unique), `addrStride` (optional, default 1;
  2 = byte-addressed registers as on the JK, §2.6), `points[]`. It describes
  what the hardware can do and contains **no periods** — nothing about how often
  anything is read.
- **Point** — `addr` (absolute wire register address), `fc`
  (`"holding"`/`"input"`), `decodeType` (`u16` `s16` `u32_be` `u32_le` `s32_be`
  `s32_le` `float32_be` `float32_le` `bitfield` `ascii`), `scale` (**exact power
  of ten**, 0.001…1000), `unit` (`""` `V` `A` `W` `VA` `var` `Hz` `Wh` `kWh`
  `varh` `VAh` `%` `Ah` `C` `min` `s`), `name` (≤23 chars, unique within the
  capability), `length` (registers, ASCII only), `access`, `writeMin`/`writeMax`.
  **No `offset` and no enclosing transaction** — read blocks are derived (§2.6).
- **`access`** — `"r"` (default, readable), `"w"` (writable), `"rw"` (both).
  It states what the **silicon supports**, not what this deployment does with
  it. `"w"`/`"rw"` require a 1-register type (an FC06 consequence) **and**
  `fc: "holding"` — FC06 reaches only the holding space, so a writable point on
  an input register is rejected at compile rather than writing a different
  register of the same number. A `"w"` point may not appear in a plan: it cannot
  be read, so watching it is incoherent.
- **Plan** — `name` (≤15 chars, unique), `capability` (the one its point names
  resolve against), `periods[]` of `{ everySec (≥1), points[] }`. A point may
  appear at only one period. **`periods` may be empty**, which is how a device
  that is only ever written through `Modbus_Request` and never polled is
  expressed (§2.6).
- **Device** — `slaveAddr` (1-247), `baud` (optional, default 9600; one of
  1200 / 2400 / 4800 / 9600 / 19200 / 38400 / 57600 / 115200 — see §2.6),
  `port` (optional, default `"rs485"`; one of the ports this firmware was built
  with — an unknown name is rejected at compile, §2.6), `plan`, and
  `topicPrefix` (≤15 chars of `[A-Za-z0-9_-]`; the MQTT namespace and the HA
  device identity — **not** the module's identity, which is the device's
  position in this array, §2.3/§2.14).

**Sharing is the normal case, not an option.** Four battery packs are four
devices, one capability, one plan:

```jsonc
{
  "capabilities": [ { "name": "jk_pb", "addrStride": 2, "points": [ /* … */ ] } ],
  "plans": [
    { "name": "pack_fast", "capability": "jk_pb",
      "periods": [ { "everySec": 5,  "points": ["soc", "pack_v", "pack_i"] },
                   { "everySec": 60, "points": ["cell_v_1", "cell_v_2"] } ] },
    { "name": "pack_lazy", "capability": "jk_pb",
      "periods": [ { "everySec": 300, "points": ["soc"] } ] }
  ],
  "devices": [
    { "slaveAddr": 1, "baud": 115200, "plan": "pack_fast", "topicPrefix": "bms1" },
    { "slaveAddr": 2, "baud": 115200, "plan": "pack_fast", "topicPrefix": "bms2" },
    { "slaveAddr": 3, "baud": 115200, "plan": "pack_lazy", "topicPrefix": "spare" }
  ]
}
```

The spare pack watches one register every 5 minutes off the **same** capability
— which the v1 schema could not express at all, because the period lived inside
the register map.

**There is no `publish` object** (§2.4). How often a value is worth forwarding
belongs to the consumer forwarding it; a config carrying `"publish"` is rejected
as an unknown key rather than silently ignored. Every monitored point is read at
its plan period and emitted every time.

**`writeMin`/`writeMax` are authored in the scaled-integer domain**, which is
the raw register domain — not in display units. This is the most common
authoring mistake. They are `int32_t` (§2.9a), so the full `u16` range and
32-bit setpoints are expressible; in v1 they were `int16_t` and anything above
32767 could not be bounded at all.

**Array order is identity, in two places.** A device's position in `devices[]`
is the bit a subscriber scopes to (§2.3); a point's position in its
capability's `points[]` is the `ptOrd` a requester addresses and events carry
(§2.9a). Appending to either is free; reordering or inserting reassigns
identities, exactly as renumbering an enum does. Plan and capability order is
*not* identity — they are resolved by name.

Bounds: ≤8 devices, ≤8 capabilities, ≤8 plans, ≤192 points total across
capabilities, ≤192 plan entries total, ≤64 derived read blocks, ≤125 registers
per block. **The caps count distinct records, not instances** — four packs on a
50-point capability cost 50 points, not 200. Instances govern only what a
catalogue burst and HA discovery produce (devices × their capability's points),
which costs flash reads and MQTT messages, not RAM.

ASCII and bitfield semantics, frozen: an ASCII point emits its decoded string on
every read (the old CRC32 dedup went with the publish policy); a bitfield emits
the raw `u16` formatted as unsigned decimal; neither gets an HA `device_class`
or unit. Formatting is the module's job — deciding whether to send it is not.

---

## 5. Operator reference

### 5.1 HTTP

```bash
# Validate only — no flash written, the previous config survives (§2.10)
curl -X POST --data-binary @my_config.json \
  http://10.42.0.203/api/modbus/config/verify

# Upload → compiles into the INACTIVE region; compile IS validation
curl -X POST --data-binary @my_config.json \
  http://10.42.0.203/api/modbus/config/upload
# on error: 422 {"error":"...","field":"scale","device":0,"transaction":1,"point":2}

curl -X POST http://10.42.0.203/api/modbus/config/apply     # hot swap, no reboot
curl      http://10.42.0.203/api/modbus/config/status
curl      http://10.42.0.203/api/modbus/config/download -o modbus_config.json
curl -X DELETE http://10.42.0.203/api/modbus/config          # erase -> unprovisioned
```

`verify` is planned with §2.10; everything else ships today. Uploads are refused
with 409 while an apply is pending. The uploaded JSON is not retained — download
regenerates it from the records (field order and whitespace may differ;
recompiling a download yields a byte-identical config).

### 5.2 CLI

| Command | Effect |
|---|---|
| `modbus read` / `modbus status` | engine + config status (**`unprovisioned`** when no valid config, §2.8), per-device failure and missed-event counters |
| `modbus probe <slave> <fc> <addr> <count> <baud>` | one-shot raw read, independent of the config — the generic replacement for `jk probe` (§1.3, §2.9). Runs as a normal transaction; the engine keeps going |
| `modbus monitor <on\|off>` | raw TX/RX frame dump via Trice |

That is the whole surface, and `probe` is the only command that touches the bus.
Removed: `modbus write` (no raw write path — §2.9a), `modbus start` /
`modbus stop` (no such lifecycle, §2.8),
`modbus set baud <rate>` (a device parameter, §2.6), `modbus port …` (a device
lives on a port; that is config, §2.5), `modbus inject …` (the test port is the
peer now, §2.5), and the whole `jk` command tree — `jk probe [slave] [baud]`
becomes `modbus probe 1 3 5120 8 115200` (§1.3).

`mqtt publish now` is removed with `Modbus_ForceRefresh` (§2.15). Under
free-running timers there is no due-state to mark, and a consumer that wants a
value sooner is asking for a shorter period. Re-driving HA state is
`Modbus_RequestCatalogue()`, which the bridge already calls on broker connect;
values follow within one period, and after §2.4 every read publishes.

Planned with the API: `modbus dump on|off` (the Trice subscriber).

### 5.3 MQTT topics

| Topic | Content |
|---|---|
| `<topicPrefix>/<name>` | point value, plain text, retained, QoS 0 |
| `<topicPrefix>/<name>/set` | inbound writes for points with `w`/`rw` access |
| `<topicPrefix>/availability` | retained `online`/`offline` per device |
| `<bridgePrefix>/status` | retained bridge-wide LWT |

HA discovery: `homeassistant/sensor/<topicPrefix>/<name>/config` per **monitored**
point (`period_sec > 0` in the catalogue — an unmonitored point would produce an
entity nothing ever updates, §2.11), plus
`homeassistant/number/<topicPrefix>/<name>_set/config` for `w`/`rw` points
(`command_topic`, min/max from the write bounds, step from the scale). Entities
carry an `availability` array (bridge status AND device availability, mode
`all`). Discovery and subscriptions re-run automatically after a config swap.

### 5.4 Trice landmarks

```
Modbus: unprovisioned (no valid config)
Modbus: started                            <- at Modbus_Init, not on command
Modbus: device online: periphnet
Modbus config: staged 2 devices 13 txns 29 points
Modbus: config swapped, active region 1
Modbus: device offline: periphnet          <- 3 consecutive failures
Modbus: missed periphnet 5s (n=1)          <- sequence still running when due again
MQTT: set periphnet/max_charge_soc = 95 -> req 3 items
```

`Modbus: walker started, 9600 baud` becomes `Modbus: started`: there is no
walker, no single baud, and no command to trigger it.
`Modbus: stopped (polls=… errors=…)` disappears with the lifecycle. The missed
line is new and fires on **first** occurrence per timer, not every time — the
running count belongs in `modbus status`, not in the log (§2.7).

### 5.5 RS485 timing

**As built:** inter-frame gap 4 ms before TX; TC flag polled before releasing
DE; 5 ms silence ends the RX frame; 1000 ms response timeout. All hardcoded, all
sized for 9600, all in the engine.

**By §2.5/§2.6 all of it moves into the port**, which is the only layer that
knows the line, and is driven by parameters handed down with each frame:
`max(3.5 char times, 1.75 ms)` for the inter-frame gap, the same floor rule for
end-of-frame silence, character = 11 bits. The 9600 case of that formula is
4.01 ms, i.e. today's constant, which is the evidence the derivation is right.
The 1.75 ms floor above 19200 baud keeps every value ≥2 ms, so millisecond
granularity still suffices at 115200. The engine never runs a character timer
after this; it sends a frame and waits for a completion event.

---

## 6. Testing

### Host (`tests/`, NOR-faithful flash mock) — where most correctness lives

```bash
cmake -B tests/build -S tests && cmake --build tests/build -j8
ctest --test-dir tests/build --output-on-failure
```

Covers the store/selector/cursor, the compiler accept+reject matrix, the export
round trip, decode vectors and units. **The v2 record change lands here first**
(§2.16 step 6), before any firmware moves: LUT version bump, the capability and
plan sections and their ordinal resolution, `baud` / `port` / `addrStride`
accept+reject, **derived read blocks** (contiguity, the 125-register ceiling and
the declared-gap rule of §2.6), an empty plan, a plan monitoring a `w` point or
naming a point outside its capability, a device naming a nonexistent plan or an
unknown port rejected, `publish` now rejected as an unknown key,
`access` accept+reject (including `w`/`rw` on an `input` transaction and on a
multi-register type, both rejected — §2.9a), `int32` write bounds round-tripping
past ±32767, and the export round trip through both spellings. Four existing tests encode the old
layout and change with it: `tests/modbus_test_stream.h:146`,
`test_modbus_store.c:104`, and `test_modbus_compiler.c:68,280`, the last being a
`threshold > 65535` reject case that becomes an unknown-key reject case.

### Integration (`tests/integration/`, live board)

Host-side C++ harness driving a board over USB/UART/UDP. Cases live in
`tests/integration/src/core/ModbusTests.cpp` and `MqttTests.cpp`, which are the
source of truth for what is asserted. `modbus_hw_*` and `mqtt_hw_*` — anything
needing a real slave or a real broker — are registered as immediate-skips, so
the rest is CI-runnable with no bus and no broker.

**The suite is rebuilt once, at step 9 of §2.16, around the test port.** Today
it asserts on decoded Trice output and fabricates responses with `modbus
inject`; afterwards the harness *is* the peer on the other side of a port. That
is a different and better kind of test:

| Today | On the test port |
|---|---|
| feeds a response in below the port | answers a request the engine actually formed |
| cannot see what was transmitted, except by parsing `Modbus TX[N]:` Trice lines with `modbus monitor on` | sees address, function code, start, count and CRC directly, and asserts on them |
| synchronous by construction — the command handler decodes and publishes before returning | the harness controls when the reply lands, so timing is chosen rather than incidental |
| exercises decode and dispatch | exercises framing, timeouts, retries, the scheduler and the FSM as well |

So the request side becomes assertable for the first time, and timeout and
malformed-frame paths become reachable without hardware — the harness simply
does not answer, or answers badly.

**What leaves the Trice contract** with the mechanisms behind it: every
`Modbus inject: …` and `Walker inject: …` line, `Modbus port: …`, and
`Modbus: stopped (polls=… errors=…)`. What stays is the observable behaviour a
test still wants to see:

| Event | String |
|---|---|
| Monitor on/off | `Modbus monitor: on` / `Modbus monitor: off` |
| TX / RX frame (monitor) | `Modbus TX[N]: <hex>` / `Modbus RX[N]: <hex>` |
| Start | `Modbus: started` |
| Device up / down | `Modbus: device online: <id>` / `Modbus: device offline: <id>` |
| Missed sequence | `Modbus: missed <id> <period> (n=N)` |
| MQTT pub / sub (monitor) | `MQTT pub: <topic> = <value>` / `MQTT sub: …` |
| MQTT inject ack | `MQTT inject: <topic>` |
| Request submitted / rejected (MQTT side) | `MQTT: set <topic> = <value>` / `MQTT: set <topic> rejected` — emitted by the requester, since the module reports outcomes only to the callback that asked (§2.9a) |

MQTT monitor lines are emitted even with no broker connected, otherwise the
bridge is unobservable in broker-less CI.

**Steps 1–7 of §2.16 must keep every string above intact** — that is their
acceptance test, and the only addition is `MB …` lines from the Trice
subscriber. Steps 8–14 rewrite the suite once and are checked against the
rewritten one.

Three caveats worth stating rather than discovering:

- **Step 5 changes which task emits `MQTT pub:`, and therefore its ordering.**
  Today `do_publish` runs on the modbus task, so a publish line lands between
  that sequence's `Modbus RX[N]:` lines. Afterwards it runs on `mqttTask`
  (§2.12a), so it lands whenever that task is scheduled. Content, topics,
  payloads and retain flags are unchanged — but any assertion that depends on
  interleaving rather than on the lines themselves will break at step 5, and
  that is expected rather than a regression.
- **The §2.4 claim is conditional on the config.** "Deleting the publish policy
  changes nothing on the wire" rests on the shipped config authoring no
  thresholds (verified — zero `publish` blocks in the Solis config that was
  built in before step 6 deleted it)
  and on `threshold == 0` already meaning "publish every read"
  (`modbus_walker.c:156`). A config that *did* author thresholds would publish
  more often afterwards. Deliberate feature removal, not a regression.
- **A test port is a config binding, not a build flag.** The same image serves a
  bench board and a real one, which is the point — but it also means a config
  that binds a real device to the test port makes that device look alive rather
  than offline. `modbus status` and `/api/modbus/config/status` must show each
  device's port so that is visible at a glance.

---

## 7. Known limits

Facts about the implementation on the board today. Most are now **in scope** —
§2 says what replaces them — so the column says which. What is left over at the
bottom is the genuinely undesigned part.

| Limit | Where | Status |
|---|---|---|
| One physical bus, hardcoded `huart2` | `modbus_rtu.c` throughout; `modbus_walker.h:23`, applied at `modbus_walker.c:328` | §2.5 — ports become a serviced set |
| One baud for the whole bus | same | §2.6 — a device parameter |
| `mbPort_uart6` in the enum but refused (pins not confirmed in the schematic) | `modbus_rtu.c:289` | §2.5 — an unpopulated port is simply absent; the schematic question remains |
| Framing constants sized for 9600 — 4 ms gap, 5 ms silence | `modbus_rtu.c:122`, `:156` | §2.5/§2.6 — derived from baud, inside the port |
| Write address assumed `startAddr + offset`; JK registers are byte-addressed | `modbus_config_store.c:334` (live); `mqtt_bridge.c:377` computes the same expression but `ha_publish_entity` discards it (`(void)regAddr`) — dead, not a second bug | §2.6 — address stride on the type; the dead one goes when MQTT stops walking the config |
| A **writable point is never checked against its transaction's function code** — marking a point on an `input` transaction writable compiles clean, and the FC06 write then lands on a *holding* register of the same number, a different register in a different space | `modbus_config_compiler.c:605-609` validates width only; the shipped config is correct by authorship, not by validation | §2.9a — `w`/`rw` access requires a `holding` transaction, rejected at compile |
| `readPeriodS` is `uint16_t`, so periods cap at 18h12m — **a 24 h read cannot be expressed** | `modbus_records.h:89` | §3.2 — `uint32_t` on the v2 plan entry |
| One map per device, copied per device — no way to share a register map between identical slaves | `modbus_records.h` (no map record) | §2.6/§3.2 — shared capabilities referenced by ordinal |
| **The read period lives inside the register map**, so two devices sharing a map must share its periods — a spare pack cannot be polled lazily off the same map | `modbus_records.h` (`readPeriodS` on the txn record) | §2.6 — capability/plan split |
| The walker publishes straight to MQTT | `modbus_walker.c:183` | §2.13 — the whole point of the API |
| Publish policy lives in the Modbus config | `modbus_records.h:77-78` | §2.4 — deleted |
| RX is polled byte-at-a-time; a UART overrun is indistinguishable from "no byte", and the per-byte budget shrinks ~12× at 115200 | `modbus_rtu.c:151` | **open** — the port contract (§2.5) is where a fix would live, but nothing here proves 115200 works; that is an on-hardware fact still to acquire |
| Reads are FC03/FC04; writes are FC06 only, one register | `modbus_rtu.c`; `Modbus_WriteSingleRegister` | §2.9a — FC16 coalescing is an internal optimisation; slave tolerance is a capability field |
| Writable points must be exactly 1 register | `modbus_config_compiler.c:606` | §2.9a — an FC06 consequence; relaxing it needs FC16 first |
| Write bounds are `int16_t` | `modbus_records.h:79` | §2.9a/§3.2 — widened to `int32_t` in v2 |
| One pending write at a time | `modbus_walker.c:35-38` | §2.9a — a small FIFO of batch submissions |

Not designed, deliberately:
- **Further device dialects** beyond address stride — function-code choice, read
  caps, block models. The constraint §2.6 sets is that whatever they turn out to
  be, they must be expressible as data on a type.
- **MQTT publish rate control.** §2.4 deletes the per-point threshold/heartbeat,
  which nothing used, and leaves the bridge publishing every sample. If broker
  or recorder load ever makes that a problem, the answer is a policy on the MQTT
  side, configured on the MQTT side — not a field back in the Modbus config.
- **Sequence fairness and startup phasing.** Sequences due at the same moment are
  serviced first-come-first-served, and every device's timers start together at
  init, so periods stay phase-locked. Both are accepted; the missed counter
  (§2.7) is the instrument that says when they stop being acceptable.

JK PB-series integration is otherwise a config problem, not a firmware one
(§1.3). Its protocol dossier lives in `~/Projects/JK_BMS`
(`src/core/JkRegisters.h`, `FW/decompiled/protocol-rs485-modbus.md` — those were
read out of the BMS binaries and override the vendor PDF). Running a DeviceInfo
read against real hardware is still the cheapest next fact to acquire, and after
§2.16 step 12 it is a `modbus probe` invocation rather than firmware.

---

## 8. Provenance

Consolidated 2026-08-08 from ten documents, now deleted: two design specs
(single-device bridge, multi-device config), their two implementation plans, the
usage reference, a common-driver direction doc, a refactor kickoff, a
full-implementation plan, a JK BMS task doc, and both integration-test docs.

**Revised 2026-08-09/10** over four review rounds, which turned §2 from an API
extraction into a design for the module the API sits on. `App/Modbus/modbus.h`
was edited to match. What changed, and why:

1. *JK BMS is not a module component* — a device is a config, not a driver.
   `jk_bms` left the boundary, the `jk` CLI tree went with it, and its two real
   facts became a device-type field (byte addressing → address stride) and a
   device parameter (115200 → baud).
2. *Baud is a device property* — different slaves at different rates share one
   wire by time-multiplexing. `Modbus_SetBaud` left the API. Framing constants
   became baud-derived, which later moved them into the port entirely.
3. *The module must not store values* — it reads, translates, emits. Two
   corrections were needed here:
   - **Per-device subscriptions stayed.** The first pass removed them by reading
     the finding as a ban on filtering; it is a ban on *memory*. Device scope is
     an ordinal — later a mask of them (§2.3) — and stores nothing.
   - **The publish policy was deleted, not carried.** The second pass had the
     module transporting `publish.threshold`/`heartbeatS` to consumers so
     authoring would stay in one place. Wrong place: Modbus has no basis to
     judge what is worth forwarding. Checking what actually used them made the
     deletion free — the shipped config authors none, and `threshold == 0`
     already means "publish every read", so it is behaviour-neutral and frees
     ~1.9 KB of CCM instead of relocating it.
4. *Nothing outside steers the module* — `Start`/`Stop`/`IsRunning`,
   `SetPort`/`GetPort` and `InjectResponse` all deleted. A port is where a
   device lives, which is config; "stopped" says nothing that "no devices" or
   "port disabled" does not. §2.8.
5. *Peripherals are a serviced set, and the engine is agnostic to them* — the
   frame-level port contract of §2.5, with parameters travelling per
   transaction and completions arriving as events. Test hooks were replaced by
   a test port that is the peer rather than a hole beneath the port; the
   integration suite is rebuilt on it once.
6. *Scheduling is event-driven, not a poll loop* — §2.7. Per-device timers per
   distinct period emit events; the engine services them as sequences. Because
   the scheduler is independent of the servicer, drift has nowhere to come from
   and overrun is answered by dropping a stacked event and counting it. Two
   consequences that had to be made deliberate: failure backoff became an event
   filter in the engine, since a free-running timer cannot know a slave is
   dead; and the config swap became per-device timer teardown rather than a
   traversal boundary. The word "lap" is retired with the traversal.

**Revised 2026-08-10 (second pass), closing the record-format question.** The
review above left §3.2 asserting the format would move once while §2.16
scheduled two separate record edits five steps apart (the `publish` deletion at
step 6, the device model at step 11), with §2.15's record-layout question
forbidding the second from being specified early. Three decisions closed it:

7. *Cardinality is the model* — port 1:many, communication data 1:1, register
   map 1:many (split again on 2026-08-11 into capability 1:many plans, plan
   1:many devices — entry 18). Maps are therefore **shared records referenced by ordinal**, not
   expanded per device: the caps count records, so copying a 50-point map across
   four packs would need 200 points and be rejected for a config holding 50
   distinct ones. Flat expansion was proposed first and fails on exactly that.
8. *A port is an enum byte*, indexing the module's static port table, because a
   port carries no per-deployment parameters. Consequence carried forward: the
   test port's transport is therefore compile-time fixed, which constrains what
   §7 may answer.
9. *So the format moves once* — v2 carries the `publish` deletion, the map
   record, `portId`/`mapOrd`/`baudCode` and the period widening together
   at step 6; step 11 acts on fields already stored. §3.2 also stopped showing
   future structs under a "what exists today" heading, and now states the
   migration consequence: v2's first boot wipes any uploaded config.

**Revised 2026-08-11 (second pass), designing the write path** — §7's largest
undesigned cluster, brought forward because two of its five symptoms are
record-shaped and step 6 promises the format moves only once. It closed by
being re-posed:

13. *The question was the contract, not the function code* — FC06-vs-FC16,
    register counts and queue depth are all downstream of "what do a requester
    and the module agree on?". The answer is that the module publishes the
    accessible set with its data types and access, withholds addresses, offsets
    and function codes, and the requester submits **two parallel arrays** —
    point ordinals and scaled-integer values — for one device, non-blocking,
    with a completion callback and a deadline. The reply is **three** arrays:
    the ids, the values, and a per-item result. §2.9a.
14. *The config decides what an item means, so it is not a write call* —
    `Modbus_Write` became `Modbus_Request`. A point's access is `r`, `w` or
    `rw`, and the module reads, writes, or writes-then-reads-back accordingly.
    That is a stronger protection than any caller-side rule, because a caller
    cannot reach a read-only register by asking differently, and it collapses
    on-demand reads and setpoint writes into one call. It also settled the
    open item beside it: `w`/`rw` requires a `holding` transaction, so the
    silent wrong-space write is a compile error.
15. *Every item is attempted, and a deadline is what makes borrowed memory
    safe* — stop-at-first-failure was proposed and rejected: `results[]` already
    reports per item, so truncating the batch destroys information the caller
    asked for and can act on. In exchange the request carries `timeout_ms`, and
    **the completion callback is guaranteed to fire within it** — which is the
    only reason a rule as strict as "the module owns your three arrays until the
    callback" is livable. A timed-out request is abandoned, never written into
    afterwards.

    Four things fell out rather than being decided: `mbEvt_writeResult` is
    deleted (an outcome belongs to its requester, not to every subscriber),
    `writeMin`/`writeMax` widen to `int32_t` to match the value domain,
    `MbCfg_FindWritablePoint()` disappears because the catalogue already is the
    accessible set, and FC16 becomes an internal coalescing optimisation whose
    only config surface is a dialect field on the map.

    It also forced contract 6: `ptOrd` is an id a requester names, so it cannot
    expire every config generation. Both ordinals are now authored identities,
    stable while array order is.

16. *A wrong default is worse than none* — Q6 asked how many built-in configs to
    ship and was answered with zero. A register map describes hardware the board
    may not have, so any built-in is a guess about the deployment that reports
    itself as configured while polling nothing. Invalid regions are erased at
    init, *unprovisioned* becomes a first-class reported state, and ~3.25 KB of
    `.rodata` comes back. A v1→v2 migration was refused alongside it: one-shot
    code, tested against streams nobody will produce again, on a device still in
    development whose configs are files in the operator's hands.

17. *Two calls deleted on review* — the clarifications above left both
    stranded. `Modbus_SubmitRawWrite` lost its result path when
    `mbEvt_writeResult` went, and reviewing it exposed the deeper problem:
    it is a hole through the access model §2.9a is built on, and unlike
    `Modbus_Probe` its blast radius is a device that gets a setpoint nobody
    authorised. `Modbus_ForceRefresh` was defined as "mark every transaction
    due", which is vocabulary the free-running scheduler of §2.7 does not have;
    a consumer wanting a value sooner is asking for a shorter period.
    `mqtt publish now` goes with it. The Commands group is now two calls,
    `Modbus_Request` and `Modbus_Probe`.

18. *Capability and plan are different facts* — reviewing the access/polling
    ambiguity exposed that the register map carried `readPeriodS`, i.e. a
    deployment decision inside a hardware description. Two devices sharing a map
    were forced to share its periods, so a spare pack could not be polled
    lazily. The config splits: a **capability** is a flat list of points with
    access, and a **plan** is periods over a capability's points; a device names
    a plan, and the plan names its capability. Consequences: authored
    transactions disappear (read blocks are derived, with a gap rule that never
    spans addresses the capability does not declare), `fc` moves onto the point,
    `access` stops meaning "what a request does" and means what the silicon
    supports — making write-only registers expressible for the first time —
    read-back becomes automatic on `rw` rather than an access mode, an empty
    plan is how "capable but unmonitored" is said, and the catalogue gains
    `period_sec` so a consumer can tell capable from monitored. Same category
    error, and same fix, as `publish.*` in §2.4 and `topicPrefix` in §2.14.

**Revised 2026-08-11, closing Q2, Q3 and Q4** — the open decisions are being
retired one at a time before any code moves. §2.15 is now numbered to the
header's `OPEN Qn` markers and ordered by the §2.16 step that needs each answer,
so "the next open decision" is a fact rather than a reading; the dispatch
question (Q4) was restored, having existed in `modbus.h` and not here. Two of
the three closed by rejecting the question as posed:

10. *A subscription covers devices, plural* — Q3 was posed as a table size and
    answered as a scope model. Bit N of a `uint8_t` mask is the device at
    position N of `devices[]`, so a fusion path over four packs holds one
    subscription instead of four, the retained device name disappears, and 8
    entries of 16 B is ample. The array position *is* the id — an authored
    `"id"` field was proposed and rejected as a second name for the same thing
    — which makes `devOrd` the device's identity rather than a per-generation
    ordinal, and demotes `topicPrefix` to what it always was: MQTT's string.
11. *The observer owns everything after the call* — Q4 was posed as synchronous
    dispatch vs a module queue and answered by removing the module from the
    question. Delivery is the call; the module keeps no queue, no buffer and no
    delivery-success notion. An observer that does work allocates in its
    callback, copies, posts to its own queue and returns. No contract was added
    beyond the non-blocking rule that already existed, so the cheapest possible
    observer stays cheap.
12. *A type is not an accessor* — the mirror-vs-re-export choice was a false
    one. `sMbCompileResult` is data and `MbCfgCompile()` writes flash; §1.2
    already separates those, and they were only ever in one header. The type
    moves to `modbus_records.h` as `sModbusCompileResult` (with `sMbCfgCounts`
    as `sModbusConfigCounts`), giving one struct, no copy and no translation
    layer. What decided it was v2: a failure inside `maps[]` has no device
    index, so the error location must gain a field — twice, under a mirror.

Rejected along the way, and worth not re-proposing: mirroring the compile result
in `modbus.h`, an authored `"id"` device field, a module-side event queue,
per-subscription delivery counters, and a static per-observer slot ring (all
above); expanding maps per device
(above); byte-offset references instead of ordinals, which would force the
compiler to backpatch and give up its single pass; carrying publish policy
through the API (round 3); keeping `modbus start|stop` as CLI sugar so the test suite
would not need rewriting — a command that exists only to satisfy a test is the
surface this refactor removes; and a byte-level port contract, which would have
left RTU silence detection with nowhere to live.

**Completed implementation plans were purged, not merged** — phase lists,
sequencing notes and as-built deviation logs for shipped work describe history,
not the system. Their still-binding decisions are stated where they apply
(point `name[24]` in §3.2, the `/availability` topic in §3.5, unit code 130 in
§3.6, the scaled-integer domain in §4).

**Speculative design was also removed**, on the same principle: several
sections designed a bus/profile layer, a schema v2 record layout, a write-path
rework and a six-phase roadmap while the API those all sit behind was still
undecided. What survives of it is §7 — the limits, as facts.

  Blockers — a decision is needed before the step it lands in
 
  1. The v2 compiler cannot resolve plan point names in one streaming pass, and ConfigVerify makes it worse (§3.2 vs §2.10 — steps 6, 7).
  To emit a sModbusPlanEntry{period, ptOrd} the compiler must map a point name to an ordinal in a capability that was already streamed out. §3.2 says "each point name against the capability
  it just emitted", but a plan may name any of the 8 capabilities, and plans come after all of them. Three consequences the doc doesn't reconcile:
  - Reading the names back from the region breaks Modbus_ConfigVerify outright — the counting sink writes nothing to read back (fw_write at modbus_config_compiler.c:350 is the only path).
  - Derived read blocks (grouping by fc + ascending address, the 125 ceiling, the declared-gap rule) need addr/fc/width for every selected point, i.e. the same random access.
  - §4's "name unique within the capability" has no implementation today — the compiler never compares point names (only strcmp on keys/enums), so this is a new all-names check with the
  same requirement.

  Concrete resolution to write down: build an in-RAM point index while emitting capabilities — {nameHash u32, addr u16, fc u8, width u8} × 192 ≈ 1.5 KB, plus plan entries retained as
  {period, ptOrd} ≈ 0.8 KB — and derive/validate blocks at plan close with an O(n²) min-scan (≤192 entries, upload-time only). That resolves name resolution, uniqueness and block derivation
  identically in compile and verify mode. But it roughly triples the compiler's ~1.3 KB static footprint, which currently sits in .ccmram with ~6 KB free — so the decision includes moving
  s_c/the index to main SRAM (~60 KB free). Nothing about it is latency-critical, same argument §2.15 Q3 used for the subscription table.

  2. The address model contradicts itself (§2.6 vs §3.2 vs §2.16 step 11 — step 6).
  §2.6 says stride is "one field on the capability and one multiply in the engine"; step 11 says "addrStride enters the address computation"; but §3.2's v2 point record stores an absolute 
  wire address and says "the stride multiply applies when the compiler computes a point's wire address from the capability's own base convention" — and there is no base field on
  sModbusCapabilityRecord for it to be relative to. Both cannot hold. It matters in three places:
  - If addr is already absolute-with-stride-baked-in, step 11 has nothing to multiply and addrStride is dead data.
  - Block derivation must run in the register-index domain, not the address domain: on the JK a 16-register block spans an address delta of 32, so contiguity, the 125 ceiling and the gap
  rule all mis-compute if they subtract raw addresses.
  - Decode indexing breaks. Today MbDecode_Scaled(pt, &regs[pt->offset]) (modbus_walker.c:152) uses offset as a buffer index; in v2 the index is (addr − blockBase) / addrStride and nothing
  says who computes it.

  Pick one: either points author {base, index} (capability carries the base) or addr stays absolute and the stride divide is what the engine does. Then say explicitly which domain
  derivation works in.

  3. The JK needs a second dialect field, and it has to land at step 6 — not step 12.
  jk_bms.c:27 records the BMS's real ceilings: quantity < 124 and quantity + wordOffset < 147. §7 lists "read caps" as deliberately undesigned, and step 12 lands the JK config — but §3.2
  promises the format moves only once. A derived block obeying only the 125-register ceiling is already one register over JK's limit, and a block starting deep in the DeviceInfo block
  violates the second rule. So the capability record needs the read cap now (its single reserved byte won't hold 147 — that's two uint16_t fields, or the format moves twice). This is the
  sequencing catch I'd fix first, since it's cheap now and expensive later.

  4. The test port has no transport, and step 9 rebuilds the entire integration suite on it.
  §2.5 defines the test port by what it replaces, §8 entry 8 notes its transport is "compile-time fixed", and §7 still lists mbPort_uart6 as pins not confirmed in the schematic
  (modbus_rtu.h:36 agrees). Undecided: which channel (spare USART + host adapter, USB CDC shared with the CLI, UDP), and if it shares a channel — the framing that separates test frames from
  CLI and Trice traffic, plus who arbitrates. Also unwritten is the port contract as C: §2.5 lists the record in prose, but doesn't assign ownership of the response timeout (port or
  engine), the shape of the per-transaction parameter block handed down with each frame, or what the rx callback reports for a short/CRC-bad frame vs. silence.

  5. No concurrency model for the API surface (steps 2, 4).
  Subscribe/Unsubscribe/RequestCatalogue/Request/ConfigApply are called from mqtt, http and cmd tasks while the modbus task dispatches synchronously. Contract 8 in modbus.h:519 admits the
  race ("the table is fixed and not lock-free; late subscription works but is not hot-plug-safe under load") rather than deciding it. Q4 closing on synchronous dispatch makes this sharper,
  not softer. Also unstated: catalogue replay "delivered after Modbus_Subscribe" must happen on the modbus task — dispatching inline in the caller's task would violate contract 1 and read
  flash from mqttTask. The clean answer is single-writer: every mutating call posts to the modbus task and returns; say so once and contract 8 disappears.

  6. Step 6's "behaviour unchanged" is harder than it reads.
  The old walker keys due-state by transaction ordinal (s_lastPollTick[MB_MAX_TXNS_TOTAL], modbus_walker.c:53) and transactions cease to exist at step 6. Derived blocks need a deterministic
  ordinal for that array, and the walker would have to derive blocks per 100 ms lap from flash, with a sort buffer §3.7 doesn't budget. Either state the keying rule and the buffer, or move
  block derivation to step 10 where sequences are built once per device.

  Cheap decisions still open

  - Modbus_Probe has no port (§2.9, confirmed in sModbusProbeReq). A device names its port, but a probe has no device — so on a two-bus board the second bus is unprobeable. Either add a
  port field (the one justified exception) or state "port 0 only".
  - Does a request-driven read emit mbEvt_sample? §2.2 says the module emits everything it reads; §2.9a routes outcomes to the requester only. Unresolved, and it decides whether HA state
  updates immediately on a set (via the rw read-back) and whether an unmonitored point publishes to a topic that has no discovery message.
  - Line format beyond baud. Everything runs 8N1 (Core/Src/usart.c:74-78), yet the gap derivation assumes an 11-bit character (8E1). Fine as a conservative floor, but say it — and if parity
  ever needs to be per-device it is a record field, so it's a step-6 question.
  - Bounds v2 doesn't state: points per capability, entries per plan period (both size the buffers in item 1), and a cap on Modbus_Request's count and timeout_ms (a 192-item batch is ~5 s
  of wire at 9600, at the head of a FIFO).
  - Offline throttle vs. requests: does an offline device's request still reach the wire, and is the retry window per device or per timer?
  - Requester-side memory: §2.13's MQTT set path needs per-pending-request storage for the borrowed arrays once the FIFO allows more than one — worth one line where the bridge is described.

  Doc consistency (mechanical)

  - v1 vocabulary survives in operator-visible places that step 6 necessarily changes: sModbusConfigStatus.transactions (modbus.h:445), the "transactions" status key and "transaction" in
  the 422 body (http_server.c:815, 822, 886, 897), the §5.1 example, §3.3's "transaction count = max(offset + width)", and the Modbus config: staged … %u txns … line (http_server.c:854,
  listed as a §5.4 landmark). §2.15 Q2 says the error location must grow but never defines the v2 shape — it needs a section discriminator plus {capIdx|planIdx|devIdx, ptIdx}.
  - Steps 1–7 acceptance says every Trice string in §6 is unchanged, but step 6 must change the staged-counts line — that exemption should be explicit.
  - The §2 staleness table (9 rows) misses two: sModbusConfigStatus.transactions, and contract 4's safe/unsafe list, which still names Modbus_SubmitWrite, SubmitRawWrite and ForceRefresh —
  all deleted.
  - Numbering: §2.16 has a "10a" between 10 and 11, and §2.15 lists Q1/Q5 ahead of the closed items despite claiming §2.16-step order.

# Modbus — the common module

**Status:** current behaviour + the design it is being rebuilt into ·
consolidated 2026-08-08 · **§2 rewritten 2026-08-10 after review**, and the
**v2 record format settled the same day** (§2.6 cardinalities, §3.2 layout) —
see [§8](#8-provenance)

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
| its own USART2 init at 115200 | the device's `baud` parameter ([§2.6](#26-devices-types-and-parameters)) |
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

**`App/Modbus/modbus.h` is the authority for the surface** — it is written, compiles clean, and
carries the contracts and open questions as comments. This section is the
rationale behind it, not a duplicate of it.

### 2.1 Shape

| Group | Calls |
|---|---|
| Lifecycle | `Modbus_Init` — that is all of it, [§2.8](#28-lifecycle--set-it-and-forget-it) |
| Subscriptions | `Modbus_Subscribe` · `Unsubscribe` · `RequestCatalogue` |
| Commands | `Modbus_SubmitWrite` · `SubmitRawWrite` · `Probe` · `ForceRefresh` |
| Configuration | `Modbus_ConfigVerify` · `Compile` · `Apply` · `Reset` · `Export` · `Status` |
| Diagnostics | `Modbus_Stats` · `LogStatus` · `SetMonitor`/`GetMonitor` |

Events: `MB_EVT_SAMPLE`, `MB_EVT_POINT_DESC`, `MB_EVT_DEVICE_STATE`,
`MB_EVT_WRITE_RESULT`, `MB_EVT_CONFIG`, `MB_EVT_TXN`.

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

### 2.3 Subscriptions are per device

```c
int Modbus_Subscribe(const char *deviceId, uint32_t eventMask,
                     fModbusSubscriber cb, void *ctx);   /* NULL = all */
```

A bus carries several devices with unrelated configs and unrelated consumers. A
CAN-fusion consumer wants one BMS and should not be handed an inverter's
registers on every sequence; a Trice dump wants everything.

Scoping by device is **routing, not suppression**, and none of §2.2's three
objections applies to it: `deviceId` resolves to a `devOrd` once per config
generation so dispatch compares integers rather than strings; it stores nothing
about values; and the answer is the same for every consumer of that device, so
there is no per-consumer policy to carry. What it buys is that each consumer's
scope is explicit at its registration site instead of buried in a filter inside
its callback.

Within that scope the subscription gets **every read** — that is where §2.2
binds.

Naming a device absent from the active config is legal and receives nothing. If
a later config adds it, delivery starts — consumers never re-register across a
config swap.

`eventMask` is the same kind of thing one level up: a stateless 6-bit predicate
on the event *type*, which lets a consumer say "descriptors and samples, never
transaction diagnostics" without a branch in its own callback.

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
- The MQTT bridge's `MB_EVT_SAMPLE` handler is `format → MqttBridge_Publish`,
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
`modbus inject` command and `MODBUS_PORT_DISABLED` all go. Where injection fed
a *response* in below the port and could never show what the engine
transmitted, the test port is the peer: the harness sees the request the engine
actually formed — address, function code, start, count, CRC — and answers it on
the same channel. That is strictly more coverage from strictly less firmware,
and it exercises the real framing, the real timeouts and the real state machine
instead of stepping around them.

Whether a test port is present is a **configuration** question, not a build
question: a device names the peripheral it lives on, so the same image serves a
bench board and a real one.

### 2.6 Devices, types and parameters

Three things, deliberately separated — and the **cardinalities are the design**,
not an implementation detail:

| | Cardinality | Holds | Example |
|---|---|---|---|
| **Port** | 1 : many devices | how frames get out and back | the RS485 UART; the test port |
| **Map** (device type) | 1 : many devices | the register map — transactions, points, decode, units, address model | "Solis inverter", "JK PB BMS" |
| **Device** | 1 : 1 | `deviceId`, slave address, baud, its port, its map | slave 1 @ 9600 on RS485, map Solis |

**Only the communication data is per-device.** The port and the map are both
shared, so four JK packs in parallel are four devices, one map, one port. That
is the whole reason the split exists, and it is why the map is *referenced* on
flash rather than copied into each device (§3.2): the compiler's caps count
records, so copying would spend the 192-point budget on duplicates — a 50-point
map across four packs would need 200 points and be rejected for a config with
50 distinct points in it.

The split is also what makes the engine port-agnostic. Moving a device to a
second bus because the first one is saturated is a change to one field. Nothing
about its register map, its identity, or any consumer's subscription moves with
it.

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

**Device types carry data, never behaviour.** The first real dialect is already
on the bench: JK register addresses are *byte*-offset from the block base, so
the wire address is `startAddr + offset × 2` where a standard slave wants
`startAddr + offset`. Registers are still 16-bit and counts are still in
registers — only the address stride differs. That is one field on the type and
one multiply in the engine.

The rule matters more than the field. If a type may hold a function pointer or a
per-type code path, then `jk_bms.c` walks back in through the type table, which
is exactly what deleting it was for. Any future dialect must first be expressed
as data — an address transform, a function-code choice, a read cap — or be
refused.

It also explains a live bug: at offset 0 the stride is invisible, which is why
JK reads from a block base work today, while writes compute `startAddr + offset`
(`modbus_config_store.c:334`) and are wrong at any non-zero offset.

### 2.7 Scheduling — timers, sequences, and slipping

There is **no polling loop**. Scheduling is an independent, event-driven
instance that emits events at the periods the config asks for; the engine
services them. Nothing walks the config looking for work.

**Timers belong to a device and a period.** A device's config has some set of
distinct read periods — 30 s, 5 min, 1 h, 24 h — and gets one timer per distinct
period, not one per transaction. Deduplicating by period is what keeps the count
negligible: the shipped Solis config has two (5 s and 60 s), a board with a JK
alongside it maybe four to six. Left un-deduplicated the worst case is 8 devices
× 16 transactions = 128 timers off the CCM heap, which is the difference between
free and noticeable. FreeRTOS software timers are cheap enough that there is no
reason to build something else; the only discipline is that the timer callback
runs in the timer service task, so like the ISR case it **only posts an event**.

**A sequence is what one timer fires:** the transactions of one device that
share one period, run back to back on that device's port. Two useful things fall
out. A device's transactions all share its baud, so a sequence costs **one line
reconfiguration**, not one per transaction. And a pending write has an obvious
injection point — between transactions *within* a sequence — which keeps writes
responsive without ever interleaving on a half-duplex wire.

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
the A/B regions, provisions the built-in default if nothing valid is present,
constructs the devices and starts their timers. From then on the module runs:
it owns its peripherals, reads what the config tells it to read, and services a
write at the first opening between reads.

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
                        sModbusCompileError *err);
```

Same signature as `Modbus_ConfigCompile`, same compiler, same single pass —
**the record writes go to a counting sink instead of flash.** There is no second
validator to keep in sync, which is the property that made "compile is
validation" worth having in the first place; verify just swaps the output.

### 2.11 The catalogue

A consumer needs the point *list* before any value arrives. HA discovery is the
forcing case: it publishes one discovery message per point at MQTT connect time
and cannot wait for samples — a 60 s point would take a minute, and a point on
an offline device would never appear.

So the module replays a **catalogue**: a burst of `MB_EVT_POINT_DESC`, one per
point in the subscription's scope, `last = 1` on the final entry. Delivered
after `Modbus_Subscribe`, after every config swap, and on demand via
`Modbus_RequestCatalogue` — which is what MQTT calls on broker connect, since
that moment is unrelated to when the config loaded.

The catalogue is not a violation of §2.2: it describes the *config*, which the
module owns and reads from flash on demand, not values, which it does not keep.

This replaced an earlier `Modbus_ConfigEnumerate(ops, ctx)` idea — a synchronous
walk the consumer would call itself. Catalogue-as-events wins on three counts:
one data path and one callback contract instead of two shapes for the same
information; every delivery comes from the modbus task, so flash records are
never read from a consumer's task alongside the engine; and "when do I re-read
the config?" stops being the consumer's problem, which today `mqtt_bridge.c`
answers by polling `MbCfgStore_ActiveBase()` on every loop.

Either way the config is walked from flash and never held in RAM — that property
is load-bearing and must not leak away through the API.

Set-topic resolution needs no enumeration: `Modbus_SubmitWrite` takes the device
and point by name.

### 2.12 Contracts

Eight of them, in the header, and they matter more than the struct shapes.
Summarised: callbacks run **in the modbus task, synchronously**; they **must not
block**; **every pointer in an event is borrowed** and dies when the callback
returns; a subscriber cannot fail a sequence; `ptOrd`/`devOrd` are stable only
within one config generation, while `deviceId` and point `name` survive.

Two of them are load-bearing under the event-driven engine:

- **Nothing dispatches from ISR or timer context.** Port completion callbacks
  and timer callbacks post events and return. Decode and subscriber dispatch
  happen in the modbus task, which is what keeps `MqttBridge_Publish` (and its
  `LOCK_TCPIP_CORE`) legal in a subscriber and Trice legal in a callback.
- **"Must not block" is stricter than it looks**, because samples arrive per
  read rather than per change: a subscriber's cost is multiplied by config size,
  not by how much the plant is moving.

### 2.13 What the consumers become

```c
Modbus_Subscribe(NULL, MB_EVT_ALL, trice_sink, NULL);

Modbus_Subscribe(NULL, MB_EVT_SAMPLE | MB_EVT_POINT_DESC |
                       MB_EVT_DEVICE_STATE | MB_EVT_CONFIG,
                 mqtt_modbus_cb, NULL);
```

- `MB_EVT_POINT_DESC` → one HA discovery message per point;
  `Modbus_RequestCatalogue()` on broker connect re-drives it.
- `MB_EVT_SAMPLE` → `MbFormat_Scaled` (or the bitfield/ASCII rule), then
  `MqttBridge_Publish`. No decision, no state.
- `MB_EVT_DEVICE_STATE` → the retained `<deviceId>/availability` topic.
- `MB_EVT_CONFIG` → nothing to do; the catalogue that follows carries the new
  point set.
- Inbound `<deviceId>/<name>/set` → `Modbus_SubmitWrite`, with the deferral out
  of `tcpip_thread` exactly as it is now.

`http_server.c` keeps its endpoints and calls only `Modbus_Config*`.
`cmd_parser.c` calls only `Modbus_*`, and loses its `jk` command tree.

### 2.14 Device identity

The module's device identity is the config field **`topicPrefix`** — a device
named after one consumer's transport. Harmless while MQTT was the only consumer
and welded in anyway; wrong as soon as a second consumer subscribes by name.

The API calls it **`deviceId`**, which for now simply *is* the `topicPrefix`
value. Whether the JSON eventually gains `"name"` as a preferred spelling is a
config-schema question, not an API one, and is deferred with everything else in
[§7](#7-known-limits). It is now the *only* transport-flavoured thing left in
the config: `publish.*`, the other one, was deleted rather than renamed (§2.4).

### 2.15 Open questions

1. **`deviceId` vs `topicPrefix`** in the JSON — deferred, see §2.14.
2. **`sModbusCompileError` mirror, or re-export `sMbCompileResult`?** The mirror
   keeps `modbus_config_compiler.h` private for ~30 lines of translation and one
   struct copy per upload. Weakest of the calls made here.
3. **`MB_MAX_SUBS = 8`** — ~16 bytes each.
4. **Who owns device availability?** Kept in the module: the retry throttle is a
   bus decision only the module can make, and two consumers deriving "offline"
   from `MB_EVT_TXN` independently would disagree with each other.
5. **How many built-in default configs?** Solis today; JK now wants one too
   (§1.3). Either `Modbus_ConfigReset(name)` with a named set (~1–2 KB `.rodata`
   each) or a repo file to upload (free, needs a network). Recommendation: the
   repo file, until a board ships with a JK attached by default.

**Closed by the 2026-08-09/10 review**, recorded so they are not re-opened: baud
accessors, port accessors and `Start`/`Stop`/`IsRunning` (all removed, §2.8);
`InjectResponse` (replaced by the test port, §2.5); writes while stopped and
catalogue-before-first-sequence (both moot — no stopped state); ASCII `value`
(unspecified, because nothing in the module computes a hash); scheduler drift
and overrun (structurally answered by a free-running scheduler plus
drop-and-count, §2.7).

**Record layout for the device/map split — closed 2026-08-10.** It was listed
here as premature; the cardinalities of §2.6 decide it. Maps are **shared
records referenced by ordinal**, not expanded per device, because the caps count
records and duplication would spend the point budget on copies. Ports are an
**enum byte**, not authored records, because a port carries no per-deployment
parameters. That makes the whole of v2 specifiable now, so the format moves
**once** — the point record's `publish` deletion (§2.4), the map record, the
device record's `portId`/`mapOrd`/`baudCode`, and `readPeriodS` widening to
`uint32_t` all land together. The layout is §3.2; the consequence for `ptOrd` is
below.

**`ptOrd` is map-relative, and identity is `{devOrd, ptOrd}`.** A shared map read
from four packs yields four distinct samples from one point record, so a point
ordinal alone no longer identifies a reading. Both fields are already in
`sModbusPointDesc`; what changes is the meaning of one comment in `modbus.h`, and
that no consumer may key on `ptOrd` by itself.

### 2.16 Implementation order

The engine rewrite and the API extraction are separable, and the API goes first
so that consumers stop moving while the insides change.

1. **`modbus.h` as a thin facade** over the existing `ModbusWalker_*`,
   `Modbus_*` (rtu) and `MbCfg*` calls. The header exists; the facade behind it
   is this step.
2. **Subscription table + dispatch**, with per-device resolution.
   `publish_point()` stops calling `MqttBridge_Publish` and raises an event for
   every decoded point.
3. **`modbus_trice_sink.c`** — the first subscriber, plus `modbus dump on|off`.
   *The visible milestone: readings in Trice with no MQTT in the picture.*
4. **Catalogue replay** — on subscribe, on config swap, on request.
5. **MQTT bridge becomes a subscriber** — format and publish every sample, HA
   discovery onto the catalogue, set-topic writes onto `Modbus_SubmitWrite`.
   `mqtt_bridge.c` drops every `Shared/Modbus` include and its
   `MbCfgStore_ActiveBase()` poll.
6. **The v2 record format, in one move** (§3.2). Everything the on-flash layout
   will ever need for §2 lands here, so it is never moved twice: the `publish`
   fields out of the point record and out of the JSON schema, the new map
   record and map section, `portId`/`mapOrd`/`baudCode` on the device record,
   `readPeriodS` widened to `uint32_t`, `MODBUS_LUT_VERSION` → 2. Compiler,
   exporter, store cursor and the host-test vectors move with it; the walker
   gains map resolution (a device's transactions now live in its map, not
   inline) and loses its tracking arrays. **The new fields are stored, exported
   and validated here but not yet acted on** — one baud, one port and stride 1
   still, so behaviour is unchanged. Separate from step 5 so a regression
   bisects cleanly, and after step 5 so the MQTT bridge is no longer walking
   the config when the stream shape changes.
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
11. **Act on the device model** — §2.6 becomes behaviour rather than stored
    fields: `baudCode` drives the line parameters handed down with each frame,
    `portId` selects the port from the module's table, `addrStride` enters the
    address computation, and several devices share one map. No record change
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
App/Modbus/modbus_default_config.c/h built-in Solis JSON + EnsureDefault()
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
    uint8_t  functionCode;    /* MB_FC_HOLDING(3) | MB_FC_INPUT(4)         */
    uint16_t startAddr;       /* wire register address                     */
    uint16_t readPeriodS;     /* caps at 18h12m — see §7                   */
} sModbusTransactionRecord;                                   /* 6 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;       /* 1-247; 0 = end-of-devices                 */
    char     topicPrefix[16];
} sModbusDeviceRecord;                                       /* 17 bytes */
```

#### v2 — the target layout (settled 2026-08-10, §2.15)

Not written yet. One format change, `MODBUS_LUT_VERSION` 1 → 2, carrying
everything §2 needs; the version field is what makes it safe, since a region
written by older firmware fails validation rather than being misread.

The stream gains a **map section ahead of the devices**, and devices reference a
map by ordinal:

```
[Header]
[Map][Txn][Point]…[Point{decodeType=0}][Txn{count=0}]
[Map]…
[Map{name[0]=0}]                              <- map sentinel
[Device]…[Device{slaveAddr=0}]                <- device sentinel (end)
```

```c
typedef struct __attribute__((packed)) {
    char     name[16];        /* map identity; name[0] == 0 = end-of-maps  */
    uint8_t  addrStride;      /* 1 = word-addressed, 2 = byte-addressed    */
    uint8_t  reserved;        /* future dialect bits (§7)                  */
} sModbusMapRecord;                                  /* 18 bytes — NEW */

/* point record: publishThreshold + publishHeartbeatS deleted   39 -> 35 */
/* txn record:   readPeriodS widened to uint32_t                 6 -> 8  */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;       /* 1-247; 0 = end-of-devices                 */
    uint8_t  baudCode;        /* rate-table index; 0 = 9600                */
    uint8_t  portId;          /* eModbusPortId — index into the port table */
    uint8_t  mapOrd;          /* ordinal into the map section              */
    char     topicPrefix[16];
} sModbusDeviceRecord;                             /* 20 bytes (was 17) */
```

**References are ordinals, never byte offsets.** That keeps two properties the
format was built for: the compiler stays single-pass with no backpatching (it
emits maps, counting them, then resolves each device's `"map"` name against a
≤8-entry in-RAM table), and nothing in the stream is position-dependent.
`MbCfg_OpenDevices()` is open-plus-drain of the map section — ≤8 maps, cheap
flash reads. If that ever matters, the header may carry a `mapSectionLen`; that
is header metadata like `streamLen`, not a pointer in the stream.

**Inline transactions survive.** A device that authors `transactions[]` directly
compiles to an anonymous map used by exactly one device — the compiler buffers
the 20-byte device record while it emits the map, then writes it. Every existing
config, including the built-in Solis default, compiles unchanged; sharing is
opt-in via a `maps` array (§4).

**Migration is a wipe, and that is worth knowing before the update.**
`MbCfgStore_RegionValid()` gates on `hdr.version` by strict equality
(`modbus_config_store.c:123`), so on the first boot of v2 firmware both regions
are invalid and `ModbusConfig_EnsureDefault()` compiles the built-in Solis
default over the active one. **An uploaded custom config is lost.** It is
recoverable only by `GET /api/modbus/config/download` *before* the update —
afterwards the JSON exists nowhere on the board.

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

Derivation: register width per `decodeType`; transaction `count` =
`max(offset + width)` across points; `scalePow10` = log10 of the JSON `scale`
(rejected unless an exact power of ten); `unit` = DLMS table lookup.

Rejection covers: bounds (8 devices / 64 transactions / 192 points, soft caps
16 per device and 24 per transaction), `count` ≤ 125, unknown keys, unknown
units, non-power-of-ten scales, `writeMin`/`writeMax` without `writable`,
`length` outside ASCII, name and prefix lengths and charset, `slaveAddr`
1-247, `functionCode` ∈ {holding, input}, `readPeriodS` ≥ 1, and `int16`
overflow on write bounds. With §2.6 it also rejects a `baud` outside the rate
table, and with §2.4 the `publish` object becomes an unknown key like any other
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
(retained `<deviceId>/availability`); the first success brings it back. Offline
devices are throttled to ≥30 s so a dead slave cannot starve the bus. This stays
in the module (§2.15 Q4): the throttle is a bus decision, and only the module
owns the bus. It is deliberately **not** an MQTT LWT — LWT is a property of the
one TCP session for the whole bridge and cannot express "this slave stopped
answering while the bridge is fine". The suffix is `/availability` rather than
`/status` because the default Solis device identity equals the bridge prefix, so
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

---

## 4. Config JSON reference

```jsonc
{
  "devices": [
    {
      "slaveAddr": 1,
      "baud": 9600,
      "topicPrefix": "periphnet",
      "transactions": [
        {
          "startAddr": 3132,
          "functionCode": "input",
          "readPeriodS": 5,
          "points": [
            { "offset": 0, "decodeType": "u16", "scale": 0.1,
              "unit": "V", "name": "battery_voltage" },
            { "offset": 6, "decodeType": "u16", "scale": 1,
              "unit": "%", "name": "battery_soc" },
            { "offset": 7, "decodeType": "u16", "scale": 1,
              "unit": "%", "name": "overdischarge_soc",
              "writable": true, "writeMin": 5, "writeMax": 40 }
          ]
        }
      ]
    }
  ]
}
```

- **Device** — `slaveAddr` (1-247), `baud` (optional, default 9600; one of
  1200 / 2400 / 4800 / 9600 / 19200 / 38400 / 57600 / 115200 — see §2.6),
  `port` (optional, default `"rs485"`; one of the ports this firmware was built
  with — an unknown name is rejected at compile, §2.6), `topicPrefix` (≤15 chars
  of `[A-Za-z0-9_-]`; the MQTT namespace, the HA device identity, and the
  module's `deviceId` — there is no separate `name`/`type` field), and **either**
  `transactions[]` inline **or** `map` naming a shared map.
- **Transaction** — `startAddr` (wire register address), `functionCode`
  (`"holding"`/`"input"`), `readPeriodS` (≥1), `points[]`. **No authored
  `count`** — the block length is derived.
- **Point** — `offset` (registers, relative to `startAddr`), `decodeType`
  (`u16` `s16` `u32_be` `u32_le` `s32_be` `s32_le` `float32_be` `float32_le`
  `bitfield` `ascii`), `scale` (**exact power of ten**, 0.001…1000), `unit`
  (`""` `V` `A` `W` `VA` `var` `Hz` `Wh` `kWh` `varh` `VAh` `%` `Ah` `C` `min`
  `s`), `name` (≤23 chars), `length` (registers, ASCII only), `writable`
  (1-register types only — an FC06 consequence), `writeMin`/`writeMax`.

**There is no `publish` object** (§2.4). How often a value is worth forwarding
belongs to the consumer forwarding it, not to the register; a config carrying
`"publish"` is rejected as an unknown key rather than silently ignored. Every
point is read at its transaction's `readPeriodS` and emitted every time — which
is exactly what the shipped Solis config already does, since it never authored a
threshold.

**`writeMin`/`writeMax` are authored in the scaled-integer domain**, which is
the raw register domain — not in display units. This is the most common
authoring mistake.

### Shared maps (v2)

Identical slaves share one register map instead of repeating it. Four battery
packs are four devices, one map:

```jsonc
{
  "maps": [
    { "name": "jk_pb", "addrStride": 2,
      "transactions": [ { "startAddr": 5120, "functionCode": "holding",
                          "readPeriodS": 5, "points": [ /* … */ ] } ] }
  ],
  "devices": [
    { "slaveAddr": 1, "baud": 115200, "port": "rs485",
      "map": "jk_pb", "topicPrefix": "bms1" },
    { "slaveAddr": 2, "baud": 115200, "port": "rs485",
      "map": "jk_pb", "topicPrefix": "bms2" }
  ]
}
```

- **Map** — `name` (≤15 chars, unique), `addrStride` (optional, default 1;
  2 = byte-addressed registers as on the JK, §2.6), `transactions[]`.
- A device gives **either** `map` **or** an inline `transactions[]`, never both.
  Inline is sugar for a private map used by that one device, so single-device
  configs are unchanged from v1.

Bounds: ≤8 devices, ≤8 maps, ≤64 transactions total (≤16/map), ≤192 points total
(≤24/transaction), ≤125 registers per transaction. **The transaction and point
caps count distinct records, not instances** — four packs on a 50-point map cost
50 points, not 200. Instances govern only how much a catalogue burst and HA
discovery produce (devices × their map's points), which costs flash reads and
MQTT messages, not RAM.

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
curl -X DELETE http://10.42.0.203/api/modbus/config          # built-in default
```

`verify` is planned with §2.10; everything else ships today. Uploads are refused
with 409 while an apply is pending. The uploaded JSON is not retained — download
regenerates it from the records (field order and whitespace may differ;
recompiling a download yields a byte-identical config).

### 5.2 CLI

| Command | Effect |
|---|---|
| `modbus read` / `modbus status` | engine + config status, per-device failure and missed-event counters |
| `modbus write <slave> <reg> <value>` | queue an FC06 write |
| `modbus probe <slave> <fc> <addr> <count> <baud>` | one-shot raw read, independent of the config — the generic replacement for `jk probe` (§1.3, §2.9). Runs as a normal transaction; the engine keeps going |
| `modbus monitor <on\|off>` | raw TX/RX frame dump via Trice |

That is the whole surface, and it is all read-only except `write` and `probe`.
Removed: `modbus start` / `modbus stop` (no such lifecycle, §2.8),
`modbus set baud <rate>` (a device parameter, §2.6), `modbus port …` (a device
lives on a port; that is config, §2.5), `modbus inject …` (the test port is the
peer now, §2.5), and the whole `jk` command tree — `jk probe [slave] [baud]`
becomes `modbus probe 1 3 5120 8 115200` (§1.3).

`mqtt publish now` belongs to the bridge but reaches in here: it marks every
transaction due so the next sequences re-read everything
(`Modbus_ForceRefresh`). After §2.4 that is the whole of it — a read always
produces a publish, so there is no second tracking set to clear.

Planned with the API: `modbus dump on|off` (the Trice subscriber).

### 5.3 MQTT topics

| Topic | Content |
|---|---|
| `<deviceId>/<name>` | point value, plain text, retained, QoS 0 |
| `<deviceId>/<name>/set` | inbound writes for `writable` points |
| `<deviceId>/availability` | retained `online`/`offline` per device |
| `<bridgePrefix>/status` | retained bridge-wide LWT |

HA discovery: `homeassistant/sensor/<deviceId>/<name>/config` per point, plus
`homeassistant/number/<deviceId>/<name>_set/config` for writable points
(`command_topic`, min/max from the write bounds, step from the scale). Entities
carry an `availability` array (bridge status AND device availability, mode
`all`). Discovery and subscriptions re-run automatically after a config swap.

### 5.4 Trice landmarks

```
Modbus: default config provisioned (11 txns 27 points)
Modbus: started                            <- at Modbus_Init, not on command
Modbus: device online: periphnet
Modbus config: staged 2 devices 13 txns 29 points
Modbus: config swapped, active region 1
Modbus: device offline: periphnet          <- 3 consecutive failures
Modbus: missed periphnet 5s (n=1)          <- sequence still running when due again
Modbus: write reg 3010 failed (-1)
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
(§2.16 step 6), before any firmware moves: LUT version bump, the map section and
map-ordinal resolution, `baud` / `port` / `addrStride` accept+reject, inline
`transactions[]` compiling to an anonymous map, a device naming a nonexistent
map or an unknown port rejected, `publish` now rejected as an unknown key, and
the export round trip through both spellings. Four existing tests encode the old
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
| Write queued / rejected | `Modbus write: reg N = V` / `Modbus write: reg N rejected` |

MQTT monitor lines are emitted even with no broker connected, otherwise the
bridge is unobservable in broker-less CI.

**Steps 1–7 of §2.16 must keep every string above intact** — that is their
acceptance test, and the only addition is `MB …` lines from the Trice
subscriber. Steps 8–14 rewrite the suite once and are checked against the
rewritten one.

Two caveats worth stating rather than discovering:

- **The §2.4 claim is conditional on the config.** "Deleting the publish policy
  changes nothing on the wire" rests on the shipped config authoring no
  thresholds (verified — zero `publish` blocks in `modbus_default_config.c`)
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
| `MODBUS_PORT_UART6` in the enum but refused (pins not confirmed in the schematic) | `modbus_rtu.c:289` | §2.5 — an unpopulated port is simply absent; the schematic question remains |
| Framing constants sized for 9600 — 4 ms gap, 5 ms silence | `modbus_rtu.c:122`, `:156` | §2.5/§2.6 — derived from baud, inside the port |
| Write address assumed `startAddr + offset`; JK registers are byte-addressed | `modbus_config_store.c:334`, duplicated at `mqtt_bridge.c:377` | §2.6 — address stride on the type; the duplicate goes when MQTT stops resolving addresses |
| `readPeriodS` is `uint16_t`, so periods cap at 18h12m — **a 24 h read cannot be expressed** | `modbus_records.h:89` | §3.2 — widened to `uint32_t` in v2 |
| One map per device, copied per device — no way to share a register map between identical slaves | `modbus_records.h` (no map record) | §2.6/§3.2 — maps become shared records referenced by ordinal |
| The walker publishes straight to MQTT | `modbus_walker.c:183` | §2.13 — the whole point of the API |
| Publish policy lives in the Modbus config | `modbus_records.h:77-78` | §2.4 — deleted |
| RX is polled byte-at-a-time; a UART overrun is indistinguishable from "no byte", and the per-byte budget shrinks ~12× at 115200 | `modbus_rtu.c:151` | **open** — the port contract (§2.5) is where a fix would live, but nothing here proves 115200 works; that is an on-hardware fact still to acquire |
| Reads are FC03/FC04; writes are FC06 only, one register | `modbus_rtu.c`; `Modbus_WriteSingleRegister` | **open** |
| Writable points must be exactly 1 register | `modbus_config_compiler.c:606` | **open** — an FC06 consequence |
| Write bounds are `int16_t` | `modbus_records.h:79` | **open** |
| One pending write at a time | `modbus_walker.c:35-38` | **open** — an event-driven engine makes a small queue natural, but §2 does not specify one |

Not designed, deliberately:

- **The write path.** FC06-only, one register, one pending, `int16_t` bounds and
  a word-addressed write address are one cluster, and the JK byte-addressed
  write model is the first thing that forces it open. §2.6's address stride
  fixes the *read* side of that; the write side needs its own conversation.
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
     the finding as a ban on filtering; it is a ban on *memory*. Device scope
     resolves to an ordinal once per config generation and stores nothing.
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
step 6, the device model at step 11), with §2.15 Q4 forbidding the second from
being specified early. Three decisions closed it:

7. *Cardinality is the model* — port 1:many, communication data 1:1, map
   1:many. Maps are therefore **shared records referenced by ordinal**, not
   expanded per device: the caps count records, so copying a 50-point map across
   four packs would need 200 points and be rejected for a config holding 50
   distinct ones. Flat expansion was proposed first and fails on exactly that.
8. *A port is an enum byte*, indexing the module's static port table, because a
   port carries no per-deployment parameters. Consequence carried forward: the
   test port's transport is therefore compile-time fixed, which constrains what
   §7 may answer.
9. *So the format moves once* — v2 carries the `publish` deletion, the map
   record, `portId`/`mapOrd`/`baudCode` and the `readPeriodS` widening together
   at step 6; step 11 acts on fields already stored. §3.2 also stopped showing
   future structs under a "what exists today" heading, and now states the
   migration consequence: v2's first boot wipes any uploaded config.

Rejected along the way, and worth not re-proposing: expanding maps per device
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

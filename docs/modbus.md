# Modbus — the common module

**Status:** current behaviour + the design it is being rebuilt into ·
consolidated 2026-08-08 · **§2 rewritten 2026-08-10 after review**, the **v2
record format settled the same day**, and every remaining open decision closed
one at a time over 2026-08-11/12 rather than during implementation.

**[§9](#9-open-decisions) is empty.** Closed in that run: Q2/Q3/Q4/Q6, the
config **split into capabilities and plans**, **ID linking**, the **address
model**, the **capability dialect group**, the **concurrency model**, the
**port contract**, the **deletion of direct bus access in either direction**,
**demand-driven polling**, **Q5 reversed — availability leaves the module**, the
**request item array**, the **numeric compile result** and **line format as a
device parameter**. Each is recorded with what it rejected in
[§2.15](#215-decisions-and-what-they-cost) and [§8](#8-provenance) entries
19-36. **The next action is [§2.16](#216-implementation-order) step 1.**

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
| [§2 **The design**](#2-the-design) | **now — this is what gets built** |
| [§3 What exists today](#3-what-exists-today) | implementing against it |
| [§4 Config JSON reference](#4-config-json-reference) | authoring a config |
| [§5 Operator reference](#5-operator-reference) | driving the board |
| [§6 Testing](#6-testing) | changing anything |
| [§7 Known limits](#7-known-limits) | planning what comes after the API |
| [§9 Open decisions](#9-open-decisions) | now empty — the audit closed them all |

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
   App/Cmd ────────►│  App/Modbus/modbus.h                     │
   App/Http ───────►│  (the only header a CONSUMER includes)   │
   App/Mqtt ───────►│                                          │
   App/Can  ───────►│  ┌────────────────────────────────────┐  │
        ▲           │  │ scheduler   timers → events        │  │
        │ events    │  │ engine      FSM, sequences, decode │  │
        └───────────┤  │ port table  slots + frame buffers  │  │
                    │  │ config      facade over Shared/    │  │
                    │  └────────────────────────────────────┘  │
                    └──────▲───────────────────┬───────────────┘
   RS485 driver ───────────┤ modbus_port.h     │ may depend on
   test driver  ───────────┘ register+complete ▼
                              (§2.5)    HAL · CMSIS-OS/FreeRTOS ·
                                        Shared/Modbus · w25q128 · trice
```

**Rules:**

- Files in `App/Modbus/` must not include `App/Mqtt`, `App/Http`, `App/Can`,
  `App/Data`, or any lwIP header.
- A **consumer** includes only `App/Modbus/modbus.h`. The scheduler, the engine
  and the framing layer are module-internal, and there is no way to select a
  port for a device from outside — a port is where a device lives, and that is
  config (§2.5, §2.6).
- A **peripheral driver** is the one other thing outside the module that talks
  to it, through `modbus_port.h`: it registers itself into a port slot and calls
  back when a frame completes (§2.5). It knows nothing about config, events or
  consumers, and the module knows nothing about its transport.
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
| a hardcoded FC03 read of block `0x1400` | points at `addr: 5120…` in a capability that declares that block ([§2.6](#26-capabilities-plans-devices-and-parameters)) |
| ASCII decode of model / hw / sw version | `decodeType: "ascii"` points — already supported |
| its own USART2 init at 115200 | the device's `baud` parameter (§2.6) |
| byte-addressed registers | the capability's `addrStride` — data, not code (§2.6) |
| no FC06 handler on the slave | the capability's `writeFc` — likewise (§2.6) |
| "is a JK actually there?" for bring-up | a capability declaring its blocks, plus `Modbus_Request` (§2.9) |

**The JK BMS becomes a capability, on the same footing as the Solis config.**
That is what makes the module worth extracting at all: the next device after JK
must cost a JSON file, not a `.c` file. The rule that keeps it that way —
**capabilities carry data, never behaviour** — is stated in §2.6, because a
capability that may hold code is just `jk_bms.c` with extra steps.

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

Nothing here is implemented. §2.1-§2.4 and §2.8-§2.14 are the module's
**surface** — what a consumer sees and may rely on. §2.5-§2.7 are what sits
**behind** it: peripherals, the device model, and scheduling. §2.15 is the
record of how each of those was decided. The split is the point: the surface is
what §2.16 lands first and holds constant while everything behind it is
replaced.

**`App/Modbus/modbus.h` predates the 2026-08-11/12 audit and is superseded
throughout.** It compiles and it carries the original contracts as comments, but
the audit changed the surface in more places than it left untouched, so it is
**not** a second source of truth and no attempt is made here to enumerate the
differences — that list would be longer than the header. Read §2 as
authoritative; §2.16 steps 1-6 rewrite the header to match, and step 2 is where
most of it lands.

Every call, event, contract and struct the header still shows in its old form is
covered by a §2.15 entry saying what replaced it and why.

The one thing the header carries that this document does not is its `OPEN Qn`
markers. All six questions are closed (§2.15); the markers are deleted by the
step that first acts on each answer.

### 2.1 Shape

| Group | Calls |
|---|---|
| Lifecycle | `Modbus_Init` — that is all of it, [§2.8](#28-lifecycle--set-it-and-forget-it) |
| Subscriptions | `Modbus_Subscribe` · `Unsubscribe` · `RequestCatalogue` |
| Commands | `Modbus_Request` ([§2.9a](#29a-requests--one-array-of-items-in-and-out)) — that is all of it, [§2.9](#29-no-direct-bus-access-in-either-direction) |
| Configuration | `Modbus_ConfigVerify` · `Compile` · `Apply` · `Erase` · `Export` · `Status` |
| Diagnostics | `Modbus_Stats` · `LogStatus` · `SetMonitor`/`GetMonitor` |

Events: `mbEvt_sample`, `mbEvt_pointDesc`, `mbEvt_config`, `mbEvt_txn`,
`mbEvt_released`. Two are gone for unrelated reasons. `mbEvt_writeResult` —
a write's outcome goes to the requester that asked for it, through the
completion callback it supplied (§2.9a), not to every subscriber — and
`mbEvt_deviceState` went with device availability itself, which is a consumer's
notion and not the module's (§2.15 Q5) — a subscriber that wants it counts
failed `mbEvt_txn` for the devices it cares about. `mbEvt_released` is the one
addition: the module telling one subscriber that its `Unsubscribe` has taken
effect and its `ctx` may be freed (§2.12b). It is the only event delivered
regardless of `eventMask`, because a consumer asked for teardown rather than
for that event.

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

**"Everything it reads" includes what a request read.** A `Modbus_Request` that
reads a register, or reads one back after writing it (§2.9a), raises
`mbEvt_sample` to every subscription scoped to that device, exactly as a
scheduled read does — and its transactions raise `mbEvt_txn`, because a
diagnostics subscriber should not have a blind spot shaped like "traffic
somebody asked for".

That is the exception **not** being made, and it is worth naming as such. The
tempting precedent does not apply: `mbEvt_writeResult` was deleted because an
*outcome* — did my write land — belongs to whoever asked for it (§2.1). A
register's value is not an outcome, it is a fact about the plant, and this rule
exists to say plant facts reach everyone scoped to that device. The requester
still gets its own copy in `item.value`, **correlated with the id it asked
about**, which is a different thing from a sample and is why both exist.

A consumer that does not want request-driven values filters them, which is
§2.4's division of labour and not the module's business. The MQTT bridge's
version of that rule is one line and comes free from §2.11 — *publish if I
created an entity for this point* — so a set's read-back on an unmonitored
setpoint updates its number entity, while a diagnostic read of a point nothing
listens to goes nowhere.

**This is a rule about memory, not about routing.** Delivering a device's
readings only to the consumers that asked for that device stores nothing and
costs nothing — see §2.3. The module does keep **scheduling and bus state**,
which is about the wire rather than the data, and which no consumer could own:
per-timer due state (§2.7), missed-event counters, and one frame buffer per
port. Consecutive-failure counts used to be on that list; §2.15 Q5 took device
availability out of the module altogether, so they are not.

### 2.3 Subscriptions scope by plan, and a subscription is what causes polling

```c
#define MB_PLAN_ALL  0xFFu

int Modbus_Subscribe(uint8_t planMask, uint32_t eventMask,
                     fModbusSubscriber cb, void *ctx);
```

Two things are true of a subscription, and the second is the one that makes the
module small.

**A consumer subscribes to a kind of thing, not to a position.** A plan names one
capability (§2.6), so every device on a plan is the same type of hardware read at
the same cadence — which is exactly the unit a consumer has an opinion about. A
BMS fusion path wants *pack data*; the operator later adds a third pack by
authoring a device that names the existing plan, and **no consumer changes**.
Under a device mask that consumer's `0x0F` would have silently become wrong.

Where a capability is split across plans — §4's `pack_fast` and `pack_lazy` over
one `jk_pb` — the split is itself the choice being offered. An observer that
wants the lazily-polled spare as well sets both bits; one that only cares about
the fast packs sets one. That is what separate plans are *for*, and keying the
subscription on capability would take the choice away.

`MB_MAX_PLANS` is 8, so the mask is a `uint8_t`. **Device count is no longer
pinned by it** — that constraint moved to plans, and the device cap is now a
flash-size question like the point ceiling (§3.3).

**A plan nobody subscribes to is not polled at all.** This is the second thing,
and it inverts the module's relationship to its own config: the config says what
*may* be read, and a subscription says what *is* read. The engine ORs the plan
masks of all live subscriptions; a device whose plan is outside that union gets
**no timers**, exactly as if its plan were empty (§2.6).

So there is no traffic nobody asked for — not as a goal the scheduler works
towards, but as a property of how work is created. What follows from it:

- **A consumer controls the bus by subscribing and unsubscribing.** The MQTT
  bridge can register when a broker connects and deregister when it drops, so a
  board with nothing listening puts nothing on the wire. A BMS fusion path
  subscribes at init and never leaves, because it always needs its packs.
- **Values lag a reconnect by up to one period.** The catalogue is config and
  replays immediately (§2.11); samples arrive when their timers next fire.
- **"Why is this device not being polled" becomes a question about
  subscribers**, so `modbus status` and `/api/modbus/config/status` report each
  device's polled state alongside its port (§6).
- **A `Modbus_Request` is unaffected.** It names a device directly and reaches
  the wire whether or not anything subscribes to that device's plan (§2.9a) — a
  request is somebody asking, which is the whole test.

Scoping is **routing, not suppression**, and none of §2.2's three objections
applies: dispatch is a bit test, it stores nothing about values, and the answer
is the same for every consumer of that plan. Within its scope a subscription
gets **every read** — that is where §2.2 binds.

`eventMask` is the same kind of thing one level up: a stateless predicate on the
event *type*, which lets a consumer say "descriptors and samples, never
transaction diagnostics" without a branch in its own callback.

**Events still identify the device**, by `devOrd` and by the `topicPrefix`
pointer the engine already holds in the record it is servicing (§2.14). Keying
the subscription on plans does not blur which pack a reading came from; it only
stops consumers from having to know config positions in order to ask.

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
  with **no memory** in it. The one test it does make — does an entity exist for
  this point (§2.13) — is a lookup in the map it already built from the
  catalogue, not a policy: it depends on the config, never on the value or on
  when the last one went out. That is the distinction this section is about.
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

The module owns a **set of peripherals** and services them. It knows nothing
about any of them: not pins, not UARTs, not sockets, not whether a frame leaves
the board at all. What it holds per port is a **function pointer, its own
buffers, and a callback it exports** — and that is the entire coupling.

```c
/* ---- what a peripheral driver provides ------------------------------- */
typedef struct {
    /* Send txLen bytes from tx, then receive one frame into rx.
     * Returns immediately; completion arrives via Modbus_PortDone().
     * Both buffers belong to the MODULE and stay valid until it fires.   */
    int  (*submit)(void *ctx, const uint8_t *tx, uint16_t txLen,
                   uint8_t *rx, uint16_t rxSize,
                   const sModbusPortParams *p);
    void *ctx;
} sModbusPortDriver;

typedef struct {
    uint32_t baud;
    uint32_t responseTimeout_ms;   /* the DRIVER arms it; 0 = its default */
    uint8_t  format;               /* eModbusLineFormat — 8N1 default     */
} sModbusPortParams;

typedef enum {
    mbPortDone_frame = 0,   /* a frame completed; len bytes are in rx      */
    mbPortDone_timeout,     /* responseTimeout_ms expired                  */
    mbPortDone_lineError,   /* overrun / framing / parity, or rx overflow  */
    mbPortDone_txFailed,    /* the frame never went out                    */
} eModbusPortDone;

/* ---- what the module provides ---------------------------------------- */
int  Modbus_PortRegister(uint8_t portId, const sModbusPortDriver *drv);
void Modbus_PortDone(uint8_t portId, eModbusPortDone how, uint16_t len);
```

One call down, one call up. Drivers include `modbus_port.h`; consumers include
`modbus.h` and never see either — the same §1.2 split that keeps
`MbCfgCompile()` out of a consumer's view.

Four properties make this work, and each removes something from the engine:

- **The unit is a frame, not a byte.** `submit` means *send what is in tx, wait
  out the line's own idea of a frame boundary, then tell me how it ended*.
  End-of-frame detection — the 3.5-character silence that defines an RTU frame —
  lives in the driver, which is the only thing that knows the line. The engine
  never runs a character timer.
- **The response timeout is the driver's too.** It is handed down per frame, and
  the driver arms it. The engine therefore waits for exactly one thing — a
  completion — and runs no deadline arithmetic at all, which is what keeps
  §2.7's "no absolute-deadline computations in the engine" true without an
  exception. This is a different timeout from `Modbus_Request`'s `timeout_ms`
  (§2.9a), which bounds a whole batch and stays with the engine.
- **Only `baud`, `format` and the timeout travel.** The inter-frame gap and the
  end-of-frame silence are *derived from baud inside the driver* (§2.6), not
  authored and not passed. Nothing about expected reply length travels either —
  that would make the contract protocol-level instead of frame-level.
- **Completion is asynchronous, and the callback only posts.** `Modbus_PortDone`
  runs in whatever context the driver completes in — an ISR for a DMA UART, a
  stack thread for a socket — and does nothing but post an event to the modbus
  task (§2.12b). No decode, no dispatch.

The last property is why several ports genuinely overlap: the engine is bounded
by the CPU it takes to form requests and decode replies, not by the wire speed of
any one line. A second RS485 bus buys real throughput rather than just address
space.

**The buffers are the module's, one tx and one rx per port.** That is the
opposite of an earlier draft, which had them owned by the driver and borrowed
upward — a loan with no stated end, and therefore a use-after-free waiting to be
written. Module-owned buffers delete the question: the engine passes pointers
down on `submit` and knows exactly when it is finished with them. A frame is at
most 256 bytes, so two ports cost about 1 KB, and it must be **main SRAM** — a
DMA driver writes into these, and CCM is CPU-only (§3.7).

**The driver reports how *reception* ended, never what the frame *means*.** A
short frame and a CRC-bad frame are both `mbPortDone_frame`; the engine parses
and rejects them, so one place decides what a valid reply is and a test peer
producing real CRCs exercises that path. What the driver does own is the
distinction the engine cannot make: silence is `timeout`, and a **line error is
its own outcome**, which is what closes §7's "a UART overrun is
indistinguishable from *no byte*".

**One completion callback, not two.** An earlier draft had a tx-result callback
beside the rx one. The engine never acts on "sent" — TX completion matters only
inside the driver, where it starts the response timer and drops DE — so a TX
failure is simply a fourth completion outcome and a transaction is exactly one
event.

**A port with no driver registered is disabled.** There is no `mbPort_disabled`
and no port-selection API; §2.8 argued that "no devices" and "port disabled"
already said everything `Stop` said, and registration makes the second of those
structural. `mbPort_uart6`
goes the same way — it was an enum entry with no pins behind it — so the enum is
`mbPort_rs485 = 0` and `mbPort_test = 1`, and a second real bus later is a third
slot with a third driver.

**The test port is a registered driver like any other, and it lives outside the
module.** Every module-side test hook goes: `Modbus_InjectResponse`,
`mbPort_disabled` and the port-selection call are all deleted, and nothing
replaces them inside `App/Modbus/`. What the harness talks to is an **App-layer
driver registered into slot 1**, which the module cannot distinguish from a UART.

Its transport is deliberately **not decided here**, because the module does not
need it decided. What the driver must do is deliver replies the harness supplies;
where those replies arrive from is an App-layer question. Today it is the
`modbus inject` CLI command; HTTP is the obvious next form and is noted in §5.2
as out of scope. Either way it is an App-layer change with no module change, no
§2.5 change and no config change, which is the payoff of the module knowing
nothing.

**This is the integration-test mechanism, not a stop-gap.** §2.9 keeps the
upward direction — replies fed in — and drops the downward one, so a test
peripheral that can only answer is not a reduced version of anything. Two
consequences worth stating rather than discovering:

- **Asserting on the request the engine formed is a bonus, not the point.** A
  harness that also wants to check what went out reads `Modbus TX[N]:` monitor
  lines, or a richer driver hands the request up. Neither is required: forming a
  correct frame downward is trusted and checked once against real hardware
  (§2.9).
- **RTU line framing is never covered by a test peripheral**, whatever its
  transport. The 3.5-character silence, inter-octet timing and DE turnaround live
  in the RS485 driver and stay hardware-only facts.

Whether a test port is present is a **configuration** question, not a build
question: a device names the port it lives on, so the same image serves a bench
board and a real one.

### 2.6 Capabilities, plans, devices and parameters

Four things, deliberately separated — and the **cardinalities are the design**,
not an implementation detail:

| | Cardinality | Holds | Example |
|---|---|---|---|
| **Port** | 1 : many devices | how frames get out and back | the RS485 UART; the test port |
| **Capability** | 1 : many plans | what the hardware *can do*: its **dialect** (how the engine must talk to it), its **blocks** (which address ranges exist), and a flat list of points — address, decode type, unit, scale, **access**, write bounds | "JK PB BMS", "Solis inverter" |
| **Plan** | 1 : many devices | what we *watch*: a `capId` and a list of time tables | "fast: SOC+pack every 5 s, cells every 60 s" |
| **Time table** | 1 : 1 plan | one period and the point ids read at it | "every 5 s: points 0, 1, 4" |
| **Device** | 1 : 1 | `deviceId`, slave address, baud, its port, its `planId`, `topicPrefix` | slave 1 @ 115200 on RS485, plan 0 |

**Everything links by id, and every id is a dense ordinal** — `capId`, `pointId`,
`planId`, `timeTableId`, `deviceId`, all `uint16_t`, all equal to the object's
position in its array. A plan names `capId 2`; a time table lists
`pointId 0, 1, 4`; a device names `planId 0`. **Names never link anything** —
`name` is a display property of an object exactly like `unit` or `scale`, and
`topicPrefix` is MQTT's string (§2.14).

The JSON authors each `id` explicitly even though it equals the position, and
the compiler **rejects a config whose ids do not run 0, 1, 2 …** in order. That
is the whole point of authoring them: an insertion or deletion that would
silently re-point every reference downstream becomes a compile error naming the
object where the run breaks. Position is still the identity — the record does
not store its own id, because a linear scan already knows it — but the author
has to say so, and saying so is checked.

Linking by id rather than by name is what keeps the compiler a **single pass**
over a stream it cannot rewind. Resolving `"battery_soc"` inside a plan would
mean looking up a point emitted in an earlier section, which needs either a
RAM index of every name or a read-back of what was just written — and read-back
is impossible in `Modbus_ConfigVerify` (§2.10), whose sink counts rather than
stores. An id needs neither: validating it is a comparison against a count the
compiler is already keeping.

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
`Modbus_Request` — is expressed by a plan with no time tables in it. An **empty
plan is legal**, and it is how "capable but unmonitored" is said.

**A time table is a period and the point ids read at it.** It exists as an
object rather than as a loose (period, point) pair because the engine already
treats it as one: a device gets one timer per time table (§2.7), a sequence is
one time table's points on one device, and the missed counter keys on
`{deviceId, timeTableId}`. Giving it an id names the thing the diagnostics
already count.

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
compile: a **time table may only list `r` or `rw` points** (a `w` point cannot be
read, so watching it is incoherent), and a **`w`/`rw` point must sit on a
`holding` function code**, since neither FC06 nor FC16 reaches the input space.

**Whether a writable point may span more than one register is a dialect
question**, which is why `writeFc` is on the capability. Under FC06 a write
lands exactly one register, so a `w`/`rw` point must be a 1-register type;
under FC16 it need not be, and the compiler enforces the rule the capability
asked for. This is not a hypothetical: the JK's Settings block is `u32` fields
at `0x1000`, `0x1004`, … — two registers each — so under an FC06-only rule not
one JK setpoint would be writable.

**Transactions are not authored any more.** A capability is a flat list of
points and a time table is a list of point ids; the **engine** derives the read
blocks when it builds a device's sequences — grouping a time table's points by
function code and ascending address, splitting at the capability's
`maxReadRegs` ceiling.

Derivation is the engine's and not the compiler's because the compiler cannot
see a point it has already streamed out (§2.10 forbids read-back), and because
nothing is gained by storing the result: blocks are a pure function of records
already in the stream, so caching them inside the stream would only create two
things that can disagree. §3.2 states the same rule from the format's side.

**Blocks are authored, and a read never crosses one.** A capability declares its
address ranges — `{base, regs}` pairs — and a derived read may span anything
*inside* one block, including addresses no point selects, but may never span two.
Bridging an unselected address costs two bytes per register (~2 ms at 9600)
against a whole extra round trip (~25 ms), so it is usually worth it; reaching
outside a block risks the slave rejecting the entire read with exception 2
(illegal data address), which is not worth anything.

Authoring the blocks rather than inferring them from where the points happen to
fall buys three things, and the third is why it was chosen:

- The **block length is the read-span limit** the slave actually enforces. The
  JK's ceiling is `quantity + wordOffset < 147` measured from a block base, which
  is precisely "a read must end before register 147 of its block" — so `regs`
  states it and no separate dialect field is needed.
- **Splitting is a boundary test**, not a search for gaps in a point list.
- **The compiler validates it.** Blocks precede points within the same
  capability, so a point outside every declared block, or past its block's
  `regs`, is a compile error pointing at the offending point. Inferred blocks
  could only ever have failed at run time, against a slave, as exception 2.

**A port is an enum, not an authored record.** `portId` on the device record
indexes the module's port table — one slot per peripheral, each holding the
module's frame buffers and whatever driver was registered into it (§2.5). Ports
therefore carry no per-deployment parameters, and a config naming a port this
firmware does not have is rejected at compile time rather than failing silently
at run time.

`eModbusPortId` lives in `modbus_records.h` — the compiler and exporter need the
name↔code table, and so does whoever calls `Modbus_PortRegister`. **Nothing
outside the module chooses a port for a device**, which stays config; but the App
layer does *populate* the table, which is a different act and the reason the enum
is not module-private.

**Baud is a device parameter, not a bus property.** It belongs to the
device↔peripheral binding, which is why one wire can serve a Solis at 9600 and a
JK at 115200: the master owns every transaction, so the line is time-multiplexed
by construction, and a slave that sees traffic at the wrong rate drops it on a
framing or address mismatch exactly as it drops traffic addressed to someone
else. Because parameters travel with the frame (§2.5), a rate change is just the
next transaction's parameters.

**Line format is a device parameter too, for the same reason.** A slave's
parity is a fact about that slave, not about the wire, and 8E1 is RTU's *spec*
default — so a config that can say 9600 but not 8E1 supports the common variant
and refuses the conformant one. `format` costs nothing to carry: it takes the
byte `sModbusDeviceRecord` already had spare (§3.2).

Framing gaps derive from baud rather than being authored — 3.5 character times
at 11 bits per character, with the RTU floors of 1.750 ms / 750 µs above 19200
baud. **11 bits is the maximum any RTU format uses**, so the gaps do not depend
on `format` at all (§5.5); it configures the UART and nothing else. At 9600 that is 4.01 ms, which is exactly the constant hardcoded today
(`modbus_rtu.c:122`), and the floors keep every value ≥2 ms so millisecond
scheduling still suffices at 115200.

**The dialect is data on the capability, and it is the whole of what the engine
knows about a slave.** The engine has no idea whether it is talking to a Solis,
a JK or something not yet on the bench; it runs one code path whose constants
come from the capability the device's plan points at. That is the guarantee the
"no behaviour in a capability" rule buys — if a capability could hold a function
pointer or select a per-capability code path, `jk_bms.c` walks straight back in
through the capability table, which is exactly what deleting it was for.

| Field | Solis | JK PB | What the engine does with it |
|---|---|---|---|
| `addrStride` | 1 | 2 | address units per register — see below |
| `writeFc` | 6 | 16 | function code for the write half of `Modbus_Request` |
| `maxReadRegs` | 125 | 123 | per-request quantity cap; splits a derived block |
| `blocks[]` | `{3000, 200}` | `{0x1000,147} {0x1200,147} {0x1400,147}` | which addresses exist; a read never crosses one |

Anything that turns out to be a fourth dialect fact must arrive the same way —
as data on this record — or be refused.

**`addrStride` is a divisor, not a multiplier.** JK registers are *byte*-addressed:
consecutive registers sit at `0x1400`, `0x1402`, `0x1404`, where a standard slave
puts them at `3000`, `3001`, `3002`. Registers are still 16-bit and the wire's
`quantity` field is still a register count — only the address units differ. So a
point's `addr` is authored **exactly as the vendor's register table gives it**
and stored verbatim, and `addrStride` converts between the address domain and the
register domain in the three places where the two are not the same thing:

| | |
|---|---|
| Contiguity | `next.addr == cur.addr + cur.width × addrStride` |
| Frame quantity | `(blockEnd − blockStart) / addrStride` registers |
| Decode index into the reply | `(pt.addr − blockStart) / addrStride` |

All three are the engine's, which is what step 12 of §2.16 lands. Nothing is
baked in at compile time, so the stored address is always the one that goes on
the wire — and a **write needs no stride arithmetic at all**, because the FC06 /
FC16 address is the authored `addr` itself. That retires the live bug where a
write computes `startAddr + offset` (`modbus_config_store.c:334`) and is wrong at
any non-zero offset: there is no offset left to add.

Getting the domains backwards is the failure worth naming, because it is silent:
a 16-register JK block spans an address delta of 32, so a contiguity test, a
quantity, or a 125-register ceiling computed on raw addresses is wrong by exactly
`addrStride` and still looks plausible.

### 2.7 Scheduling — timers, sequences, and slipping

There is **no polling loop**. Scheduling is an independent, event-driven
instance that emits events at the periods the config asks for; the engine
services them. Nothing walks the config looking for work.

**One timer per device per time table, and only while something is
subscribed.** Timers exist because a subscription asked for that plan (§2.3);
a device whose plan is outside the union of live subscription masks has none,
and creating or destroying them is what subscribing and unsubscribing does. A
device's plan holds some set of time tables — 30 s, 5 min, 1 h, 24 h — and the
device gets one timer for each, not one per derived read block. A device whose
plan is empty gets no timers at all (§2.6). The time table being an authored
object is what keeps the count negligible and the identity obvious: the shipped
Solis config has two (5 s and 60 s), a board with a JK alongside it maybe four to
six, and a timer is named by `{deviceId, timeTableId}` rather than by a period
value recovered by scanning. One timer per authored *transaction* — the shape v1
had — would have been 8 devices × 16 = 128 timers off the CCM heap, which is the
difference between free and noticeable. FreeRTOS software timers are cheap
enough that there is no reason to build something else; the only discipline is
that the timer callback runs in the timer service task, so like the ISR case it
**only posts an event**.

**A sequence is what one timer fires:** the derived read blocks of one device's
time table, run back to back on that device's port. Two useful things fall out.
Every block in a sequence goes to one device, so they all share its baud and
format and a sequence costs **one line reconfiguration**, not one per block. And
a pending request has an obvious injection point — between blocks *within* a
sequence — which keeps requests responsive without ever interleaving on a
half-duplex wire.

**A request batch runs whole, at one injection point.** §2.9a promises the batch
"belongs to one sequence on one port at one baud", and splitting it across
openings would make that untrue and complicate abandoning it on timeout. The
cost is stated rather than hidden: an 8-item batch is 8 round trips, ~0.5 s at
9600 baud, and that time comes out of the device whose sequence it interrupted —
so a large batch on a short period can show up in the missed counter. That is
the counter doing its job, not a fault.

**It is bounded, though.** `MB_REQ_MAX_ITEMS` caps a batch at 100 and
`timeout_ms` at 60 s (§2.9a), so the worst a single request can take from a port
is about 12 s at 9600 baud and far less at 115200 — and that is a ceiling on
every device sharing the port, not only on the one whose sequence was
interrupted, since a half-duplex line serialises them all.

**Timers free-run; service time never feeds back.** Because the scheduler is
independent of the servicer, nothing rearms a timer on completion, so a period
is a period and drift has nowhere to accumulate. There are no absolute-deadline
computations and no tick-wrap comparisons anywhere in the engine.

**Stacking is dropped and counted.** Event identity is `{deviceId,
timeTableId}` — which is what makes the time table an authored object rather
than a period found by scanning (§2.6). If the engine is handed a device's 5 s
event while its previous one is still unserviced, the new one is **dropped and
recorded**. That device's 60 s table is a different identity and is unaffected.
This is coalescing with an observable, and it keeps the queue small: timer
count plus a few slots for the API calls that post (§2.12b). A post that fails
because the queue is full is just another dropped event through the same
counter.

**The missed counter is a capacity signal, not a curiosity.** A non-zero count
means the config is asking for more than the wire can deliver — which is exactly
the fact that says "this line needs splitting", the reason devices are
peripheral-agnostic in the first place. Per-timer granularity says *which* read
is starving. Exposed through `Modbus_Stats` and the config status JSON, plus a
Trice line.

**There is no failure backoff, because there is no offline.** An earlier draft
kept the "3 consecutive failures ⇒ throttle to ≥30 s" rule and moved it into the
engine as an event filter. §2.15 Q5 removed the concept it rested on: the module
does not decide that a slave is dead, so it has no state to filter on. A
consumer that stops wanting a plan's data unsubscribes, and the timers go with
the subscription (§2.3) — which is a stronger remedy than a throttle, since it
takes the traffic to zero rather than to one attempt per 30 s.

The cost is stated rather than hidden: **a slave that stops answering while
something is still subscribed is retried at its plan's cadence indefinitely**,
spending one response timeout per attempt — about 20 % of a line at a 5 s period
with a 1 s timeout. Unsubscribing fixes it when a whole plan is dead, which is
the usual case (an unplugged bus). One dead device among healthy siblings on a
shared plan is a **config** change: move it to a plan with no time tables, which
§2.6 already defines as how "capable but unmonitored" is said, and apply it hot
(§2.10). The module offers no automatic remedy, deliberately — deciding a device
is not worth talking to is exactly the judgement §2.2 keeps out of it.

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
devices the config describes. Timers are **not** started here — they come and go
with subscriptions (§2.3), so a board that boots with a valid config and no
subscribers puts nothing on the wire. From then on the module runs: it services
whatever plans are subscribed to, and takes a request at the first opening
between reads.

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
having when "no devices configured", "nothing subscribed" and "no driver
registered" already say everything it said.

It reshaped one more call before deleting it outright: `Modbus_Probe` (§2.9).

### 2.9 No direct bus access, in either direction

`Modbus_Probe` is deleted, and with it the last way for anything outside the
module to put a frame on a wire or to feed one in. The invariant that buys is
short enough to hold in the head:

> **Every frame the module puts on a wire comes from the compiled config, and
> every reply it decodes came back from one.**

Earlier drafts had two exceptions, and they were the same exception facing
opposite ways. `Modbus_InjectResponse` fed a reply *upward* — driver side to app
side — without the config having asked for it. `Modbus_Probe` sent a request
*downward* — app side to a device — without the config having declared it. Both
existed for integration testing, and neither asked the config anything.

**Testing keeps the upward half and drops the downward one.** The harness
supplies replies through the **test peripheral** (§2.5), which is a registered
driver like any other, so the module cannot tell it from a UART and needs no
hook. What is being tested is everything *above* the port — decode, events,
scheduling, availability, the MQTT surface. That the module forms a correct
frame *downward* is trusted rather than continuously asserted, and is checked
against real hardware once, through the module itself, at the point the module
is first trusted.

**Nothing is lost that the rest of the design does not already do better.** What
`Modbus_Probe` was for was reading registers that are in no config, and §2.9a
already reaches every point of a device's capability whether or not any time
table watches it — with `sModbusReqReply.exc` carrying the exception code, which
was most of the diagnosis. A register worth asking about is a register worth
declaring, uploads are hot (§2.10) and a capability may declare far more than a
plan watches (§2.6), so "declare it, then request it" costs one upload and
exercises the real path instead of a parallel one.

What genuinely goes is **discovery under total uncertainty** — scanning for an
unknown slave address, or reading outside any declared block to find where a
block ends. That is a bench activity at a board you are standing next to, where a
USB-RS485 adapter and standard tooling beat any firmware affordance, and it needs
no code here. The JK is not even that case: its dossier already gives the block
bases and lengths, so its DeviceInfo read is an ordinary capability plus a
request.

The write side was closed the same way and for a stronger reason: `Modbus_Request`
decides what an item means from the config, and `Modbus_SubmitRawWrite` was
deleted because it decided the config did not apply (§2.9a). Reading could never
damage a battery, which is why the read hole survived three rounds of review —
but "harmless" was the whole of its defence, and that is not the same as needed.

### 2.9a Requests — one array of items, in and out

The write path was §7's largest undesigned cluster. What settled it was framing
it as **the contract between the module and a requester** rather than as a
function-code question: the config already describes every accessible register,
so the module publishes that set with its data types and access, keeps
addresses and function codes inside, and the requester submits a selection.

It is `Modbus_Request`, not `Modbus_Write`, because **the config decides what
each item means**, not the caller:

| Capability access | What a request does with it | `item.value` on return |
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
followed by a read of it, so `item.value` always reflects what the register
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
typedef struct {
    int32_t  value;    /* in: value to write · out: read or read-back      */
    uint16_t id;       /* in: pointId within the device's capability       */
    int16_t  result;   /* out: eModbusErr; mbErr_pending until decided     */
} sModbusReqItem;      /* exactly 8 bytes, no padding                      */

typedef void (*fModbusReqDone)(const sModbusReqReply *rep, void *ctx);

int Modbus_Request(uint8_t devOrd, sModbusReqItem *items, uint16_t count,
                   uint32_t timeout_ms, fModbusReqDone cb, void *ctx);
```

**One array of items, not parallel arrays.** An earlier draft passed `ids[]` and
`values[]` down and handed `ids[]`, `values[]` and `results[]` back. The item
struct is better on four counts, and the second is a defect the parallel form
had:

- **One borrow, not three.** The memory rule below governs a single object, so a
  caller cannot free two of the three and forget the last.
- **It removes a lie.** The old input was `const int32_t *values`, yet the module
  **writes** into it — reads and read-backs land there. `id` is in-only, `value`
  is in/out and `result` is out-only, and one `const` on one pointer cannot say
  three different things. Field-level direction can.
- **Index alignment stops being an invariant.** Three arrays must be the same
  length and stay aligned with nothing enforcing it; one array makes the
  mismatch unrepresentable.
- **The per-item state model becomes literal.** "Every item carries a state" is
  a *field* of the item rather than a notion spread across three allocations.

It costs nothing: `int32 + uint16 + int16` packs to exactly 8 bytes, so 100
items is 800 B either way.

**`item.id` is a `ptOrd`; `item.value` is in the scaled-integer domain** — the
same domain `mbEvt_sample.value` arrives in and `writeMin`/`writeMax` are
authored in. One device per call: `devOrd` is a parameter, the ids are points of
that device's capability, and the whole batch belongs to one sequence on one
port at one baud, so "the module finished" is a statement about one slave.

**A `ptOrd` is an identity, not a cursor** — the point's position in the config,
exactly as `devOrd` is the device's position in `devices[]` (§2.14). The module
owns the mapping and nothing internal leaks: a requester never sees a
a wire address, a stride or a function code. It carries the same price
as §2.3, and it is the same price an enum carries: **reordering points in the
JSON reassigns identity.**

**The same array back**, delivered to `cb`:

```c
typedef struct {
    sModbusReqItem *items;      /* the array as submitted, now filled in    */
    uint16_t        count;
    uint8_t         devOrd;
} sModbusReqReply;
```

*(Members are ordered largest-first so the struct carries no alignment padding —
the coding standard's rule, and worth following even on a struct this small
because it is the shape every requester copies.)*

**`item.result` holds one enum, not two.** *Timed out* and *not attempted* are
members of `eModbusErr` rather than a separate `MB_REQ_*` family — a caller
switching on a result should not have to know which of two namespaces a value
came from, and an `int16_t` carrying values from two enums is exactly the kind
of thing that survives review and then surprises someone.

**Every item carries a state, and the callback fires exactly when none is
`pending`.** That single invariant replaces three separate rules:

```
accepted        every item = mbErr_pending
in order        the item being serviced gets its definite result when its
                reply lands — mbErr_ok, or a reason
at the deadline the item in flight becomes mbErr_timedOut;
                the ones never started become mbErr_notAttempted
```

The module works one item at a time on one device, so its cursor already
distinguishes those last two and they cost nothing to keep apart — and they are
genuinely different facts for a **write**: an in-flight write may have landed on
the slave, a never-started one certainly did not. Normal completion, timeout and
the config-swap abandonment below are then the same rule seen three times, not
three rules.

**`eModbusErr` says *why*, including which exception.** A result that reports
"exception" without the code throws away most of the diagnosis — `modbus_rtu.h`
says so itself, arguing that telling illegal-function from illegal-address from
illegal-value apart "is the whole diagnosis when bringing up an unfamiliar
slave", and keeping the code in a separate `Modbus_LastException()` accessor
because a flat enum was the only alternative on offer. Per-item results make
that reasoning point the other way: one global "last exception" cannot say which
item got which. So the codes fold in — `mbErr_excIllegalFunction`,
`_excIllegalAddress`, `_excIllegalValue`, `_excDeviceFailure`, `_excOther` — and
the batch-level `exc` field is **deleted** — with the item array it would have
been the only thing in the reply that was not per item.

Config failures are per item too: an id that does not resolve in the device's
capability is `mbErr_idNotFound` on that item, and every other item still runs.
`mbErr_ok` stays 0, so `if (items[i].result)` remains the idiom.

**Nothing has to be argued about what comes back**, which is the quiet win. The
parallel form had to justify each returned array — the ids so a requester
juggling batches can tell them apart, the values because read-backs are half the
point, the results because a batch is per item. With one array the item *is* the
correlation, so all three are properties of the shape rather than decisions
defended in prose.

**The requester is not the only recipient of what a request reads.** Every read
and every read-back also raises `mbEvt_sample` to subscribers scoped to that
device, and the transactions raise `mbEvt_txn` — §2.2 admits no exception for
reads somebody asked for. What the reply adds is **correlation**: the value, the
id that requested it and the result that describes it are one object, which a
broadcast sample cannot express. A consumer that wants both is free to use both;
one that only wants the plant fact never calls `Modbus_Request` at all.

**Every item is attempted.** A failure on item 3 does not stop items 4-8: the
module works the whole list and records what happened to each. There is no
"how far it got" summary, because `item.result` already says it per item, and no
rollback, because FC06 lands one register at a time and the wire cannot offer
one. **Interpreting the mix is the caller's job** — it asked for eight things
and it is told eight outcomes.

**`timeout_ms` bounds the whole request, and the module must respect it.** When
it expires the module stops, marks the unfinished items *timed out* (and any
never started *not attempted*) and calls `cb`. That is what makes the
memory rule safe rather than merely stated:

> **The completion callback always fires, within `timeout_ms`, and it is the only
> moment at which the caller may free or reuse the item array.**

Until then the array belongs to the module — contract 3 pointing the other way,
and the same principle as §2.12a: the requester supplies the memory, the module
allocates nothing. One array rather than three is what makes that rule cheap to
obey. A timed-out request is *abandoned*, not merely reported: a response
arriving for it afterwards is discarded and never written into memory the caller
has been told it may reclaim. `cb` runs in the modbus task under the
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
It has to: "the callback always fires" is what lets a caller reclaim its array,
and a swap is not permitted to be the exception that strands it.

**In flight: a FIFO of 8 submissions**, drained in arrival order. Callers rarely
meet `mbErr_full`, which matters once several consumers can write — and it is why
the borrowing rule above is stated as loudly as it is, since the module may be
holding more than one requester's array at a time. Each entry carries its own
deadline, and at ~32 bytes the whole table is 256 B.

Depth 8 is a **fairness** bound, not a throughput one. Because the deadline runs
from submission, the worst wait for the last entry is depth × the maximum
timeout, and a request that spends its entire deadline queued completes with
every item *not attempted*. Eight keeps `mbErr_full` rare across the three
consumers that can write — MQTT, a fusion path, the CLI — without making that
tail meaningless.

#### Bounds, and where each one is checked

| | |
|---|---|
| `count` | **1…100** (`MB_REQ_MAX_ITEMS`) |
| `timeout_ms` | **1…60000**; `0` is rejected |
| FIFO | 8 entries |

**`MB_REQ_MAX_ITEMS` is its own constant, deliberately not tied to
`MB_MAX_POINTS_TOTAL`.** A request is a coherent group of things a caller wants
done together, not a bulk-transfer mechanism, and the two numbers move for
unrelated reasons — the point ceiling is a flash-size question (§3.3), while the
batch ceiling is about how long one caller may hold the bus and its own arrays.
**If a caller needs more than 100, it makes more requests**; they are
independent, each with its own callback and deadline, and the FIFO takes four of
them at once.

**There is deliberately no "no timeout" value.** `timeout_ms == 0` is rejected
rather than meaning "wait forever", because §2.9a's memory rule is livable only
while the callback is guaranteed to fire — an unbounded deadline is the same as
no guarantee. The 60 s ceiling is not derived from the worst batch, which at 100
items of `rw` is about 12 s at 9600 baud; it is where the request stops being a
request. **Anything that wants the bus for longer than a minute is asking to be
polled, and a plan is what polling is for** (§2.6). No lower bound is needed: a
caller passing 5 ms gets every item *not attempted*, which is well-defined and
its own mistake.

**Validation splits across two tasks, and the split falls out of §2.12b rather
than being invented here.** Checking `count` against *this* device's capability
would need a flash read, and flash reads belong to the modbus task — so a
synchronous return can only check constants:

| Where | Checks | Reports |
|---|---|---|
| **Submit** (caller's task) | null pointers, `count` 1…100, `timeout_ms` 1…60000, FIFO space | the return value — `mbErr_badArg` / `mbErr_full` |
| **Service** (modbus task) | each id against the device's actual capability | `mbErr_idNotFound` on that item; every other item still runs |

The second row is already how a config swap behaves (§2.9a completes outstanding
requests with "a config error on the ones whose ids no longer resolve"); this
makes it the general rule rather than a swap-time special case.

**Repeated ids in one batch are legal.** Items execute in order and each slot
gets its own result, so id 7 appearing twice is a write then a write, each with
its own read-back. That is well-defined, so there is no distinctness rule to
document and no O(n²) scan on the engine task to run.

Consequences worth naming:

- **`mbEvt_writeResult` is deleted.** An outcome belongs to whoever asked for
  it, not to every subscriber; the event enum drops to five types.
- **`writable: true` becomes `access`** in the JSON and two bits in the point
  record's existing `flags` byte (`MB_PT_READ`, `MB_PT_WRITE`), so it costs no
  record space. Record-shaped all the same, so it lands in v2 at step 6.
- **`writeMin`/`writeMax` must widen to `int32_t`**, matching the value domain.
  Also v2, or the format moves twice.
- **A point with `w` or `rw` access must sit on a `holding` function code**, and
  the compiler now rejects otherwise (§7's silent wrong-space write). Access
  modes decide what the module *does*; this rule keeps what it does coherent
  with where the point lives, since neither write function code reaches the
  input space.
- **The write function code is `writeFc` on the capability, not an
  optimisation.** An earlier draft of this section had FC16 as an internal
  coalescing choice the module could make freely. That is wrong, and the JK is
  the counter-example: its Modbus slave implements **only FC 0x03 and 0x10** and
  answers everything else — including FC06 — with exception 1, illegal function
  (`~/Projects/JK_BMS/FW/decompiled/protocol-rs485-modbus.md` §2.1, read out of
  the BMS binary). A module that "optimises" its way to FC16 would be the only
  thing that works on a JK, by accident. So the function code is declared, the
  engine uses what it is told, and coalescing contiguous registers into one FC16
  frame stays available as an optimisation *within* that declaration.
- **`Modbus_SubmitRawWrite` is deleted.** It was a hole straight through the
  protection this section is built on: the config decides what may be written,
  and a raw write decides it does not. Reading an unknown register cannot damage
  a battery and writing one can, which is why the read hole outlived this by
  three review rounds before §2.9 closed it too.
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
see the Q2 resolution in [§2.15](#215-decisions-and-what-they-cost).

### 2.11 The catalogue

A consumer needs the point *list* before any value arrives. HA discovery is the
forcing case: it publishes one discovery message per point at MQTT connect time
and cannot wait for samples — a 60 s point would take a minute, and a point on
a device that has stopped answering would never appear at all.

So the module replays a **catalogue**: a burst of `mbEvt_pointDesc`, one per
point of every device on a subscribed plan (§2.3), `last = 1` on the final
entry. Delivered after `Modbus_Subscribe`, after every config swap, and on
demand via `Modbus_RequestCatalogue`.

**The on-demand call stays, and demand-driven polling is what settles it.** It
was worth asking whether it had become redundant: the bridge now subscribes when
a broker connects, and `Subscribe` already replays, so its one named caller
appeared to be covered. The reasoning runs the other way. Since §2.3, subscribing
and unsubscribing **start and stop bus traffic** and unsubscribing costs a
`ctx`-release handshake (§2.12b) — so re-subscribing to force a replay would stop
polling in the gap, fire `mbEvt_released`, and use teardown as a query. A
read-only way to ask is worth more now than when Subscribe was cheap, not less.

Underneath that is the general point: a consumer's "I need the point list"
moment is not guaranteed to coincide with its "I want the data" moment. It does
for today's bridge, and that is a fact about today's bridge rather than a
property of the API. Leaving the list obtainable only as a side effect of
another call forces a future consumer either to restructure its lifecycle around
that coincidence or to cache what §2.2 says it should not. **Being able to ask
beats inferring**, and asking costs one post and a flash walk.

**The replay always runs on the modbus task**, never inline in the caller's
(§2.12b). `Subscribe` and `RequestCatalogue` post it; dispatching it inline
would violate contract 1 and would have `mqttTask` reading config records out of
flash. A subscriber that registers before `Modbus_Init` gets its catalogue when
a config first loads, which is why subscribing early is now safe rather than
merely tolerated.

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
ordinals, and deliberately no address or function code — so "the full
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

Eight of them, and they matter more than the struct shapes. Summarised:
callbacks run **in the modbus task, synchronously**; they **must not block**;
**every pointer in an event is borrowed** and dies when the callback returns; a
subscriber cannot fail a sequence; and **both ordinals are authored
identities** — `devOrd` is the device's position in `devices[]` (§2.14), `ptOrd`
the point's position in its capability (§2.9a), and each survives a config swap
for exactly as long as the author leaves that array order alone.

That last one replaced the original contract 6, which said ordinals were valid
only within one config generation. It stopped being true when scope and write
addressing both became ordinals: an id that expires every swap cannot be the
thing a requester names. The residual risk moved rather than vanishing — a
requester that caches ordinals across a **reordered** config writes or reads the
wrong entry, which is the reason `mbEvt_config` is followed by a fresh
catalogue. Under ID linking that reordering is itself a compile error (§2.6, §4),
so what remains is a config whose author deliberately renumbered it.

**Contract 8 became a guarantee rather than a caveat.** It used to say the table
"is fixed and not lock-free" and that late subscription "is not hot-plug-safe
under load" — an admitted race. §2.12b decides it: subscribe at any time,
including before `Modbus_Init`.

Four are load-bearing under the event-driven engine:

- **Nothing dispatches from ISR or timer context.** Port completion callbacks
  and timer callbacks post events and return. Decode and subscriber dispatch
  happen in the modbus task, which is what keeps `MqttBridge_Publish` (and its
  `LOCK_TCPIP_CORE`) legal in a subscriber and Trice legal in a callback.
- **"Must not block" is the only rule**, and it is stricter than it looks:
  samples arrive per read rather than per change, so a subscriber's cost is
  multiplied by config size, not by how much the plant is moving.
- **Borrowed pointers are what make the whole dispatch model work** (§2.12a) —
  an observer that keeps anything copies it before returning.
- **Borrowing runs both ways, and a borrow ends when the module says it ended.**
  A requester's `Modbus_Request` item array is the module's until the completion
  callback fires — freeing or reusing it earlier corrupts a frame still on the
  wire — and a subscriber's `ctx` is its own until `mbEvt_released` (§2.12b). One
  rule, two instances, no flag or handshake word in either: the module notifies,
  and until it has, the memory is on loan. The request's guarantee that its
  callback fires within `timeout_ms` (§2.9a) is what keeps that livable rather
  than open-ended.

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

#### 2.12b Concurrency — the API is a third event source, not a locked surface

Consumers live in the mqtt, http and cmd tasks; the engine lives in the modbus
task. Nothing in §2 said how those meet, and the header's contract 8 admitted a
race rather than deciding one.

**The mechanism already exists.** §2.7 has timer callbacks posting events and
§2.12 has port completions posting events, both into one queue the modbus task
drains. The API needs no concurrency design of its own — it becomes the **third**
source on that queue. Contract 1 stops being a rule the API works around and
becomes a consequence of how the API is called, and there is no mutex anywhere in
the module.

Not every call posts, though, and the three groups divide by *why*:

**Posted — the call mutates engine state:**

| Call | Why it cannot run on the caller's task |
|---|---|
| `RequestCatalogue` | reads config records from flash *and* fires callbacks; both belong to the modbus task, or contract 1 breaks and `mqttTask` walks flash |
| `Request` | §2.9a already specifies post-and-return with a completion callback — the submission FIFO **is** this queue |
| `Unsubscribe` | see the release protocol below |
| `ConfigApply` | §2.7 tears down per-device timers and bumps generation counters; only the owning task may |
| `ConfigErase` | erases a region that may be being walked — §3.4's invariant protects only the *inactive* one |

**Every module call is now non-blocking**, in every group. `Modbus_Probe` was
the one exception — it blocked its caller on a completion semaphore — and §2.9
deleted it, so "must not block" is a rule the module keeps as well as imposes.

**Runs on the caller's task — `ConfigCompile`, `ConfigVerify`, `ConfigExport`.**
These must *not* move. The byte source is the HTTP socket, §3.3 sizes the
compiler for the 4 KB HTTP stack, and an upload takes seconds — posting it would
stall the engine for the whole transfer. They need no locking because §3.4's A/B
invariant already provides it: a compile writes the region nobody is walking, and
the header is written last, so a torn upload never validates. `ConfigApply`
re-checks that the staged region is valid before committing, on the modbus task,
which is what makes a swap racing a half-finished upload a no-op instead of a
corruption.

**Synchronous and unserialized — `Stats`, `LogStatus`, `ConfigStatus`,
`Get`/`SetMonitor`.** Word-sized counters are atomic on Cortex-M4. A stats
snapshot may be internally inconsistent — `polls` read before `errors` updates —
and that is accepted, not overlooked: nothing acts on the relationship between
two counters.

**`Subscribe` does not post, deliberately.** A posted subscribe cannot return
"table full", which is a real init-time error a consumer needs. Instead a brief
critical section claims a slot, the entry is filled, and **`inUse` is written
last, behind a barrier**. The dispatcher therefore sees either a complete entry
or no entry — never half of one. That is at most 8 critical sections in the life
of the system, which is not a lock in any meaningful sense, and it buys a
synchronous error return. Only the catalogue replay that follows is posted.

That turns contract 8 from a warning into a guarantee. **Subscribe at any time,
including before `Modbus_Init`** — the table is static, so an early subscriber
simply fills a slot and its catalogue arrives when a config loads. The old
wording ("late subscription works but is not hot-plug-safe under load") described
a hazard that publish-last ordering removes.

**Unsubscribe: a borrow ends when the module says it ended.** Clearing `inUse`
from another task is unsafe for a reason no shared flag can fix — a dispatcher
that has already read the entry is committed to calling it, so a
check-then-call always leaves a window between the check and the call. Indirecting
the `ctx` through a pointer-to-pointer shortens that window; it does not close it,
because the load and the call are not atomic against the release.

So release is **notified**, exactly as §2.9a notifies the end of a request's
borrow:

1. `Modbus_Unsubscribe(handle)` posts and returns. Legal from inside a subscriber
   callback — a self-post is processed after the current dispatch finishes.
2. On the modbus task the module clears `inUse`. No dispatch can be in flight,
   because dispatch happens on that task.
3. The module makes one final call to the subscriber's own callback with
   **`mbEvt_released`**, delivered **regardless of `eventMask`** — the consumer
   asked for teardown, not for that event, so the mask does not gate it.
4. **That call is the release point.** After it returns the module will never
   call again and the slot is reusable. A consumer holding a heap `ctx` frees it
   there; §2.12a already establishes that allocating and freeing on the modbus
   task is legal and bounded.

A consumer with a static `ctx` — the Trice sink, the CAN fusion path — ignores
the event and pays nothing.

The payoff is that the module ends up with **one memory rule, stated twice**
rather than two mechanisms: *a borrow ends when the module tells you it ended.*
It covers the request arrays (§2.9a) and the subscriber `ctx` identically.

**Three rules fall out of the queue and are load-bearing:**

- **An API post must never block.** Contract 4 permits `Request` and
  `RequestCatalogue` from inside a subscriber callback, which is a self-post from
  the very task draining the queue — a blocking post on a full queue would
  deadlock outright. A full queue returns `mbErr_full` immediately.
- **A submission carries the config generation it was made against.** That is
  what makes §2.9a's "a config swap completes outstanding requests"
  implementable: submissions already ahead of the swap event run normally, and
  ones stamped with the retiring generation are completed with *not attempted*
  when the swap is processed.
- **Queue depth is timers plus a few API slots.** §2.7 sizes it from the timer
  count; the API calls that post are the rest of it. Depth still stays trivial,
  and a post that fails because the queue is full is reported through the same
  dropped-event counter.

One cost, stated rather than buried: **a catalogue burst now runs on the modbus
task by construction**, and §2.12a already calls it the heaviest thing the
dispatcher does. With v2's larger point ceiling a burst can delay a sequence
enough to show in the missed counter (§2.7). That is the instrument doing its
job, and it is the price of the catalogue never being read from a consumer's
task.

### 2.13 What the consumers become

```c
Modbus_Subscribe(MB_PLAN_ALL, mbEvt_all, trice_sink, NULL);

Modbus_Subscribe(MB_PLAN_ALL, mbEvt_sample | mbEvt_pointDesc |
                              mbEvt_txn | mbEvt_config,
                 mqtt_modbus_cb, NULL);     /* on broker connect */

Modbus_Subscribe(0x03, mbEvt_sample | mbEvt_txn,
                 bms_fusion_cb, NULL);      /* pack_fast + pack_lazy */
```

- `mbEvt_pointDesc` → one HA discovery message per point;
  the `Subscribe` on broker connect replays it, and `Modbus_RequestCatalogue()`
  re-drives it any time the bridge needs the list again without disturbing its
  subscription (§2.11).
- `mbEvt_sample` → copy into an allocation, post to `mqttTask`, and format +
  `MqttBridge_Publish` there (§2.12a) if an entity exists for that point. No
  state, and the one test is a catalogue lookup rather than a judgement about
  the value.
- `mbEvt_txn` → the bridge's own per-device failure count, and from it the
  retained `<topicPrefix>/availability` topic. The module has no opinion on
  whether a device is alive (§2.15 Q5); HA availability is MQTT's semantic and
  is now computed where it is published.
- `mbEvt_config` → nothing to do; the catalogue that follows carries the new
  point set.
- Inbound `<topicPrefix>/<name>/set` → resolve the name to `{devOrd, ptOrd}`
  against the map built from the catalogue, then `Modbus_Request` with `count = 1`
  (§2.9a), keeping the deferral out of `tcpip_thread` exactly as it is now. The
  batch API subsumes the single-point case; there is no second entry point for
  it. **The bridge does not publish the read-back itself** — it arrives as an
  ordinary `mbEvt_sample` (§2.2) and goes out through the handler above, so
  there is one publish path rather than two and no dedupe question. The
  completion callback is used only for the success/failure line, which means the
  bridge must keep its item array alive until it fires but never has to read it.
  At `count = 1` that array is one `sModbusReqItem` — 8 bytes of the API's own
  type, with no bridge-specific struct around it. Whether the bridge allocates
  one per pending set or reserves a small fixed set is its own call at step 5;
  either fits, and nothing in the module depends on which.

**The bridge publishes a sample if it created an entity for that point**, and
that single rule covers the awkward cases. §2.11 gives it sensors for
`period_sec > 0` and number entities for writable points regardless — so a set's
read-back on an unmonitored setpoint updates its number entity, while a
diagnostic read of a point nothing listens to is dropped rather than published to
a topic with no discovery message behind it. Deciding that is consumer judgement,
which is where §2.4 puts it.

`http_server.c` keeps its endpoints and calls only `Modbus_Config*`.
`cmd_parser.c` calls only `Modbus_*`, and loses its `jk` command tree.

### 2.14 Device identity

**The module's device identity is the device's position in `devices[]`** —
`devOrd` in an event, and the `deviceId` a requester names (§2.9a). It is not a
string, and the module retains no string per subscription.

It is deliberately **not** what a subscription scopes to. Subscriptions key on
plans (§2.3), because a consumer has an opinion about a kind of hardware at a
cadence rather than about a position in an array — so identity and scope answer
different questions and neither borrows the other's shape.

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

### 2.15 Decisions, and what they cost

**Nothing here is open**, and neither is [§9](#9-open-decisions). This section
is the record of what was decided and, more usefully, *what was rejected on the
way* — several proposals came back across review rounds, so the reasoning is
kept to stop them coming back a third time.

It is also the reference for reading `App/Modbus/modbus.h`, which predates the
audit: anything the header still shows in its old form has an entry here saying
what replaced it. The `Qn` numbering matches the header's `OPEN Qn` markers,
each deleted by the §2.16 step that first acts on the answer.

Entries are grouped by the round that closed them rather than by step, since
none is waiting on anything.

- **Q1 — should `topicPrefix` get a transport-neutral spelling in the JSON?**
  Deferred, see §2.14, and **downgraded to cosmetic** by the Q3 answer: identity
  is now the device's position in `devices[]`, nothing subscribes by name and
  `topicPrefix` is honestly named for the one consumer that renders it. (The
  question was originally "`deviceId` vs `topicPrefix`"; there is no `deviceId`
  in the JSON, and in the API it is now an ordinal.) Decided by step 6, where
  the schema moves anyway.
- **Q5 — Who owns device availability? Reversed and closed 2026-08-12: the
  consumer does.** The earlier answer gave it to the module, on the grounds that
  the retry throttle is a bus decision only the module can make and that two
  consumers deriving "offline" independently would disagree — while recording
  that "the strict reading of §2.2 would push it out". Demand-driven polling
  (§2.3) removed the ground it stood on. There is no traffic nobody asked for,
  so the throttle is not the module's to own; the consumer that wants the data
  decides how hard to chase it, which is §2.4's argument about publish policy
  applied to cadence instead of transport. Two consumers disagreeing is no
  longer a defect: the union of subscriptions wins, and a device stays polled
  while anyone still wants it. Deleted with the concept: `s_consecFails`, the
  offline bitmask, the ≥30 s throttle, §2.7's failure-backoff filter and
  `mbEvt_deviceState`. A subscriber that wants availability counts failed
  `mbEvt_txn` itself and publishes it where that word already means something —
  the MQTT bridge's retained `<topicPrefix>/availability` (§2.13).

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

**Q3 closed 2026-08-11 — scope is a bitmask, and `MB_MAX_SUBS` stays 8.
Re-keyed onto plans 2026-08-12.** The question was "is 8 enough", and it was the
wrong question: at one entry per device per consumer, a fusion path over four JK
packs spends four of them and 8 is marginal. The answer is that a subscription
covers **several** things at once, so the fusion consumer spends one entry and
the census is MQTT + Trice + fusion ≈ 3. The entry is
`{planMask, eventMask, cb, ctx, inUse}` = 16 B padded, so the table is **128 B**,
in main SRAM (~60 KB free) rather than CCM (~6 KB) since nothing about it is
latency-critical. The retained `char[16]` device name is gone with the name-based
scope that needed it.

What changed on 2026-08-12 is *what* the bits count. They were `devices[]`
positions; they are now plans (§2.3), because a consumer has opinions about a
kind of hardware read at a cadence, not about a position in an array — so adding
a fourth pack to an existing plan needs no consumer change, where a device mask
would have left the fusion path's `0x0F` quietly wrong. It also unpins
`MB_MAX_DEVICES`, which the `uint8_t` device mask had been holding at 8; the
device cap is now a flash-size question like the point ceiling.

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
step 15 CMake check enforces.

One struct, no copy, no translation layer, and §1.2 holds by construction rather
than by duplication. The deciding cost was not the ~30 lines: **v2 forces a
field change**, because a failure inside `capabilities[0].points[2]`
(§3.2, §4) has no device index, so the error location must grow — under a mirror
that edit lands in two structs plus the translation plus the 422 renderer.
*(Closed 2026-08-12: the location did not grow, it changed kind — see §3.3. The
argument for one shared struct held either way, and the edit it predicted landed
once instead of four times.)*
`fMbByteSource` and the API's `fModbusByteSource` were deliberately left as two
typedefs: C treats them as the same type, so the facade passes callbacks through
with no cast.

The move itself is a rename over ~30 sites (compiler, store, `http_server.c`,
`modbus_default_config.c` (until step 6 deletes it), `modbus_walker.c`, three
host tests) and lands with
**step 1**, which is the first step that needs the type. Until then `modbus.h`
still carries the `sModbusCompileError` mirror and its `OPEN Q2` comment.

**Closed 2026-08-11 — everything links by dense authored id, and this reverses
part of Q3.** Q3's close rejected an authored `"id"` field as *"a second name
for the same thing"*. That was right while position-as-identity had one job, the
subscription mask. It has three: the mask bit, the requester-facing `ptOrd`
(§2.9a), and — once capability, plan and device became separate sections —
the mechanism by which one section refers to another. At three jobs an explicit
id stops being a synonym.

What forced it was the compiler, not taste. §3.2's earlier draft had plans
resolving **point names** against "the capability it just emitted", which cannot
work: plans follow *all* capabilities, so a name may belong to any of them. The
repairs were a RAM index of every point name, or reading emitted records back —
and read-back is impossible in `Modbus_ConfigVerify` (§2.10), whose sink counts
instead of storing, which would have split verify and compile into two
behaviours. An id needs neither: checking it is a comparison against a count the
compiler already holds. See §2.6 for the model, §3.2 for the records.

Three consequences worth stating separately, because each retires something:

- **Names stop linking anything.** A point `name` is the MQTT topic suffix and
  nothing else, so §4's "unique within the capability" rule disappears rather
  than needing an implementation.
- **Reordering becomes a compile error**, not a silent reassignment. The id is
  authored *because* position is the identity — writing it down is what makes an
  insertion detectable (§2.6, §4).
- **Blocks are authored**, `{base, regs}` on the capability. Deriving them from
  where the points happen to fall was the alternative and loses the property that
  matters: authored blocks precede the points in the same capability, so a point
  outside its block is a **compile** error rather than an exception 2 from a
  slave months later.

**Closed 2026-08-11 — `addrStride` is a divisor, and the dialect is a group.**
§2.6, §3.2 and §2.16 step 12 disagreed three ways about where the stride is
applied: "one multiply in the engine", "the compiler computes the wire address
from the capability's own base convention" (from a base field that does not
exist), and "`addrStride` enters the address computation" five steps later. The
answer is that a point's `addr` is authored as the vendor's table gives it and
stored verbatim, and the stride converts **address units to registers** in the
engine — contiguity, frame quantity, decode index. Nothing is baked in at
compile time, so step 12 has real work and writes need no stride arithmetic at
all (§2.6).

It also generalised: `addrStride` is not a special case but the first of four
dialect fields, joined by `writeFc`, `maxReadRegs` and `blocks[]`. The engine
does not know what it is talking to; the capability supplies the constants for
its one code path. **`writeFc` is the correction that mattered** — §2.9a had
FC16 as an internal optimisation the module could choose freely, and the JK
implements *only* FC 0x03 and 0x10, so a module choosing FC06 talks to nothing.
The read caps go in the same group, which is why §7's "read caps" entry closes
here rather than at step 13: §3.2 promises the format moves once, and step 13
is seven steps too late to add a field.

**Closed 2026-08-11 — the API is a third event source, and a borrow ends with a
notification.** The surface had no concurrency model at all: `Subscribe`,
`RequestCatalogue`, `Request` and `ConfigApply` are called from the mqtt, http
and cmd tasks while the modbus task dispatches, and contract 8 admitted the race
instead of deciding it. Q4's close on synchronous dispatch made this sharper, not
softer.

The answer needed no new mechanism. §2.7's timers and §2.12's port completions
already post into one queue the modbus task drains; the API becomes the third
source on it, so contract 1 is a consequence of how the module is called rather
than a rule callers must respect, and there is **no mutex anywhere in the
module**. What took the deciding was that "everything posts" is wrong in both
directions — `ConfigCompile` must stay on the HTTP task (its byte source is the
socket, and posting it would stall the engine for the length of an upload), and
`Subscribe` must stay synchronous (a posted subscribe cannot return "table
full"). §2.12b has the three groups.

`Unsubscribe` was the one call that could not be made safe cheaply, and deleting
it in favour of a mutable event mask was proposed and rejected. A
pointer-to-pointer `ctx`, so the module could observe a release, was also weighed
and rejected: it shortens the window between the dispatcher's check and its call
but cannot close it, because the load and the call are not atomic against the
free. What closed it was noticing the module already had the pattern —
§2.9a ends a request's borrow with a **notification**, not a flag. So
`Unsubscribe` posts, the module clears the entry on its own task, and one final
`mbEvt_released` callback is the moment the `ctx` may be freed. One memory rule,
two instances, and the event enum goes from five to six for a reason unrelated to
the `mbEvt_writeResult` deletion that had put it at five.

**Closed 2026-08-12 — the module knows nothing about transports, so the transport
question leaves the module.** §2.5 defined a port by its members and left three
things unstated (who owns the response timeout, the shape of the per-frame
parameter block, what the rx callback reports for a short or CRC-bad frame), and
§9 carried "the test port has no transport" as the largest unknown below the
line. Both dissolved together once the coupling was named properly: the module
holds a **function pointer, its own buffers and a callback it exports**, and a
driver is *registered* into a port slot from outside. It cannot know a pin, a
UART or a socket, so the response timeout and the framing gaps are the driver's
by necessity rather than by preference, and the parameter block shrinks to
`baud`, `format` and the timeout.

The rx question was answered by refusing it: a driver reports how **reception**
ended — frame, timeout, line error, tx failure — never what a frame *means*. A
short frame and a CRC-bad frame are both "a frame arrived" and are the engine's
to reject, which keeps one definition of a valid reply.

An earlier draft had the buffers owned by the *driver* and borrowed upward. That
was backwards and carried a loan with no stated end; module-owned buffers delete
the question rather than documenting it. They move to main SRAM, because a DMA
driver writes into them (§3.7).

**The test port's transport is now nobody's decision to block on.** It is an
App-layer driver in slot 1, outside `App/Modbus/`, initially a **stub fed by the
existing `modbus inject` command** — so the module carries no test hook from step
8 onward while the integration suite keeps working unchanged. A real peer (HTTP
to the harness, a socket, a spare UART) replaces the stub's byte source later,
with no module, config or §2.5 change. Two things were rejected on the way:
choosing a transport *now* (a spare USART needs a schematic answer open since §7;
USB CDC shared with the CLI needs a mux and an arbiter written for a test-only
feature), and treating the stub as a true dead end, which would have broken the
suite's response fabrication for five steps to save no code at all. §2.16 splits
step 9 accordingly.

**Closed 2026-08-12 — `Modbus_Probe` is deleted, and the bus has no outside
entrance in either direction.** The question that settled it was not "should
probe take a port" but "what is probe *for*", and the answer turned out to be
symmetric with `inject`: both bypass the compiled config to make one transaction
happen with caller-supplied content, `inject` feeding a reply **upward** and
probe sending a request **downward**, and both existed for integration testing.

So the pair is decided as a pair. **Testing keeps the upward half and drops the
downward one** — the test peripheral supplies replies, the suite asserts on what
comes up, and that the module forms a correct frame downward is trusted and
checked once against real hardware rather than continuously asserted. That
leaves probe with no test role, and its other roles had already been absorbed:
§2.9a reaches every point of a capability whether or not a plan watches it, and
`sModbusReqReply.exc` carries the exception code that was most of the diagnosis.

What genuinely goes is discovery under total uncertainty — scanning for an
unknown address, or reading past a declared block to find where it ends. That is
a bench activity at a board you are standing next to, where a USB-RS485 adapter
beats any firmware affordance. Three options were weighed: keeping probe CLI-only
(rejected — its only surviving justification was *remote* diagnosis and the CLI
needs physical access), giving it an HTTP endpoint to make that justification
real, and moving it to an App-layer tool driving the RS485 driver directly
(rejected with the rest once the downward direction stopped being tested).

The invariant is the payoff, and it is worth more than the call: **every frame
the module puts on a wire comes from the compiled config, and every reply it
decodes came back from one.** §2.5 could previously say the module has no test
hooks only by ignoring §2.9's "one direct-access hole"; now it is true without
qualification. Two smaller things fall out: §2.1's Commands group is one call,
and §2.12b loses the only blocking call in the API.

**Closed 2026-08-12 — `Modbus_Request` is bounded, and the timeout is the bound
that matters.** Neither `count` nor `timeout_ms` was capped. The two are not
equally serious: an oversized `count` needs no separate defence, because the
deadline expires partway and §2.9a already defines exactly what happens to the
unfinished and never-started items — whereas an unbounded `timeout_ms` makes
"the callback always fires within it" an unbounded promise, which is the same as
not making one. That is the rule the whole borrowed-memory model rests on.

So: `timeout_ms` is 1…60000 with **0 rejected** — there is deliberately no
"wait forever" value — and the ceiling is where a request stops being a request
rather than where the bus gives out. Anything wanting the line for more than a
minute is asking to be polled, and §2.6's plans are what polling is.

`count` is 1…100, in **`MB_REQ_MAX_ITEMS`, its own constant**. Tying it to
`MB_MAX_POINTS_TOTAL` was proposed and rejected: the point ceiling is a
flash-size question and the batch ceiling is about how long one caller may hold
a shared line, so the two move for unrelated reasons. A caller needing more
makes more requests — they are independent, each with its own deadline and
callback.

Two things fell out rather than being decided. Validation splits across two
tasks because §2.12b already put flash reads on the modbus task, so a
synchronous return can only check constants and per-id validation becomes a
per-item result — which is how a config swap already behaved. And **repeated ids
in one batch are legal**: items run in order and each slot carries its own
result, so there is no distinctness rule and no O(n²) scan on the engine task.
The FIFO is pinned at 8 as a fairness bound, since the deadline runs from
submission and depth × timeout is the worst wait for the last entry.

**Closed 2026-08-12 — a request-driven read emits like any other read.** §2.2
says the module emits everything it reads; §2.9a routes a request's outcomes to
the requester. The overlap was never resolved, and it decides whether an HA
entity updates immediately on a set and whether an unmonitored point can reach a
topic with no discovery message behind it.

It is answered by **not** carving the exception. §2.2 introduces itself as the
rule most likely to be eroded by a convenient exception, and §8 entry 3 records
two rounds of eroding it already; "everything it reads, except when someone
asked" would be the third and the first to survive. The precedent that looks
like it applies does not: `mbEvt_writeResult` was deleted because an *outcome*
belongs to its requester, and a register's value is not an outcome but a fact
about the plant. The requester's copy differs by being **correlated** — beside
the id and the result — which no broadcast sample expresses.

What settled it was that emitting makes the consumer simpler rather than
noisier. The MQTT bridge stops publishing a set's read-back itself and lets it
arrive as an ordinary sample, so there is one publish path instead of two, and
the completion callback shrinks to a success/failure line. The junk-topic worry
is consumer judgement (§2.4) and the bridge's rule falls out of §2.11 already:
*publish if I created an entity for this point*, which covers a number entity on
an unmonitored setpoint and drops a diagnostic read nothing listens to. Settled
alongside it: a request's transactions raise `mbEvt_txn` too, since a
diagnostics subscriber should not have a blind spot shaped like requested
traffic.

**Closed 2026-08-12 — a request carries one array of items.** The question
arrived sideways, while deferring how much per-pending memory the MQTT bridge
should reserve: if the answer is "8 bytes either way", why are those 8 bytes
spread across three allocations? Parallel `ids`/`values`/`results` cost the
caller three borrows where one would do, left index alignment as an invariant
nothing enforced, and declared the input `const int32_t *values` while the module
writes read-backs through it — `id` is in-only, `value` in/out, `result`
out-only, and a single `const` on a single pointer cannot say three different
things.

`sModbusReqItem{value, id, result}` packs to exactly 8 bytes, so a 100-item
batch costs 800 B under either shape and the change is free. What it removes
besides the defect is an argument: the parallel form had to justify each
returned array in prose, and an item array makes the correlation structural.
`sModbusReqReply` drops to three members.

**The requester-side memory question is not closed, deliberately** — it is the
bridge's own call at step 5, and both shapes fit. What this decides is only that
the smallest version is one `sModbusReqItem`, the API's own type, with no
bridge-specific struct around it (§2.13).

**Closed 2026-08-12 — the compile result carries codes, not sentences.** Q2
predicted that v2 would force the error location to grow a field, because a
failure inside `capabilities[0].points[2]` has no device index. It did not grow;
it changed kind. `{deviceIdx, txnIdx, pointIdx}` was a **path encoded in
integers** that worked only because v1's JSON was exactly three levels deep, and
v2's three top-level arrays give three differently-shaped paths.

A path *string* was proposed first and rejected on the rule that decided this:
**an error is a number.** Text in a result struct is paid for on every result
whether or not anyone reads it, cannot be reworded without breaking every test
that `strcmp`s it, and makes reporting more than one failure prohibitive.
`{err, field, idx[3], section}` plus counts is **20 bytes against ~136**, with
`ModbusCfg_ErrString`, `_FieldString` and `_PathString` producing text where a
person reads it — the 422 body and the CLI. Two enums rather than one because
*out of range* and *unknown key* apply across ~40 fields and folding them would
duplicate every reason.

One discipline that needed stating because the usual justification does not
reach it: **`eModbusCfgErr` is append-only.** The rule about never renumbering
was written for enums persisted in flash, and this one never is — but it crosses
the HTTP boundary into a body a harness pins, which demands the same care for a
different reason.

Two things came with it. `sModbusConfigCounts` becomes
`{capabilities, plans, devices, points}`, since transactions cease to exist at
step 6. And **multiple failures per upload became affordable** — 80 B for four,
against ~550 for two of them under strings — which is recorded in §3.3 as newly
possible rather than adopted, since continuing past a failure means separating
semantic errors from structural ones.

**Closed 2026-08-12 — line format is a device parameter, and the timing never
needed it.** The framing derivation assumes 11 bits per character while the port
runs 8N1 at 10, which looked like an unstated conservative margin. It is
stronger than that: **11 bits is the maximum any RTU format uses** — 8E1, 8O1
and 8N2 are all 11, and 8N1 is the widespread non-conformant variant — so the
derivation is *correct for every format*, not merely safe for this one. Timing
therefore takes no format input at all, and `sModbusPortParams.format` (§2.5)
configures the UART and nothing else. The 10 % excess on 8N1 is safe both ways,
because the master owns the bus and there is exactly one reply per request.

`format` becomes a **device record field at step 6**, spending the byte
`sModbusDeviceRecord` already had spare, and is authored in the JSON beside
`baud`. Reserving the byte silently was the alternative and was rejected: format
is the same category of fact as baud — a property of the slave, not of the wire
— and §2.6's argument for making baud configurable applies to it word for word.
It is not speculative either, since **8E1 is RTU's own default**, so the board
today supports the common variant and refuses the conformant one.

§5.5 carries the trap this will otherwise spring: on STM32, 8E1 is
`UART_WORDLENGTH_9B` + `UART_PARITY_EVEN`, because the HAL counts parity inside
the word length — `8B` + `EVEN` yields 7 data bits and fails like a wiring
fault.

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
**once** — the point record's `publish` deletion (§2.4), the capability, block,
plan and time-table records, the device record's
`portId`/`planId`/`baudCode`/`format`,
the dialect group and the period widening to `uint32_t` all land together. The
layout is §3.2; the consequence for `ptOrd` is below.

**`ptOrd` is capability-relative, and identity is `{devOrd, ptOrd}`.** A shared capability read
from four packs yields four distinct samples from one point record, so a point
ordinal alone no longer identifies a reading. Both fields are already in
`sModbusPointDesc`; what changes is the meaning of one comment in `modbus.h`, and
that no consumer may key on `ptOrd` by itself. Under ID linking `ptOrd` **is**
the authored `pointId` and `devOrd` the authored `deviceId` — the API's names
for them are kept, since the API never sees the JSON.

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
   and `ctest` is the whole check. `Modbus_Request`'s **bounds land with its
   shape** (§2.9a) — `count` 1…100, `timeout_ms` 1…60000 with 0 rejected,
   rejected synchronously — so no consumer is ever written against an unbounded
   version, even while the facade satisfies it with `count = 1` over the
   existing single slot.
2. **Subscription table + dispatch** — 8 entries of 16 B in main SRAM, scoped by
   **plan** mask, dispatched synchronously (§2.15 Q3/Q4). `publish_point()` stops
   calling `MqttBridge_Publish` and raises an event for every decoded point.
   `mbEvt_deviceState` is deleted and availability moves to the bridge (§2.15
   Q5). Demand-driven polling lands here too — a plan outside the union of live
   subscription masks gets no timers (§2.3) — which under the step-1 facade means
   the walker skips devices nothing subscribes to.
   Carries the concurrency model (§2.12b): `Subscribe` claims its slot in a
   critical section and writes `inUse` last, `Unsubscribe` posts and completes
   with `mbEvt_released`, and contracts 4 and 8 are rewritten in the header.
   The facade's posted calls land on whatever queue the walker has until step 10
   builds the real one — the *contract* is what must be right here, since
   consumers are written against it from step 3 onwards.
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
   capability, block, plan and time-table sections, the transaction record
   deleted, `portId`/`planId`/`baudCode`/`format` on the device record, periods
   widened
   to `uint32_t` on the time table, **`writeMin`/`writeMax` widened to `int32_t`
   and `fc` moved onto the point (§2.9a, §2.6)**, the four dialect fields
   (`addrStride`, `writeFc`, `maxReadRegs`, `blocks[]`), **id linking with
   authored dense ids and their run check**, the point ceiling raised to 384,
   `MODBUS_LUT_VERSION` → 2. The compile result goes numeric with it (§3.3) —
   `{err, field, idx[3], section}` replacing the two strings,
   `sModbusConfigCounts` gaining capabilities and plans and losing transactions,
   and the 422 body changing shape. Compiler,
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

8. **The port contract** (§2.5) — `modbus_port.h`, `Modbus_PortRegister` /
   `Modbus_PortDone`, module-owned frame buffers in main SRAM, the four
   completion outcomes. The RS485 driver first, behind the existing synchronous
   engine, so the contract is proven before anything depends on its asynchrony.
   `Modbus_InjectResponse`, `mbPort_disabled`, `mbPort_uart6` and the
   port-selection call are deleted here, and §7's overrun row closes because
   `mbPortDone_lineError` makes an overrun a distinct, counted outcome.
9. **The test peripheral** — an App-layer driver registered into slot 1,
   **outside `App/Modbus/`**, whose byte source is the existing `modbus inject`
   command. The module keeps no test hook in either direction (§2.9), and the
   integration suite keeps working unchanged: it already feeds replies and
   asserts on what comes up, which is exactly what this mechanism does.
   `Modbus_Probe` and `modbus probe` are deleted here, and `modbus port` with
   them.
10. **Event-driven scheduling** — per-device timers, sequences, drop-and-count,
    per-device teardown on config swap, and timers created and destroyed by
    subscription rather than by config alone (§2.3). The 100 ms tick, the
    traversal and `s_lastPollTick` go with it, and so does `s_consecFails[8]` —
    there is no failure backoff to filter with (§2.15 Q5).
11. **The write path becomes real** (§2.9a). The *shape* lands at step 1 — the
    facade offers `Modbus_Request` and satisfies it with `count = 1` over the
    existing single slot, so consumers are written against the final signature
    from the start. The *engine* lands here: batch execution, stop-at-first-
    failure, the three-array reply, the submission FIFO, and the address taken
    from the capability's stride so a byte-addressed slave is written where it
    is read.
    `MbCfg_FindWritablePoint()` is deleted at step 5, when MQTT starts
    resolving names from the catalogue instead of from flash.
12. **Act on the device model** — §2.6 becomes behaviour rather than stored
    fields: `baudCode` and `format` drive the line parameters handed down with
    each frame (and `format` is where §5.5's `WORDLENGTH_9B` note earns its
    keep),
    `portId` selects the port from the module's table, and several devices share
    one capability. The dialect group starts being obeyed: `addrStride` divides
    in all three places (contiguity, frame quantity, decode index), `writeFc`
    selects the write frame, and `maxReadRegs` plus the block bounds split
    derived reads. No record change here; step 6 already wrote the format. Host
    tests first.
13. **Delete `jk_bms.c/h`**, drop the `jk` CLI tree, land the JK config, and read
    DeviceInfo through it at 115200 — an ordinary capability, plan and request,
    since the dossier already gives the block bases and lengths.
14. **`http_server.c` / `cmd_parser.c`** onto `Modbus_*`; `Modbus_Init()` moves
    to `App_DefaultTaskEntry`.
15. **Dependency check in CMake.**

**Acceptance for steps 1–7:** MQTT topics, payloads, retain flags, HA entity set
and every Trice string in [§6](#6-testing) unchanged — the integration suite is
the check; `ctest` stays green; `modbus dump on` prints every decoded point with
the broker down.

**Acceptance for steps 8–15:** the same MQTT and HA surface, reached through a
different engine. The suite is **re-pointed** at step 9 rather than rewritten
(§6), so the Trice strings above stay the check for the new engine too.

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

#### v2 — the target layout (settled 2026-08-10; capability/plan split and ID linking 2026-08-11, §2.6)

Not written yet. One format change, `MODBUS_LUT_VERSION` 1 → 2, carrying
everything §2 needs; the version field is what makes it safe, since a region
written by older firmware fails validation rather than being misread.

The stream gains **two sections ahead of the devices** — capabilities, then
plans — and **loses the transaction record entirely**, because read blocks are
derived rather than authored (§2.6):

```
[Header]
[Capability][Block × blockCount][Point]…[Point{decodeType=0}]  <- point sentinel
[Capability]…
[Capability{name[0]=0}]                        <- capability sentinel
[Plan][TimeTable][pointId × entryCount]…
      [TimeTable{entryCount=0}]                <- time-table sentinel
[Plan]…
[Plan{name[0]=0}]                              <- plan sentinel
[Device]…[Device{slaveAddr=0}]                 <- device sentinel (end)
```

```c
typedef struct __attribute__((packed)) {
    char     name[16];        /* display only; name[0] == 0 = end-of-caps  */
    uint8_t  addrStride;      /* address units per register (§2.6)         */
    uint8_t  writeFc;         /* 6 = FC06, 16 = FC16 — a dialect fact      */
    uint16_t maxReadRegs;     /* per-request quantity cap; 0 = 125         */
    uint8_t  blockCount;      /* sModbusBlockRecord × blockCount follow    */
    uint8_t  reserved;
} sModbusCapabilityRecord;                           /* 22 bytes — NEW */

typedef struct __attribute__((packed)) {
    uint16_t base;            /* first wire address of the block           */
    uint16_t regs;            /* length in REGISTERS, not address units    */
} sModbusBlockRecord;                                 /* 4 bytes — NEW */

typedef struct __attribute__((packed)) {
    uint8_t  decodeType;      /* eModbusDecodeType; 0 = end-of-points      */
    uint8_t  flags;           /* MB_PT_READ | MB_PT_WRITE  (access, §2.6)  */
    uint8_t  functionCode;    /* MOVED HERE from the txn record            */
    uint8_t  length;          /* ASCII register length; unused otherwise   */
    uint16_t addr;            /* wire address, verbatim as authored        */
    int8_t   scalePow10;
    uint8_t  unit;
    int32_t  writeMin, writeMax;   /* scaled-int domain (§2.9a)            */
    char     name[24];
} sModbusPointRecord;                          /* 40 bytes (v1 was 39) */

typedef struct __attribute__((packed)) {
    char     name[16];        /* display only; name[0] == 0 = end-of-plans */
    uint16_t capId;           /* the capability its point ids index into   */
} sModbusPlanRecord;                                 /* 18 bytes — NEW */

typedef struct __attribute__((packed)) {
    uint32_t period_sec;      /* ≥1; uint32 so 24 h is expressible         */
    uint16_t entryCount;      /* 0 = end-of-time-tables; uint16 pointIds
                                 follow this record, entryCount of them    */
} sModbusTimeTableRecord;                             /* 6 bytes — NEW */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;       /* 1-247; 0 = end-of-devices                 */
    uint8_t  baudCode;        /* rate-table index; 0 = 9600                */
    uint8_t  portId;          /* eModbusPortId — index into the port table */
    uint8_t  format;          /* eModbusLineFormat; 0 = 8N1 (§2.6)         */
    uint16_t planId;          /* index into the plan section               */
    char     topicPrefix[16];
} sModbusDeviceRecord;                             /* 22 bytes (was 17) */
```

**No record stores its own id.** Ids are dense ordinals equal to array position
(§2.6), and a linear scan over a self-describing stream already knows the
position of what it is reading. The `"id"` in the JSON is an authoring
assertion the compiler checks, not a field it stores.

**Sentinel or count, decided by one rule: does anything address it?** A
capability, point, plan, time table and device are all reachable by id, so they
are records in a stream and a reader must be able to consume whole ones —
sentinel. A block base and a time table's point id are *payload*: nothing
anywhere refers to "block 2 of capability 1", so they are plain arrays behind a
count on the record that owns them. This also sidesteps a sentinel value that
could not exist: under dense numbering `pointId 0` is legal, so a
sentinel-terminated id list would have needed a reserved `0xFFFF`.

**A time table nests inside its plan.** An earlier draft had the plan flat — a
list of (period, point) pairs, with distinct periods found by scanning — on the
argument that one sentinel rule beats two. The engine's own structure decided it
otherwise: a timer, a sequence and a missed counter are all per *(device,
period)*, so the object exists whether or not the format names it, and naming it
makes the diagnostics key on `{deviceId, timeTableId}` instead of on a period
value recovered by scanning. An **empty plan** — the time-table sentinel
immediately — is the legal way to say "capable but never polled" (§2.6).

**`functionCode` moved onto the point** because there is no transaction record
left to hold it, and it was always a property of the register anyway: holding
and input are different address spaces. It also puts the `w`/`rw`-requires-
holding rule (§2.9a) next to the field it constrains.

**`offset` became `addr`, authored verbatim.** Offsets existed to be relative to
a transaction's `startAddr`; with transactions derived, there is nothing to be
relative to. The author writes the address exactly as the slave's register table
gives it — `3132` on a Solis, `0x1400` on a JK — and the compiler stores it
unchanged. `addrStride` is applied by the engine, never baked in here (§2.6).

**References are ids, never names and never byte offsets.** That keeps two
properties the format was built for. The compiler stays **single-pass with no
backpatching and no index**: it emits capabilities while counting their points,
then plans — checking each `capId` against the capability count and each
`pointId` against that capability's point count — then devices, checking
`planId` the same way. And nothing in the stream is position-dependent, so
opening the device section is open-plus-drain of the two sections ahead of it.
If that ever matters the header may carry section lengths; that is header
metadata like `streamLen`, not a pointer in the stream.

**Section order is forced by the single pass**: capabilities, then plans, then
devices. Each references only what precedes it, so every id is checkable when it
is met and nothing needs a second visit.

**Read blocks are derived by the engine, at device construction** — grouping a
time table's points by function code and ascending address under the
capability's `maxReadRegs` and never crossing a block boundary (§2.6). They are
not stored, and the compiler does not derive them either: it cannot see a point
it has already streamed out, and storing a pure function of the stream inside
the stream would only create two things that can disagree.

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
table and a `format` outside `8N1`/`8E1`/`8O1`/`8N2`, with §2.9a an unknown
`access` value and `w`/`rw` on anything but a `holding` point (and, under
`writeFc: 6`, anything but a 1-register one),
and with §2.4 the `publish` object becomes an unknown key like any other
— which is the useful failure mode, since an old config is rejected by name
rather than silently losing a setting. Failures report the first offending
`{device, transaction, point, field, reason}` — that is what makes the HTTP 422
useful, and it is the shape that breaks in v2 (below).

**What v2 adds is all range checking, and it needs no new machinery** (§2.6,
§3.2). Ids must run `0, 1, 2 …` within each array, or the config is rejected at
the object where the run breaks. A `capId`, `planId` or `pointId` must be below
the count of the section it indexes — and because section order is forced, that
count is a running total the compiler already holds. A point must fall inside one
of its capability's declared blocks and end within that block's `regs`; the
blocks precede the points in the same capability, so this is checkable at the
point that offends. Bounds become ≤8 capabilities / plans / devices, **≤384
points** and ≤384 time-table entries — the ceiling is now the 16 KB region
(≈380 points of stream at 40 B each), not RAM, since §2.4 removed the
`MB_MAX_POINTS_TOTAL` → CCM coupling (§3.7). Raising it further is a flash-map
move, not a constant change: the LUT regions cannot grow in place, because WG
Time sits immediately above the selector at 0x102000.

#### The compile result is all numbers

`{device, transaction, point, field, reason}` cannot locate a v2 failure: a
problem inside `capabilities[0].points[2]` has no device index, and a config now
has three top-level arrays with three differently-shaped paths under them. The
triple was never really three integers — **v1's JSON was exactly three levels
deep, so a path happened to fit in three ints**, and v2 is where the disguise
stops working.

The replacement carries no text at all:

```c
typedef struct {
    sModbusConfigCounts counts;  /* what compiled before the failure      */
    uint16_t err;                /* eModbusCfgErr;   0 = none             */
    uint16_t field;              /* eModbusCfgField; 0 = not a field      */
    int16_t  idx[3];             /* -1 = n/a; meaning given by section    */
    uint8_t  section;            /* eModbusCfgSection                     */
    uint8_t  ok;                 /* == (err == 0)                         */
} sModbusCompileResult;          /* 20 bytes, no padding                  */
```

**20 bytes against ~136** for the `char field[24]` + `char reason[64]` it
replaces, and `fail()` becomes three assignments instead of two string copies.
Prose in a struct is the wrong place for prose: it is paid for on every result
whether or not anyone reads it, and it makes reporting more than one failure
prohibitive.

Text comes from functions instead, which is what keeps the result legible where
a person actually looks at it:

```c
const char *ModbusCfg_ErrString(uint16_t err);
const char *ModbusCfg_FieldString(uint16_t field);
int         ModbusCfg_PathString(const sModbusCompileResult *r,
                                 char *out, uint32_t size);
```

`ModbusCfg_PathString` is what makes `idx[3]` legible: `section` says what the
indices mean and the renderer assembles them, with `field` as the last segment —
`capabilities[0].points[2].scale`, `plans[1].timeTables[0].everySec`,
`devices[2].slaveAddr`, or `plans[1].timeTables[0].points[3]` where the leaf is a
bare id rather than a key.

**`field` stays a separate enum from `err`** because they compose. *Out of
range*, *unknown key* and *missing required key* apply to many keys; folding
them together would need one value per (key, reason) pair and duplicate every
reason across ~40 fields. Two small enums also let `field` earn its keep twice,
as the offending key and as the path's leaf.

**`eModbusCfgErr` is append-only**, and for a reason the usual one does not
cover. The doc's rule about never renumbering exists for enums persisted in
flash (`eFwuRes`, `eModbusDecodeType`, `eCrashType`); this one is never
persisted, so that argument does not apply — but it **crosses the HTTP
boundary** into a 422 body a test harness pins values against, which demands the
same discipline for a different reason.

The response carries the code and the rendered forms together, so a harness
asserts on the number and an operator reads the sentence:

```json
{"err":12,"message":"scale must be an exact power of ten",
 "path":"capabilities[0].points[2].scale"}
```

The strings are ~2.2 KB of `.rodata` (roughly 1.6 KB of reasons, 0.6 KB of field
names) against the 3.25 KB §2.15 Q6 returned. Emitting bare codes and mapping
them host-side would cost nothing on the device and was rejected: a `curl` that
returns `{"err":12}` is not the useful 422 this section exists to produce.

**Multiple failures per upload become affordable, and are not designed here.**
At 20 bytes the first four failures cost 80 B where two strings each would have
cost ~550, so an operator could fix four problems in one cycle instead of four.
It is not free — the compiler aborts at the first failure today, and continuing
means separating semantic errors it can skip past (bad scale, unknown unit,
out-of-range id) from structural ones it cannot (malformed JSON). Recorded as
newly possible, not as decided.

`sModbusConfigCounts` changes with it, since transactions cease to exist at step
6: `{devices, transactions, points}` becomes
`{capabilities, plans, devices, points}`, which is also what
`/api/modbus/config/status` and the `Modbus config: staged …` line report.

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
needs one (§2.4). `s_consecFails[8]` is **deleted too**, later and for a
different reason: §2.15 Q5 moved device availability out of the module, so there
is nothing for it to count towards. What survives into the engine is one frame
buffer per port instead of the single shared `s_regBuf[125]`.

The word this document used for one traversal was "lap". It is retired: the
engine has no traversal to bound. The unit of work is a **sequence** (§2.7) —
one device's derived read blocks for one time table.

Per-device availability: 3 consecutive failed transactions mark a device offline
(retained `<topicPrefix>/availability`); the first success brings it back. Offline
devices are throttled to ≥30 s so a dead slave cannot starve the bus.

**All of that leaves the module** (§2.15 Q5, reversed 2026-08-12). The *topic*
survives unchanged and so do the two decisions behind it, which are MQTT's and
were always MQTT's: it is deliberately **not** an LWT — LWT is a property of the
one TCP session for the whole bridge and cannot express "this slave stopped
answering while the bridge is fine" — and the suffix is `/availability` rather
than `/status` because the Solis device identity equals the bridge prefix, so
`/status` would collide with the bridge-wide LWT topic; it also matches HA's
`availability_topic` convention. What moves is only *who computes it*: the bridge
counts failed `mbEvt_txn` per device (§2.13). One shipped behaviour does not
survive the move, and that is intended — a device answering **exceptions** is
answering, so it no longer counts towards being offline (`device_mark_result`
treats every non-`mbErr_ok` as a failure today, `modbus_walker.c:287-296`, which
would mark a healthy slave offline on three misconfigured reads).

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
| USART2, PD5/PD6/PD7 | exclusive to the module; after §2.5 it is a registered driver in port slot 0 |

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

**§2.5 makes that binding rather than hypothetical.** The frame buffers are the
module's, one tx and one rx per port, and a driver may write into them by DMA —
so they live in **main SRAM**, not CCM. At a 256-byte maximum RTU frame, two
ports cost ~1 KB, replacing the single shared `s_regBuf[125]` (250 B in CCM).
Net: CCM down 250 B, main SRAM up ~1 KB, both in the direction the budget wants.

**What §2.3/§2.12a add, in full:** a 128 B subscription table (8 × 16 B) and a
256 B request FIFO (8 × ~32 B, §2.9a), both placed in **main SRAM** rather than
CCM for the same reason — nothing about either is latency-critical. Observer event copies are not module memory at all, but they
do transit the 48 KB FreeRTOS heap in `.ccmheap`, so a misbehaving observer that
leaks its allocations exhausts the same heap task stacks come from. That is the
observer's bug to find; the instrument is `xPortGetFreeHeapSize()`, not
`modbus status`.

---

## 4. Config JSON reference

Three arrays, in dependency order: **capabilities** describe hardware,
**plans** describe what to watch, **devices** bind a plan to a slave.
Everything links by `id`, never by name (§2.6).

```jsonc
{
  "capabilities": [
    {
      "id": 0, "name": "solis",
      "addrStride": 1, "writeFc": 6, "maxReadRegs": 125,
      "blocks": [ { "base": 3000, "regs": 200 } ],
      "points": [
        { "id": 0, "addr": 3132, "fc": "input", "decodeType": "u16",
          "scale": 0.1, "unit": "V", "name": "battery_voltage" },
        { "id": 1, "addr": 3138, "fc": "input", "decodeType": "u16",
          "scale": 1, "unit": "%", "name": "battery_soc" },
        { "id": 2, "addr": 3009, "fc": "holding", "decodeType": "u16",
          "scale": 1, "unit": "%", "name": "max_charge_soc",
          "access": "rw", "writeMin": 70, "writeMax": 100 }
      ]
    }
  ],

  "plans": [
    {
      "id": 0, "name": "inverter_normal", "capability": 0,
      "timeTables": [
        { "id": 0, "everySec": 5,  "points": [0, 1] },
        { "id": 1, "everySec": 60, "points": [2] }
      ]
    }
  ],

  "devices": [
    { "id": 0, "slaveAddr": 1, "baud": 9600, "port": "rs485",
      "plan": 0, "topicPrefix": "periphnet" }
  ]
}
```

- **`id`, everywhere** — every object in every array carries one, and it must
  equal its position: `0, 1, 2 …`, no gaps, no reordering. The compiler rejects
  a broken run at the object where it breaks. It is authored rather than
  inferred so that an insertion or deletion — which would silently re-point
  every reference after it — fails loudly instead (§2.6).
- **Capability** — `id`, `name` (≤15 chars, display only), the dialect
  (`addrStride`, `writeFc`, `maxReadRegs` — §2.6), `blocks[]`, `points[]`. It
  describes what the hardware can do and contains **no periods** — nothing about
  how often anything is read.
- **Block** — `{ base, regs }`: the first wire address of an address range the
  slave implements, and its length **in registers**. Every point must fall
  inside one, a derived read never crosses one, and `regs` is the read-span limit
  the slave enforces (§2.6). One block covering the whole map is the normal case;
  the JK needs three.
- **Point** — `id`, `addr` (the wire address exactly as the slave's register
  table gives it), `fc` (`"holding"`/`"input"`), `decodeType` (`u16` `s16`
  `u32_be` `u32_le` `s32_be` `s32_le` `float32_be` `float32_le` `bitfield`
  `ascii`), `scale` (**exact power of ten**, 0.001…1000), `unit` (`""` `V` `A`
  `W` `VA` `var` `Hz` `Wh` `kWh` `varh` `VAh` `%` `Ah` `C` `min` `s`), `name`
  (≤23 chars — the MQTT topic suffix, and **only** that: it links nothing and
  need not be unique), `length` (registers, ASCII only), `access`,
  `writeMin`/`writeMax`. **No `offset` and no enclosing transaction** — read
  blocks are derived (§2.6).
- **`access`** — `"r"` (default, readable), `"w"` (writable), `"rw"` (both).
  It states what the **silicon supports**, not what this deployment does with
  it. `"w"`/`"rw"` require `fc: "holding"` — neither write function code reaches
  the input space, so a writable point on an input register is rejected at
  compile rather than writing a different register of the same number — and,
  under `writeFc: 6`, a 1-register type as well. A `"w"` point may not appear in
  a time table: it cannot be read, so watching it is incoherent.
- **Plan** — `id`, `name` (≤15 chars, display only), `capability` (the `capId`
  its point ids index into), `timeTables[]`. **`timeTables` may be empty**, which
  is how a device that is only ever written through `Modbus_Request` and never
  polled is expressed (§2.6).
- **Time table** — `id`, `everySec` (≥1), `points[]` of point ids into the plan's
  capability. A point may appear in only one time table of a plan.
- **Device** — `id`, `slaveAddr` (1-247), `baud` (optional, default 9600; one of
  1200 / 2400 / 4800 / 9600 / 19200 / 38400 / 57600 / 115200 — see §2.6),
  `format` (optional, default `"8N1"`; one of `8N1` `8E1` `8O1` `8N2` — a
  per-device line parameter exactly as `baud` is, §2.6; `8E1` is RTU's own
  default and plenty of slaves ship that way),
  `port` (optional, default `"rs485"`; one of the ports this firmware was built
  with — an unknown name is rejected at compile, §2.6), `plan` (a `planId`), and
  `topicPrefix` (≤15 chars of `[A-Za-z0-9_-]`; the MQTT namespace and the HA
  device identity — **not** the module's identity, which is `id`, §2.3/§2.14).

**Sharing is the normal case, not an option.** Four battery packs are four
devices, one capability, one plan:

```jsonc
{
  "capabilities": [
    { "id": 0, "name": "jk_pb",
      "addrStride": 2, "writeFc": 16, "maxReadRegs": 123,
      "blocks": [ { "base": 4096,  "regs": 147 },     /* 0x1000 Settings   */
                  { "base": 4608,  "regs": 147 },     /* 0x1200 Realtime   */
                  { "base": 5120,  "regs": 147 } ],   /* 0x1400 DeviceInfo */
      "points": [ /* id 0 = soc, 1 = pack_v, 2 = pack_i, 3.. = cell_v_n */ ] }
  ],
  "plans": [
    { "id": 0, "name": "pack_fast", "capability": 0,
      "timeTables": [ { "id": 0, "everySec": 5,  "points": [0, 1, 2] },
                      { "id": 1, "everySec": 60, "points": [3, 4] } ] },
    { "id": 1, "name": "pack_lazy", "capability": 0,
      "timeTables": [ { "id": 0, "everySec": 300, "points": [0] } ] }
  ],
  "devices": [
    { "id": 0, "slaveAddr": 1, "baud": 115200, "plan": 0, "topicPrefix": "bms1" },
    { "id": 1, "slaveAddr": 2, "baud": 115200, "plan": 0, "topicPrefix": "bms2" },
    { "id": 2, "slaveAddr": 3, "baud": 115200, "plan": 1, "topicPrefix": "spare" }
  ]
}
```

The spare pack watches one register every 5 minutes off the **same** capability
— which the v1 schema could not express at all, because the period lived inside
the register map.

The JK capability is also what the dialect fields are for, all four of them at
once: byte-addressed registers (`addrStride: 2`), a slave with no FC06 handler
(`writeFc: 16`), a 123-register quantity ceiling, and three separate address
blocks whose 147-register length is the read-span limit the BMS enforces
(§2.6). None of it is a code path.

**There is no `publish` object** (§2.4). How often a value is worth forwarding
belongs to the consumer forwarding it; a config carrying `"publish"` is rejected
as an unknown key rather than silently ignored. Every monitored point is read at
its plan period and emitted every time.

**`writeMin`/`writeMax` are authored in the scaled-integer domain**, which is
the raw register domain — not in display units. This is the most common
authoring mistake. They are `int32_t` (§2.9a), so the full `u16` range and
32-bit setpoints are expressible; in v1 they were `int16_t` and anything above
32767 could not be bounded at all.

**`id` is identity, and it is checked.** A device's `id` is the bit a subscriber
scopes to (§2.3); a point's `id` is the `ptOrd` a requester addresses and events
carry (§2.9a). Both are dense ordinals, so they still *are* array positions —
but because the author writes them down, reordering or inserting is a compile
error naming the object where the run breaks, rather than a silent reassignment
discovered later as a write to the wrong register. Appending stays free. That is
the whole difference from renumbering an enum, and it is why the ids are
authored at all.

Bounds: ≤8 devices, ≤8 capabilities, ≤8 plans, **≤384 points total across
capabilities**, ≤384 time-table entries total, ≤8 blocks per capability, ≤64
derived read blocks, ≤125 registers per read. **The caps count distinct records,
not instances** — four packs on a 50-point capability cost 50 points, not 200.
Instances govern only what a catalogue burst and HA discovery produce (devices ×
their capability's points), which costs flash reads and MQTT messages, not RAM.

The point ceiling is a **flash** limit, not a RAM one: §2.4 deleted the
per-point CCM arrays, so a point costs only its 40 bytes of stream, and 384 is
about what a 16 KB region holds. Device count is the exception — it is pinned at
8 by the `uint8_t` subscription mask (§2.3), not by the format.

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
# on error, v1 today:  422 {"error":"...","field":"scale","device":0,"transaction":1,"point":2}
# on error, from step 6: 422 {"err":12,"message":"scale must be an exact power of ten",
#                             "path":"capabilities[0].points[2].scale"}
#   err is an append-only code a harness pins; message and path are rendered
#   from it on the device (§3.3)

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
| `modbus monitor <on\|off>` | raw TX/RX frame dump via Trice |

**No command touches the bus.** Removed: `modbus write` (no raw write path —
§2.9a), `modbus probe` (no direct bus access at all — §2.9), `modbus start` /
`modbus stop` (no such lifecycle, §2.8), `modbus set baud <rate>` (a device
parameter, §2.6), `modbus port …` (a device lives on a port; that is config,
§2.5), and the whole `jk` command tree. Reading a register that is in no plan is
`Modbus_Request` against a capability that declares it; reading one that is in no
*capability* is a bench job with a USB-RS485 adapter (§2.9).

**`modbus inject …` stays, and changes owner.** It is no longer a hole beneath
the port — `Modbus_InjectResponse` is deleted at step 8 — but the command
survives as the byte source of the **test peripheral driver** in port slot 1,
which is App-layer code the module cannot distinguish from a UART (§2.5). It is
not a stop-gap: feeding replies in through a port *is* the integration-test
mechanism (§2.9, §6).

> **Note, out of scope here.** The HTTP API will want the same feed —
> integration tests driving the test peripheral over HTTP rather than over a CLI
> line. That is an `http_server.c` design question and is not designed in this
> document.

`mqtt publish now` is removed with `Modbus_ForceRefresh` (§2.15). Under
free-running timers there is no due-state to mark, and a consumer that wants a
value sooner is asking for a shorter period. Re-driving HA state is
`Modbus_RequestCatalogue()`, which the bridge may call at any time (§2.11);
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
MQTT: device online: periphnet             <- bridge-derived, §2.15 Q5
Modbus config: staged 1 capability 1 plan 1 device 29 points
Modbus: config swapped, active region 1
MQTT: device offline: periphnet            <- bridge-derived, §2.15 Q5
Modbus: missed periphnet 5s (n=1)          <- sequence still running when due again
MQTT: set periphnet/max_charge_soc = 95 -> req 3 items
```

`Modbus: walker started, 9600 baud` becomes `Modbus: started`: there is no
walker, no single baud, and no command to trigger it. The two device
online/offline lines change **prefix**, not content: availability is derived and
published by the MQTT bridge now (§2.15 Q5), so they are `MQTT:` lines.
`Modbus: stopped (polls=… errors=…)` disappears with the lifecycle. The missed
line is new and fires on **first** occurrence per timer, not every time — the
running count belongs in `modbus status`, not in the log (§2.7). It prints the
period for a human, but the counter it reports keys on `{deviceId,
timeTableId}`, which is the timer's actual identity (§2.6).

The staged-counts line above is its **post-step-6** form; today it reads
`Modbus config: staged 2 devices 13 txns 29 points`. It changes because
transactions cease to exist and `sModbusConfigCounts` gains capabilities and
plans (§3.3), and it is **not** covered by the steps 1-7 acceptance rule, which
binds the §6 contract table rather than these landmarks.

### 5.5 RS485 timing

**As built:** inter-frame gap 4 ms before TX; TC flag polled before releasing
DE; 5 ms silence ends the RX frame; 1000 ms response timeout. All hardcoded, all
sized for 9600, all in the engine.

**By §2.5/§2.6 all of it moves into the driver**, which is the only layer that
knows the line, and is driven by parameters handed down with each frame:
`max(3.5 char times, 1.75 ms)` for the inter-frame gap, the same floor rule for
end-of-frame silence, character = 11 bits. The 9600 case of that formula is
4.01 ms, i.e. today's constant, which is the evidence the derivation is right.
The 1.75 ms floor above 19200 baud keeps every value ≥2 ms, so millisecond
granularity still suffices at 115200. The engine never runs a character timer
after this; it sends a frame and waits for a completion event.

**11 bits is the maximum, not a safety margin — so the timing does not depend on
the line format.** RTU's formats are 8E1, 8O1 and 8N2, all 11 bits per
character; 8N1 is the widespread non-conformant variant at 10. Deriving from 11
is therefore *correct for every format the port can run*, rather than merely
generous for the one it runs today, and that is what keeps
`sModbusPortParams.format` (§2.5) a UART-configuration field with no part in the
arithmetic. On 8N1 every gap comes out about 10 % longer than strictly needed —
3.65 ms of real 3.5-character time at 9600 against the 4.01 ms computed — which
is safe in both directions: the master owns the bus and there is exactly one
reply per request, so a longer silence can never split a frame or merge two.

**Implementation note, because it fails quietly.** On STM32, **8E1 is
`UART_WORDLENGTH_9B` + `UART_PARITY_EVEN`**. The HAL counts the parity bit
inside the word length, so `WORDLENGTH_8B` + `PARITY_EVEN` gives **7 data bits
plus parity** — a device configured for 8E1 then decodes some bytes correctly
and mangles the rest, which reads like a wiring fault rather than a
configuration one.

---

## 6. Testing

### Host (`tests/`, NOR-faithful flash mock) — where most correctness lives

```bash
cmake -B tests/build -S tests && cmake --build tests/build -j8
ctest --test-dir tests/build --output-on-failure
```

Covers the store/selector/cursor, the compiler accept+reject matrix, the export
round trip, decode vectors and units. **The v2 record change lands here first**
(§2.16 step 6), before any firmware moves: LUT version bump; the capability,
block, plan and time-table sections; **id linking** — a broken `id` run, a
`capId`/`planId`/`pointId` past its section's count, and an id referring
forward, all rejected; `baud` / `format` / `port` / dialect-field accept+reject;
a point
outside every declared block or past its block's `regs`, rejected; **derived
read blocks** (contiguity in the register domain under `addrStride`, the
`maxReadRegs` ceiling, never crossing a block boundary — §2.6); an empty plan; a
time table listing a `w` point; a device naming a nonexistent plan or an unknown
port; `publish` now rejected as an unknown key; `access` accept+reject
(including `w`/`rw` on an `input` point, rejected always, and on a
multi-register type, rejected only under `writeFc: 6` — §2.6); `int32` write
bounds round-tripping past ±32767; and the export round trip over every new
field, since a field the exporter forgets is invisible until someone downloads a
config and re-uploads it. **Reject cases assert on `res.err`, not on prose** (§3.3) — a code
survives rewording the message, a `strcmp` against `reason` does not, and every
existing reject test changes that way. The stride cases want a JK-shaped vector specifically —
`addrStride: 2` is where a contiguity or quantity computed in the address domain
passes every stride-1 test and is wrong by a factor of two. Four existing tests
encode the old layout and change with it: `tests/modbus_test_stream.h:146`,
`test_modbus_store.c:104`, and `test_modbus_compiler.c:68,280`, the last being a
`threshold > 65535` reject case that becomes an unknown-key reject case.

### Integration (`tests/integration/`, live board)

Host-side C++ harness driving a board over USB/UART/UDP. Cases live in
`tests/integration/src/core/ModbusTests.cpp` and `MqttTests.cpp`, which are the
source of truth for what is asserted. `modbus_hw_*` and `mqtt_hw_*` — anything
needing a real slave or a real broker — are registered as immediate-skips, so
the rest is CI-runnable with no bus and no broker.

**The suite is not rebuilt — it is re-pointed, at step 9.** It already does the
right thing: fabricate a reply, then assert on what comes up out of the module.
What changes is only where the reply enters. Today `modbus inject` is a hole
beneath the port; from step 9 the same command feeds an App-layer **test
peripheral driver** in slot 1, and the module has no test hook in either
direction (§2.9). Cases, assertions and Trice strings are untouched.

**Integration testing is upward-only, deliberately** (§2.9). What the suite
proves is that a reply arriving at a port becomes the right decoded value, the
right event, the right MQTT topic, the right availability transition and the
right scheduling behaviour. What it does *not* prove is that the frame the engine
sent downward was correct — that is trusted, and checked once against real
hardware at the point the module is first trusted. Two things follow:

- **Timeout and malformed-frame paths become reachable without hardware** — the
  driver simply does not answer, or answers badly — and the harness controls
  *when* a reply lands, so timing is chosen rather than incidental. Both are new
  and both are upward.
- **A richer driver could hand the request up too**, letting the harness assert
  on address, function code, start, count and CRC. Nothing forbids it and it may
  be worth having; it is a bonus rather than the reason the mechanism exists.

**RTU line framing is never covered by a test peripheral**, whatever transport
eventually feeds it: the 3.5-character silence, inter-octet timing and DE
turnaround live in the RS485 driver and remain hardware-only facts.

**What leaves the Trice contract at step 9**: `Modbus port: …` and
`Modbus: stopped (polls=… errors=…)`, with the mechanisms behind them. The
`Modbus inject: …` and `Walker inject: …` lines **stay** — the command survives
as the test peripheral's byte source (§5.2), so what was a module hook becomes
App-layer plumbing without the suite noticing. What else stays is the observable
behaviour a test wants to see:

| Event | String |
|---|---|
| Monitor on/off | `Modbus monitor: on` / `Modbus monitor: off` |
| TX / RX frame (monitor) | `Modbus TX[N]: <hex>` / `Modbus RX[N]: <hex>` |
| Start | `Modbus: started` |
| Device up / down | `MQTT: device online: <id>` / `MQTT: device offline: <id>` — **bridge-emitted**, since the module has no availability notion (§2.15 Q5) |
| Missed sequence | `Modbus: missed <id> <period> (n=N)` |
| MQTT pub / sub (monitor) | `MQTT pub: <topic> = <value>` / `MQTT sub: …` |
| MQTT inject ack | `MQTT inject: <topic>` |
| Request submitted / rejected (MQTT side) | `MQTT: set <topic> = <value>` / `MQTT: set <topic> rejected` — emitted by the requester, since the module reports outcomes only to the callback that asked (§2.9a) |

MQTT monitor lines are emitted even with no broker connected, otherwise the
bridge is unobservable in broker-less CI.

**Steps 1–7 of §2.16 must keep every string above intact** — that is their
acceptance test, and the only addition is `MB …` lines from the Trice
subscriber. Steps 8 onwards keep them too: the suite is re-pointed rather than
rewritten, so the same strings remain the check for the new engine.

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
- **A test peripheral is a config binding, not a build flag.** The same image serves a
  bench board and a real one, which is the point — but it also means a config
  that binds a real device to the test port makes that device answer when the
  real one would not. `modbus status` and `/api/modbus/config/status` must show
  each device's port — and, since §2.3, whether it is being polled at all — so
  that is visible at a glance.

---

## 7. Known limits

Facts about the implementation on the board today. Most are now **in scope** —
§2 says what replaces them — so the column says which. What is left over at the
bottom is the genuinely undesigned part.

| Limit | Where | Status |
|---|---|---|
| One physical bus, hardcoded `huart2` | `modbus_rtu.c` throughout; `modbus_walker.h:23`, applied at `modbus_walker.c:328` | §2.5 — ports become a serviced set |
| One baud for the whole bus | same | §2.6 — a device parameter |
| `mbPort_uart6` in the enum but refused (pins not confirmed in the schematic) | `modbus_rtu.c:289` | **closed** — §2.5 deletes the entry at step 8: a slot with no registered driver *is* absent. The schematic question survives only for a future second *real* RS485 bus |
| Framing constants sized for 9600 — 4 ms gap, 5 ms silence | `modbus_rtu.c:122`, `:156` | §2.5/§5.5 — derived from baud inside the driver, and format-independent since 11 bits is RTU's maximum |
| Write address assumed `startAddr + offset`; JK registers are byte-addressed | `modbus_config_store.c:334` (live); `mqtt_bridge.c:377` computes the same expression but `ha_publish_entity` discards it (`(void)regAddr`) — dead, not a second bug | §2.6 — the point's `addr` is authored verbatim and a write uses it unchanged, so there is no offset left to add; the dead one goes when MQTT stops walking the config |
| A **writable point is never checked against its transaction's function code** — marking a point on an `input` transaction writable compiles clean, and the FC06 write then lands on a *holding* register of the same number, a different register in a different space | `modbus_config_compiler.c:605-609` validates width only; the shipped config is correct by authorship, not by validation | §2.9a — `w`/`rw` access requires a `holding` point, rejected at compile |
| `readPeriodS` is `uint16_t`, so periods cap at 18h12m — **a 24 h read cannot be expressed** | `modbus_records.h:89` | §3.2 — `uint32_t` on the v2 time-table record |
| One map per device, copied per device — no way to share a register map between identical slaves | `modbus_records.h` (no map record) | §2.6/§3.2 — shared capabilities referenced by `capId` |
| **The read period lives inside the register map**, so two devices sharing a map must share its periods — a spare pack cannot be polled lazily off the same map | `modbus_records.h` (`readPeriodS` on the txn record) | §2.6 — capability/plan split |
| The walker publishes straight to MQTT | `modbus_walker.c:183` | §2.13 — the whole point of the API |
| Publish policy lives in the Modbus config | `modbus_records.h:77-78` | §2.4 — deleted |
| RX is polled byte-at-a-time; a UART overrun is indistinguishable from "no byte", and the per-byte budget shrinks ~12× at 115200 | `modbus_rtu.c:151` | §2.5 — `mbPortDone_lineError` makes an overrun a distinct, counted outcome at step 8. Whether 115200 works remains an on-hardware fact still to acquire |
| Reads are FC03/FC04; writes are FC06 only, one register | `modbus_rtu.c`; `Modbus_WriteSingleRegister` | §2.6 — `writeFc` is a capability field, and it has to be: the JK implements only FC 0x03 and 0x10 and rejects FC06 |
| Writable points must be exactly 1 register | `modbus_config_compiler.c:606` | §2.6 — an FC06 consequence, so the rule follows `writeFc`; under FC16 a multi-register setpoint is legal, which the JK's `u32` Settings fields require |
| Write bounds are `int16_t` | `modbus_records.h:79` | §2.9a/§3.2 — widened to `int32_t` in v2 |
| One pending write at a time | `modbus_walker.c:35-38` | §2.9a — a FIFO of 8 batch submissions |

Not designed, deliberately:
- **A fifth dialect fact.** The group is closed at four — `addrStride`,
  `writeFc`, `maxReadRegs`, `blocks[]` (§2.6) — which covers both slaves on the
  bench. Whatever a third slave turns out to need, the constraint is unchanged
  and is the point of the rule: it must be expressible as **data on the
  capability**, never a code path, or it is refused.
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
§2.16 step 13 it is a config upload plus a `Modbus_Request`, not firmware and
not a bus tool (§2.9).

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
   facts became capability data (byte addressing → `addrStride`) and a device
   parameter (115200 → baud).
2. *Baud is a device property* — different slaves at different rates share one
   wire by time-multiplexing. `Modbus_SetBaud` left the API. Framing constants
   became baud-derived, which later moved them into the port entirely.
3. *The module must not store values* — it reads, translates, emits. Two
   corrections were needed here:
   - **Scoped subscriptions stayed.** The first pass removed them by reading the
     finding as a ban on filtering; it is a ban on *memory*. Scope is a bitmask
     — over devices then, over plans since entry 30 (§2.3) — and stores
     nothing.
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
   integration suite is rebuilt on it once. *(Sharpened by entries 24-25: the
   engine is not merely agnostic to peripherals, it holds registered drivers and
   knows no transport — and the suite is re-pointed at step 9 rather than
   rebuilt, since feeding replies through a port is what it already did.)*
6. *Scheduling is event-driven, not a poll loop* — §2.7. Per-device timers per
   distinct period emit events; the engine services them as sequences. Because
   the scheduler is independent of the servicer, drift has nowhere to come from
   and overrun is answered by dropping a stacked event and counting it. Two
   consequences that had to be made deliberate: failure backoff became an event
   filter in the engine, since a free-running timer cannot know a slave is
   dead; and the config swap became per-device timer teardown rather than a
   traversal boundary. The word "lap" is retired with the traversal.
   *(Entry 29 later deleted the backoff outright, along with the notion of a
   slave being dead; and entry 30 made a timer's existence depend on a
   subscription.)*

**Revised 2026-08-10 (second pass), closing the record-format question.** The
review above left §3.2 asserting the format would move once while §2.16
scheduled two separate record edits six steps apart (the `publish` deletion at
step 6, the device model at step 12), with §2.15's record-layout question
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
   §7 may answer. *(Superseded by entry 24: the module holds registered drivers
   and knows no transport at all, so the test port's channel is an App-layer
   choice that constrains nothing here.)*
9. *So the format moves once* — v2 carries the `publish` deletion, the map
   record, `portId`/`mapOrd`/`baudCode` and the period widening together
   at step 6; step 12 acts on fields already stored. §3.2 also stopped showing
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
    and function codes, and the requester submits an **array of items** — each
    a point ordinal and a scaled-integer value — for one device, non-blocking,
    with a completion callback and a deadline. The same array comes back with a
    per-item result filled in. §2.9a. *(It was two arrays in and three back
    until entry 34 collapsed them.)*
14. *The config decides what an item means, so it is not a write call* —
    `Modbus_Write` became `Modbus_Request`. A point's access is `r`, `w` or
    `rw`, and the module reads, writes, or writes-then-reads-back accordingly.
    That is a stronger protection than any caller-side rule, because a caller
    cannot reach a read-only register by asking differently, and it collapses
    on-demand reads and setpoint writes into one call. It also settled the
    open item beside it: `w`/`rw` requires a `holding` transaction, so the
    silent wrong-space write is a compile error.
15. *Every item is attempted, and a deadline is what makes borrowed memory
    safe* — stop-at-first-failure was proposed and rejected: the per-item result
    already reports per item, so truncating the batch destroys information the
    caller asked for and can act on. In exchange the request carries
    `timeout_ms`, and **the completion callback is guaranteed to fire within
    it** — which is the only reason a rule as strict as "the module owns your
    item array until the callback" is livable. A timed-out request is abandoned, never written into
    afterwards.

    Four things fell out rather than being decided: `mbEvt_writeResult` is
    deleted (an outcome belongs to its requester, not to every subscriber),
    `writeMin`/`writeMax` widen to `int32_t` to match the value domain,
    `MbCfg_FindWritablePoint()` disappears because the catalogue already is the
    accessible set, and FC16 becomes an internal coalescing optimisation whose
    only config surface is a dialect field on the map. *(The last of those was
    wrong and is corrected in entry 21 — FC16 is a declared dialect fact, not an
    optimisation, because the JK has no FC06 handler at all.)*

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
    `Modbus_Request` and `Modbus_Probe`. *(One, after §2.9 deleted the
    second.)*

18. *Capability and plan are different facts* — reviewing the access/polling
    ambiguity exposed that the register map carried `readPeriodS`, i.e. a
    deployment decision inside a hardware description. Two devices sharing a map
    were forced to share its periods, so a spare pack could not be polled
    lazily. The config splits: a **capability** is a flat list of points with
    access, and a **plan** is periods over a capability's points; a device names
    a plan, and the plan names its capability. Consequences: authored
    transactions disappear (read blocks are derived — from authored blocks, per
    entry 21), `fc` moves onto the point,
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

**Revised 2026-08-11 (third pass), auditing the design against itself.** The
2026-08-11 commit that opened this round listed sixteen observations; the five
that argued from shipped v1 code against unimplemented §2 design were discarded
on the standing rule that the document leads (§2). Of what remained, three
closed here and the rest are still open, listed in §7 and §2.15:

19. *Linking by name could not survive the single pass* — §3.2 had plans
    resolving point names against "the capability it just emitted", which is
    impossible because plans follow all capabilities. Every repair needed either
    a RAM index of every name or a read-back of emitted records, and read-back
    is unavailable to `Modbus_ConfigVerify`, whose sink counts rather than
    stores — so it would have split verify from compile, which §2.10 exists to
    prevent. **Everything links by dense authored id instead**, checked against
    counts the compiler already keeps. Names demote to display strings, §4's
    name-uniqueness rule disappears, reordering becomes a compile error rather
    than a silent reassignment, and Q3's rejection of an authored `"id"` is
    partly reversed — correctly, because position now has three jobs rather than
    one. §2.6, §3.2, §2.15.
20. *The address model contradicted itself three ways* — "one multiply in the
    engine" (§2.6), "the compiler computes the wire address from the
    capability's own base convention" (§3.2, from a base field that never
    existed), and "`addrStride` enters the address computation" at step 12.
    Resolved by making the stride a **divisor applied by the engine**:
    addresses are authored as the slave's own table gives them and stored
    verbatim, and the stride converts address units to registers in exactly
    three places. Writes need no stride arithmetic at all, which retires §7's
    `startAddr + offset` bug rather than fixing it.
21. *A dialect is a group, and one of its members was mis-designed as an
    optimisation* — `addrStride` generalised into four fields
    (`addrStride`, `writeFc`, `maxReadRegs`, `blocks[]`), so the engine has one
    code path and the capability supplies its constants. §2.9a had FC16 as an
    internal coalescing choice; the JK implements **only FC 0x03 and 0x10**, so
    the write function code is config, not cleverness — and under FC16 the
    1-register write rule lifts, which the JK's `u32` Settings fields require.
    Blocks are **authored** rather than inferred, because authored blocks
    precede their points in the stream and turn a bad address into a compile
    error instead of an exception 2 from a slave. This also pulled §7's "read
    caps" entry forward from step 13 to step 6, where the format actually moves.
22. *The API is a third event source* — the surface had no concurrency model,
    and contract 8 admitted a race rather than deciding one. The engine already
    drains one queue fed by timers and port completions, so API mutations become
    the third feed and no mutex enters the module. Two calls resist the rule for
    opposite reasons and are stated as exceptions rather than smoothed over:
    `ConfigCompile`/`Verify`/`Export` stay on the caller's task because their
    byte source is a socket and posting them would stall the engine for a whole
    upload, and `Subscribe` stays synchronous because a posted one cannot return
    "table full" — made safe instead by writing `inUse` last, which turns
    contract 8 into "subscribe at any time, including before `Modbus_Init`".
23. *A borrow ends when the module says it ended* — `Unsubscribe`'s free hazard
    resisted two cheaper fixes. Deleting the call in favour of a mutable event
    mask was rejected as a surface change to dodge a correctness question; a
    pointer-to-pointer `ctx` was rejected because observing a release does not
    help when the observation and the use are not atomic. The module already had
    the right pattern in §2.9a, where a completion callback ends the borrow of a
    requester's arrays, so the same shape ends a subscriber's: `Unsubscribe`
    posts, and a final `mbEvt_released` — the one event the `eventMask` does not
    gate — is the moment the `ctx` may be freed. The enum returns to six types,
    for a reason unrelated to the deletion that had made it five.
24. *A module that knows nothing about transports has no transport question* —
    §2.5's three unstated details and §9's "the test port has no transport" were
    one problem wearing two hats. Naming the coupling exactly — a function
    pointer down, a callback up, buffers owned by the module — makes the
    response timeout and the framing gaps the driver's by necessity, shrinks the
    per-frame parameters to `baud`/`format`/timeout, and turns "which channel
    does the test port use" into an App-layer question that blocks nothing. The
    rx-report question was answered by refusing it: a driver says how reception
    ended, never what a frame means. Buffer ownership was inverted from the
    earlier draft, which had lent them upward with no stated end.
25. *The test hook moves out rather than disappearing* — §2.5 claimed the test
    port "replaces every test hook", and it does, **for the module**. The
    fabrication itself is still wanted: slot 1 starts as an App-layer stub fed by
    `modbus inject`, so the integration suite survives the rest of the sequence
    unchanged at
    today's coverage, and a real peer is a later change to that driver's byte
    source alone. §6 records that a test peripheral never covers RTU *line*
    framing whatever feeds it.
26. *`inject` and `probe` were one hole facing two ways* — asking what
    `Modbus_Probe` was actually **for** dissolved it. Both bypassed the compiled
    config to run one caller-specified transaction, `inject` upward and probe
    downward, and both existed for integration testing. Testing keeps the upward
    half through the test peripheral and trusts the downward one, checking it
    once against real hardware; probe's remaining uses were already covered by
    §2.9a reaching a whole capability with `exc` in the reply. So probe is
    deleted rather than given the port field §9 was about to add, the API's
    Commands group falls to one call, §2.12b loses its only blocking call, and
    §2.5's "no test hooks" claim becomes true without the exception §2.9 used to
    carve out. Discovery under total uncertainty leaves the firmware and becomes
    a bench job with a USB-RS485 adapter.
27. *The deadline is the bound, the count is fail-fast* — `Modbus_Request` had
    neither capped. The asymmetry decided it: an oversized batch is already
    handled by the deadline expiring partway, which §2.9a defines per item,
    while an unbounded deadline voids the guarantee that makes borrowed caller
    memory safe at all. `timeout_ms` becomes 1…60000 with no "wait forever"
    value, capped where a request stops being a request rather than where the
    bus does — past a minute the caller is asking for a plan. `count` becomes
    1…100 in its own constant, decoupled from `MB_MAX_POINTS_TOTAL` because a
    flash ceiling and a bus-occupancy ceiling move for unrelated reasons; more
    than 100 is more requests. Two consequences fell out: validation splits by
    task because §2.12b put flash reads on the engine, and repeated ids need no
    rule because in-order execution already defines them.
28. *The exception to §2.2 was not made* — whether a request-driven read also
    emits `mbEvt_sample` had been open since the write path was designed.
    Emitting wins on the rule's own terms: §2.2 names itself as the one an
    exception will erode, and a value is a plant fact rather than an outcome, so
    the `mbEvt_writeResult` precedent does not reach it. The requester keeps
    what a broadcast cannot give — correlation with the id and result. It also
    made the MQTT bridge smaller: the set read-back publishes through the normal
    sample handler, leaving the completion callback a success/failure line, and
    the "which samples are worth publishing" question resolves to a rule §2.11
    had already implied — publish if an entity exists for that point.
29. *Polling is demand-driven, and that removed a concept rather than a rule* —
    asking whether a request reaches an offline device exposed that the module
    should not have had "offline" at all. Subscriptions now scope by **plan**
    and a plan nobody subscribes to gets no timers, so there is no traffic
    nobody asked for — as a property of how work is created, not a goal the
    scheduler pursues. That knocked out the ground under §2.15 Q5: the consumer
    that wants the data decides how hard to chase it, exactly as §2.4 assigns
    publish cadence to whoever publishes. Reversed accordingly, deleting
    `s_consecFails`, the offline bitmask, the ≥30 s throttle, §2.7's
    failure-backoff filter and `mbEvt_deviceState`; availability is computed and
    published by the MQTT bridge, where the word already meant something. The
    cost is named rather than mitigated: a slave that dies while something is
    still subscribed is retried at its plan's cadence forever, and the remedy is
    a config change — move it to a plan with no time tables (§2.6) and apply it
    hot. A semantics-free per-device cooldown was proposed to soften that and
    rejected, because the config already says it.
30. *Subscriptions key on plan, not on device position* — the mask counted
    `devices[]` positions, which made every consumer depend on config ordering:
    adding a fourth pack silently invalidated a fusion path's `0x0F`. A plan
    names one capability, so its devices are one kind of hardware read at one
    cadence — which is the unit a consumer actually has an opinion about, and
    where a capability is split across plans (`pack_fast` / `pack_lazy`) the
    split *is* the choice being offered. It also unpinned `MB_MAX_DEVICES`,
    which the `uint8_t` device mask had been holding at 8 even after ids widened
    to `uint16_t`.
31. *An item's result is a state, not a verdict* — the result was defined only
    at completion. Making every item `mbErr_pending` at acceptance, definite when
    its reply lands, and swept at the deadline gives one invariant — **the
    callback fires exactly when no item is pending** — in place of three separate
    rules for completion, timeout and config-swap abandonment. The cursor
    distinguishes *timed out* from *not attempted* for free, and they are
    different facts for a write. It also forced the exception codes into
    `eModbusErr`: `modbus_rtu.h` had kept them out on the grounds that telling
    1/2/3 apart "is the whole diagnosis", which under per-item results is the
    argument for folding them in. `exc` deleted, `sModbusReqReply` down to five
    members.
32. *`Modbus_RequestCatalogue` stays, and the reason inverted* — entry 29 made
    the bridge subscribe on broker connect, and since `Subscribe` replays the
    catalogue the call looked redundant. It is the opposite: demand-driven
    polling gave subscribing **side effects on the bus**, so re-subscribing to
    force a replay would stop polling in the gap and use the `mbEvt_released`
    teardown handshake as a query. A read-only way to ask is worth more now than
    when Subscribe was cheap. The general form is that a consumer's "I need the
    list" moment coinciding with its "I want the data" moment is a fact about
    today's bridge, not a property of the API — and the alternative to asking is
    caching what §2.2 says not to cache.
33. *The numbering said the sequence had been edited* — §2.16 carried a step
    "10a" wedged between 10 and 11, left from inserting the write path into a
    finished list, and §2's staleness table pointed at "step 11" for a write
    engine the list called 10a. Renumbered 10a → 11 with everything after it
    shifted, and the eleven cross-references chased through §2.5, §2.6, §2.15,
    §3.2, §7 and §8. §2.15 lost its claim to be ordered by §2.16 step — true
    when it held open questions, meaningless once §9 held them — and is retitled
    to what it actually is. The staleness table went with a later sweep: the
    audit moved the header so far that enumerating the differences was longer
    and less honest than saying the header is superseded outright.
34. *One array of items, not three parallel ones* — raised while deferring a
    requester-side memory question, and worth more than the tidiness it was
    offered as. Parallel `ids`/`values`/`results` made the caller's borrow three
    objects instead of one, left index alignment as an unenforced invariant,
    and — the actual defect — declared the input `const int32_t *values` while
    the module writes read-backs into it. `id` is in-only, `value` in/out and
    `result` out-only, which one `const` on one pointer cannot express and
    field-level direction can. `sModbusReqItem` packs to exactly 8 bytes, so a
    100-item batch is 800 B either way. It also deleted an argument: the
    parallel form had to justify why each array came back, where an item array
    makes the correlation structural.
35. *An error is a number* — v2 broke `{device, transaction, point}` as a
    location, because a failure inside `capabilities[0].points[2]` has no device
    index. The triple turned out to be a **path encoded in integers**, workable
    only while v1's JSON was exactly three levels deep. A path *string* was
    proposed and rejected on the principle that decided it: text in a result
    struct is paid for on every result, cannot be reworded without breaking
    every test that compares it, and makes multi-error reporting prohibitive.
    `{err, field, idx[3], section}` is 20 bytes against ~136, with
    `ModbusCfg_ErrString` / `_FieldString` / `_PathString` rendering text at the
    422 body and the CLI where a person reads it. Two enums, not one, because
    *out of range* and *unknown key* apply across ~40 fields. `eModbusCfgErr` is
    append-only for a reason the usual rule misses — it is never persisted, but
    it crosses the HTTP boundary. Four failures per upload became affordable and
    were left undesigned.
36. *11 bits was a maximum, not a margin* — the last open item looked like a
    missing sentence about a conservative floor. RTU's formats are 8E1, 8O1 and
    8N2, all 11 bits, and 8N1 is the non-conformant variant at 10 — so deriving
    gaps at 11 is correct for **every** format rather than generous for one, and
    the timing takes no format input at all. `format` then joins `baud` as a
    device parameter and an authored JSON key, spending a byte the device record
    already had spare; reserving it silently was rejected, since 8E1 is RTU's
    own default and the board would otherwise support the common variant while
    refusing the conformant one. §5.5 records the STM32 trap that makes this
    fail quietly: 8E1 is `WORDLENGTH_9B` + `PARITY_EVEN`, not `8B`.

Rejected along the way, and worth not re-proposing: mirroring the compile result
in `modbus.h`, an authored `"id"` device field, a module-side event queue,
per-subscription delivery counters, and a static per-observer slot ring (all
above); deleting `Modbus_Unsubscribe` in favour of a mutable event mask, and a
pointer-to-pointer subscriber `ctx` so the module could observe a release
(both entry 23); driver-owned frame buffers borrowed upward, picking the test
port's transport before anything needed it, and a dead-end test stub (entries
24-25); tying the request batch cap to `MB_MAX_POINTS_TOTAL` (entry 27);
parallel `ids`/`values`/`results` arrays (entry 34); a JSON-path string as the
compile-error location, and emitting bare codes with no on-device strings (both
entry 35); reserving the device record's spare byte for line format instead of
authoring it (entry 36);
a semantics-free per-device cooldown to soften demand-driven polling, and
keying subscriptions on capability rather than plan (both entries 29-30); keeping `Modbus_Probe` CLI-only, giving it an HTTP endpoint, and moving
it to an App-layer tool driving the RS485 driver directly (all entry 26); expanding maps per device
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

---

## 9. Open decisions

**None.** The 2026-08-11/12 audit opened with sixteen observations and closed
every question in §2's surface; what each answer decided, and what was rejected
reaching it, is recorded in [§2.15](#215-decisions-and-what-they-cost) and
[§8](#8-provenance) entries 19-36.

Of the sixteen, five were discarded rather than answered: they argued from
shipped v1 code against unimplemented §2 design, and this document leads the
implementation, so a divergence there is expected rather than a finding. Two more
left this list without being decided here, because they are not this document's
to decide — the MQTT bridge's per-pending request storage, which is the bridge's
own call at step 5 and works either way (§2.13), and the test peripheral's
transport, which stopped being a Modbus question the moment the module held
registered drivers instead of peripherals (§2.5).

What remains genuinely undesigned is in [§7](#7-known-limits), and it is small:
a fifth dialect fact if a third slave ever needs one, MQTT-side publish rate
policy, and sequence fairness. All three are named there as deliberate, with the
constraint any answer must satisfy.

**The next thing to do is [§2.16](#216-implementation-order) step 1**, not
another decision.

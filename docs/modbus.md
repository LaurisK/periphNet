# Modbus — the common module

**Status:** the design. Restructured 2026-08-12 so the *idea* leads, the rules
follow from it, and the shipped code appears only where neither reaches.
Nothing in §2-§8 is implemented yet; §10 is the order it lands in, and the next
action is step 1.

Single source of truth for Modbus in PeriphNet.

| Section | Read it when |
|---|---|
| [§1 The idea and the pattern](#1-the-idea-and-the-pattern) | **first, and again whenever this document is silent** |
| [§2 The boundary](#2-the-boundary) | placing a file or an include |
| [§3 The model](#3-the-model--ports-capabilities-plans-devices) | authoring or reading config concepts |
| [§4 The surface](#4-the-surface--what-a-consumer-sees) | writing a consumer |
| [§5 The engine](#5-the-engine--ports-scheduling-dispatch) | writing the module |
| [§6 Config JSON](#6-config-json-reference) | authoring a config |
| [§7 Record format](#7-record-format-v2) | touching the compiler, store or exporter |
| [§8 Operator reference](#8-operator-reference) | driving the board |
| [§9 Testing](#9-testing) | changing anything |
| [§10 Sequencing](#10-sequencing) | picking up the work |
| [§11 As-built](#11-as-built--what-is-on-the-board) | bisecting, migrating, or measuring |
| [§12 Undesigned](#12-undesigned) | planning what comes after |

Related, separate: [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md)
(why autonomy constrains this) and [pylontech_can_protocol.md](pylontech_can_protocol.md)
(a future consumer).

---

## How to read this document

**Precedence when this document is silent:**

1. **An explicit statement here.**
2. **Failing that, derivation from the pattern in [§1.2](#12-the-pattern).** The
   rules there are generative, not decorative — most silences have exactly one
   answer consistent with them, and [§1.3](#13-the-four-questions) is the
   procedure for finding it.
3. **Only when neither reaches, the shipped code** ([§11](#11-as-built--what-is-on-the-board)),
   and then as a *default to be replaced*, never as authority.

The order matters in one direction especially. The code on the board is the v1
engine this design replaces, so a gap filled from it does not complete the
design — it contradicts it. `App/Modbus/modbus.h` is **superseded**: it compiles
and it carries the original contracts as comments, but it predates the design
and is not a second source of truth.

**When you rely on a pattern derivation, write it back here as a statement.**
Derivation is inference, and two competent readers can infer differently; a
sentence added the first time an answer is needed costs nothing and stops the
second implementer re-deriving it another way.

---

## 1. The idea and the pattern

### 1.1 The idea

Poll Modbus RTU slaves described by an uploadable config, decode their
registers, and hand every reading to whoever subscribed to it. The module does
not know what MQTT is, what Home Assistant is, or what CAN is.

Its whole job, stated as narrowly as it will go:

> **Know what to read and how to read it, translate registers to values and
> values back to registers, and hand the result on.**

Nothing else. No per-consumer policy, no memory of values already seen, no
opinion about whether a slave is worth talking to.

### 1.2 The pattern

These are the rules the design is generated from. They are normative: a
question this document does not answer is answered here.

#### What the module is

- **A translator, not a participant.** The test applied to every candidate
  feature is *does the module have a basis to judge this?* Publish cadence,
  device availability, retry backoff and built-in defaults all failed it and
  left. A module that knows what to read, how to read it, and how to convert
  registers ↔ entities is small enough to test exhaustively on the host.
- **It keeps state about the wire, never about the data.** Scheduling state,
  per-timer due state, missed-event counters and one frame buffer per port are
  the module's. Previous values, last-publish times, change detection,
  consecutive-failure counts and offline flags are not, and do not reappear
  anywhere else in the module under another name.
- **The instrument replaces the remedy.** Overload is not smoothed, it is
  counted and named. A wedged consumer is not detected, it is declared the
  consumer's problem. Where the module cannot fix something without judgement it
  does not have, it reports and stops.

*Do not re-propose:* carrying publish policy through the API; a module-side
value cache "just for change detection"; per-subscription delivery counters.

#### Where facts live

- **A hardware description must not carry a deployment or transport decision.**
  This same category error was made three times and fixed the same way each
  time: publish thresholds in the register map, read periods in the register
  map, and `topicPrefix` as the module's device identity. When a field looks
  like it belongs to two things, it belongs to the one that *varies
  independently*.
- **Cardinality is the model.** Port 1:many devices, capability 1:many plans,
  plan 1:many devices, communication data 1:1. Anything shared is *referenced by
  id*, never copied per device — the config caps count records, so copying spends
  the budget on duplicates.
- **Data, never behaviour.** A device is a config, not a driver (§2.3). A
  dialect is four data fields on a capability, and the engine runs one code path
  whose constants come from them. If a future slave needs something that cannot
  be expressed as data on the capability, it is refused — a capability that may
  hold a function pointer or select a code path is `jk_bms.c` with extra steps.

*Do not re-propose:* expanding a register map per device; a per-capability code
path or driver hook; deriving a device's identity from a consumer's string.

#### Who owns the bus, the memory and the time

- **Config is the sole authority on the wire.** *Every frame the module puts on
  a wire comes from the compiled config, and every reply it decodes came back
  from one.* There is no hole in either direction — no probe, no injection point,
  no raw write. Protection therefore comes from **data**: the capability decides
  what a request item means, so a caller cannot reach a read-only register by
  asking differently.
- **A borrow ends when the module says it ended.** One memory rule, and it
  applies in both directions: a requester's item array belongs to the module
  until the completion callback fires, and a subscriber's `ctx` belongs to it
  until `mbEvt_released`. No flags, no handshake words, no check-then-call — the
  module notifies, and until it has, the memory is on loan. The module allocates
  nothing on a consumer's behalf.
- **Delivery is the call.** Dispatch is synchronous. There is no module-side
  queue, no per-subscriber buffer, no delivery-success notion and no drop
  counter: a callback that was invoked was served. The only rule imposed on a
  consumer is *must not block*, so the cheapest possible consumer stays cheap.
- **One queue, one task, no mutex.** Timers, port completions and mutating API
  calls are three sources feeding one queue that the modbus task drains. ISR and
  timer contexts only ever post. Where a call cannot follow this, it is stated as
  an exception with its reason rather than smoothed over.
- **Work exists because someone asked for it.** A subscription creates timers; a
  plan nobody subscribes to is not polled. "No traffic nobody asked for" is a
  property of how work is created, not a goal the scheduler pursues.
- **Time free-runs.** Nothing rearms on completion, so a period is a period and
  drift has nowhere to accumulate. There are no absolute-deadline computations
  and no tick-wrap comparisons in the engine. Overrun coalesces: a stacked event
  is dropped and counted.

*Do not re-propose:* a blocking module call; a module-side event queue; a
static per-consumer slot ring; a pointer-to-pointer `ctx` so the module can
observe a release; deleting `Unsubscribe` in favour of a mutable event mask; a
per-device cooldown or retry throttle; keeping `start`/`stop` so a test need not
change.

#### How things are represented

- **Derive rather than store, and never store a pure function of the stream.**
  Read blocks are derived from points, not authored and not compiled in. Ids are
  positions, not stored fields. Verify is the compile pass with a counting sink,
  not a second validator. The hazard being avoided each time is *two things that
  can disagree*.
- **Single pass, no rewind, no backpatch.** The compiler reads a stream it
  cannot rewind and holds no index, which is why everything links by dense
  authored id, why section order is forced, and why names link nothing.
- **Author the redundancy that makes silence loud.** Ids are authored even
  though position is identity, and blocks are authored even though they could be
  inferred — in both cases so that a mistake becomes a **compile error naming the
  offending object** instead of a wrong register discovered months later against
  a slave.
- **An error is a number.** Result structs carry codes; text is rendered by
  functions where a person reads it. Prose in a struct is paid for on every
  result, cannot be reworded without breaking every test that compares it, and
  makes reporting more than one failure prohibitive.

*Do not re-propose:* linking by name; byte-offset references in the stream; a
JSON-path string in the compile result; emitting bare error codes with no
on-device strings.

#### How decisions get made here

- **State the cost; do not add a knob to hide it.** A dead slave is retried at
  its plan's cadence forever. A 100-item batch can hold a 9600-baud line for
  ~12 s. A catalogue burst can show up in the missed counter. Each is named and
  left alone, because the remedy in every case is config or a consumer decision,
  which is where the judgement belongs.
- **A knob whose default is obvious is not an option.** Per-request read-back
  opt-in, availability thresholds and publish throttles were all rejected on
  this.
- **The surface freezes before the insides move**, and **the record format moves
  once**. Both exist so that a change lands in one place instead of churning
  every consumer or every stored region twice.

### 1.3 The four questions

Applied in order, these resolve most silences:

1. **Whose fact is this?** The plant's, the wire's, the deployment's, or a
   consumer's transport?
2. **Is it about the wire or about the data?** Wire state stays; data state does
   not exist.
3. **Does the module already hold it, or would keeping it be memory?** Handing
   over something already in hand is free; retaining it is not.
4. **Can it be data rather than code, derived rather than stored, and a compile
   error rather than a runtime one?**

Worked example, because the procedure is the point: *does `mbEvt_sample` carry
the point descriptor, or only ordinals?* The engine is decoding a record it has
just read from flash (Q3: already in hand), the descriptor describes config
rather than values (Q2: not data state), and the alternative forces every
consumer to cache the catalogue, which Q1 assigns to nobody. So the event
carries a **borrowed** `const sModbusPointDesc *` — which is the rule now stated
in §4.4.

---

## 2. The boundary

### 2.1 The picture

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
                                        HAL · CMSIS-OS/FreeRTOS ·
                                        Shared/Modbus · w25q128 · trice
```

### 2.2 The rules

- Files in `App/Modbus/` must not include `App/Mqtt`, `App/Http`, `App/Can`,
  `App/Data`, or any lwIP header.
- A **consumer** includes only `App/Modbus/modbus.h`. Scheduler, engine and
  framing are module-internal, and there is no way to select a port for a device
  from outside — a port is where a device lives, and that is config.
- A **peripheral driver** is the one other thing outside the module that talks to
  it, through `modbus_port.h` (§5.1). It registers itself into a port slot and
  calls back when a frame completes. It knows nothing about config, events or
  consumers, and the module knows nothing about its transport.
- Consumers may use `Shared/Modbus` **types** (`modbus_records.h` — enums, unit
  codes, bounds, the compile result). They must not call its **flash accessors**
  (`MbCfg_*`, `MbCfgStore_*`).
- `Shared/Modbus/` stays pure (libc + w25q128), host-testable, and out of
  `${SHARED_SOURCES}` so it never links into the 32 KB bootloader.
- All of the above is greppable; enforce it in CMake, in the spirit of the check
  that keeps `bootloader/secrets*.c` out of the application.

**Placement follows from the pattern, not from history.** Anything that is a
pure function of records and dialect constants — decode, format, the scaled ↔
register conversions, read-block derivation — belongs in `Shared/Modbus/` where
host tests reach it. Anything that touches a port, a timer or a subscriber is
the engine's.

### 2.3 A device is a config, not a driver

`jk_bms.c/h` is **not part of this module** and does not survive. Everything it
does is expressible without device-specific firmware:

| `jk_bms.c` does | replaced by |
|---|---|
| a hardcoded FC03 read of block `0x1400` | points at `addr: 5120…` in a capability declaring that block (§3.2) |
| ASCII decode of model / hw / sw version | `decodeType: "ascii"` points — already supported |
| its own USART2 init at 115200 | the device's `baud` parameter (§3.4) |
| byte-addressed registers | the capability's `addrStride` — data, not code |
| no FC06 handler on the slave | the capability's `writeFc` — likewise |
| "is a JK actually there?" | a capability declaring its blocks, plus `Modbus_Request` |

The JK BMS becomes a capability, on the same footing as a Solis config. That is
what makes the module worth extracting: **the next device after JK must cost a
JSON file, not a `.c` file.**

---

## 3. The model — ports, capabilities, plans, devices

### 3.1 Four things, and the cardinalities are the design

| | Cardinality | Holds | Example |
|---|---|---|---|
| **Port** | 1 : many devices | how frames get out and back | the RS485 UART; the test port |
| **Capability** | 1 : many plans | what the hardware *can do*: its **dialect**, its **blocks**, and a flat list of points | "JK PB BMS", "Solis inverter" |
| **Plan** | 1 : many devices | what we *watch*: a `capId` and a list of time tables | "fast: SOC+pack every 5 s, cells every 60 s" |
| **Time table** | 1 : 1 plan | one period and the point ids read at it | "every 5 s: points 0, 1, 4" |
| **Device** | 1 : 1 | slave address, baud, format, port, `planId`, `topicPrefix` | slave 1 @ 115200 on RS485, plan 0 |

**A device names a plan; the plan names its capability.** One reference on the
device, so a plan/capability mismatch is impossible by construction.

**Capability and plan are separate because they are facts about different
things.** A register map is a fact about the silicon and never varies; how often
to read cell voltages is a fact about this deployment. Welding them together
forces every device sharing a map to share its periods, so a spare pack could not
be polled lazily while the primary is polled hard.

**Only the communication data is per-device.** Four JK packs in parallel are four
devices, one capability, one or two plans, one port.

**An empty plan is legal**, and it is how "capable but unmonitored" is said — a
device that is only ever written through `Modbus_Request`, or one deliberately
taken out of the poll rotation without deleting it.

### 3.2 Ids, blocks and access

**Everything links by id, and every id is a dense ordinal** — `capId`, `pointId`,
`planId`, `timeTableId`, `deviceId`, all `uint16_t`, all equal to the object's
position in its array. Names never link anything: `name` is a display property
exactly like `unit` or `scale`, and `topicPrefix` is MQTT's string.

The JSON authors each `id` explicitly even though it equals the position, and the
compiler **rejects a config whose ids do not run 0, 1, 2 …**. That is the whole
point of authoring them: an insertion or deletion that would silently re-point
every reference downstream becomes a compile error naming the object where the
run breaks.

**Blocks are authored, and a read never crosses one.** A capability declares its
address ranges as `{base, regs}` pairs. A derived read may span anything *inside*
one block, including addresses no point selects, but never two. Bridging an
unselected address costs two bytes per register (~2 ms at 9600) against a whole
extra round trip (~25 ms); reaching outside a block risks the slave rejecting the
entire read with exception 2. Authoring them buys three things:

- The **block length is the read-span limit** the slave enforces. The JK's
  ceiling is `quantity + wordOffset < 147` measured from a block base — precisely
  "a read must end before register 147 of its block" — so `regs` states it and no
  separate dialect field is needed.
- **Splitting is a boundary test**, not a search for gaps in a point list.
- **The compiler validates it.** Blocks precede points within the same
  capability, so a point outside every declared block, or past its block's
  `regs`, is a compile error pointing at the offending point.

**Access is capability, and it means what the silicon supports:** `r` readable,
`w` writable, `rw` both. Two rules are enforced at compile: a **time table may
only list `r` or `rw` points** (a `w` point cannot be read, so watching it is
incoherent), and a **`w`/`rw` point must sit on a `holding` function code**,
since neither FC06 nor FC16 reaches the input space.

**Whether a writable point may span more than one register is a dialect
question.** Under FC06 a write lands exactly one register, so a `w`/`rw` point
must be a 1-register type; under FC16 it need not be. This is not hypothetical:
the JK's Settings block is `u32` fields at `0x1000`, `0x1004`, … — two registers
each — so under an FC06-only rule not one JK setpoint would be writable.

**Transactions are not authored.** A capability is a flat list of points and a
time table is a list of point ids; the **engine** derives the read blocks when it
builds a device's sequences (§5.2).

### 3.3 The dialect

Four data fields on the capability, and they are the whole of what the engine
knows about a slave:

| Field | Solis | JK PB | What the engine does with it |
|---|---|---|---|
| `addrStride` | 1 | 2 | address units per register — see below |
| `writeFc` | 6 | 16 | function code for the write half of a request |
| `maxReadRegs` | 125 | 123 | per-request quantity cap; splits a derived block |
| `blocks[]` | `{3000, 200}` | `{0x1000,147} {0x1200,147} {0x1400,147}` | which addresses exist; a read never crosses one |

Anything a third slave turns out to need must arrive the same way — as data on
this record — or be refused.

**`addrStride` is a divisor, not a multiplier.** JK registers are *byte*-
addressed: consecutive registers sit at `0x1400`, `0x1402`, `0x1404`, where a
standard slave puts them at `3000`, `3001`, `3002`. Registers are still 16-bit
and the wire's `quantity` field is still a register count — only the address
units differ. A point's `addr` is authored **exactly as the vendor's register
table gives it** and stored verbatim; the stride converts between the address
domain and the register domain in the three places where they differ:

| | |
|---|---|
| Contiguity | `next.addr == cur.addr + cur.width × addrStride` |
| Frame quantity | `(blockEnd − blockStart) / addrStride` registers |
| Decode index into the reply | `(pt.addr − blockStart) / addrStride` |

All three are the engine's. Nothing is baked in at compile time, so the stored
address is always the one that goes on the wire — and a **write needs no stride
arithmetic at all**, because the FC06/FC16 address is the authored `addr` itself.

Getting the domains backwards is the failure worth naming, because it is silent:
a 16-register JK block spans an address delta of 32, so a contiguity test, a
quantity, or a 125-register ceiling computed on raw addresses is wrong by exactly
`addrStride` and still looks plausible.

### 3.4 Line parameters are device parameters

**Baud belongs to the device↔peripheral binding, not to the bus.** One wire
serves a Solis at 9600 and a JK at 115200: the master owns every transaction, so
the line is time-multiplexed by construction, and a slave seeing traffic at the
wrong rate drops it on a framing or address mismatch exactly as it drops traffic
addressed to someone else. Because parameters travel with the frame (§5.1), a
rate change is just the next transaction's parameters.

**Line format is a device parameter for the same reason.** A slave's parity is a
fact about that slave. 8E1 is RTU's *spec* default, so a config that can say 9600
but not 8E1 supports the common variant and refuses the conformant one.

**Framing gaps derive from baud, and take no format input.** 3.5 character times
at **11 bits per character**, with the RTU floors of 1.750 ms / 750 µs above
19200 baud. 11 bits is the maximum any RTU format uses — 8E1, 8O1 and 8N2 are all
11, and 8N1 is the widespread non-conformant variant at 10 — so the derivation is
*correct for every format*, not merely safe for one. `format` configures the UART
and nothing else.

**A port is an enum, not an authored record.** `portId` on the device record
indexes the module's port table — one slot per peripheral, each holding the
module's frame buffers and whatever driver was registered into it. Ports carry no
per-deployment parameters, and a config naming a port this firmware does not have
is rejected at compile time. `eModbusPortId` lives in `modbus_records.h`, because
the compiler, the exporter and whoever calls `Modbus_PortRegister` all need the
name↔code table; **nothing outside the module chooses a port for a device.**

---

## 4. The surface — what a consumer sees

### 4.1 Shape

| Group | Calls |
|---|---|
| Lifecycle | `Modbus_Init` — that is all of it (§4.2) |
| Subscriptions | `Modbus_Subscribe` · `Unsubscribe` · `RequestCatalogue` |
| Commands | `Modbus_Request` (§4.6) — that is all of it |
| Configuration | `Modbus_ConfigVerify` · `Compile` · `Apply` · `Erase` · `Export` · `Status` |
| Diagnostics | `Modbus_Stats` · `LogStatus` · `SetMonitor`/`GetMonitor` |

There is **no** `SetBaud`/`GetBaud`, no `SetPort`/`GetPort`, no
`Start`/`Stop`/`IsRunning`, no `InjectResponse` and no `Probe`. Each was a knob
on something that is now either config or nobody's business outside the module.
`eModbusErr` lives here (it appears in events); `eModbusPortId` does not (ports
are internal to consumers, though drivers see it).

### 4.2 Lifecycle — set it and forget it

`Modbus_Init` is the entire lifecycle. It initialises the flash store, recovers
the A/B regions, **erases anything that fails validation**, and constructs
whatever devices the config describes. Timers are **not** started here — they
come and go with subscriptions — so a board that boots with a valid config and no
subscribers puts nothing on the wire.

**There is no built-in default config, and none is provisioned.** A register map
describes hardware the board may not have, so a fallback config is a guess about
the deployment, and a wrong guess polls a slave that answers nothing while
looking configured. The board is told what it is for; until then it is not for
anything.

**Zero devices is a valid, first-class state.** No devices, no timers, no bus
traffic, an empty catalogue, no HA discovery, no set-topic subscriptions.
`modbus status` and `/api/modbus/config/status` report it as **unprovisioned**
rather than as an error.

**Invalid is erased, not repaired.** A region failing magic, version or CRC is
erased at init, so "invalid" collapses to one observable state instead of a
spectrum of partially-readable ones, and the region is immediately reusable.

**Port registration is independent of `Modbus_Init` and may follow it.** A port
slot is claimed by writing the driver last, the same publish-last ordering that
makes `Subscribe` safe at any time (§4.8). A device bound to a slot with no
driver is simply **not polled** — that is what "a port with no driver is
disabled" means — and its state is visible per device in `modbus status`.

### 4.3 Subscriptions scope by plan, and a subscription is what causes polling

```c
#define MB_PLAN_ALL  0xFFu          /* MB_MAX_PLANS is 8 */

int Modbus_Subscribe(uint8_t planMask, uint32_t eventMask,
                     fModbusSubscriber cb, void *ctx);   /* handle >= 0, or mbErr_full */
int Modbus_Unsubscribe(int handle);
int Modbus_RequestCatalogue(int handle);
```

**A consumer subscribes to a kind of thing, not to a position.** A plan names one
capability, so every device on a plan is the same type of hardware read at the
same cadence — exactly the unit a consumer has an opinion about. A BMS fusion
path wants *pack data*; the operator later adds a third pack by authoring a
device that names the existing plan, and **no consumer changes**. Where a
capability is split across plans (`pack_fast` / `pack_lazy`), the split is itself
the choice being offered.

**A plan nobody subscribes to is not polled at all.** The config says what *may*
be read; a subscription says what *is* read. The engine ORs the plan masks of all
live subscriptions; a device whose plan is outside that union gets **no timers**.
What follows:

- **A consumer controls the bus by subscribing and unsubscribing.** The MQTT
  bridge can register when a broker connects and deregister when it drops.
- **Values lag a reconnect by up to one period.** The catalogue is config and
  replays immediately; samples arrive when their timers next fire.
- **"Why is this device not polled" is a question about subscribers**, so
  `modbus status` and the config status JSON report each device's polled state
  alongside its port.
- **A `Modbus_Request` is unaffected** — it names a device directly and reaches
  the wire whether or not anything subscribes to that device's plan.

Scoping is **routing, not suppression**: dispatch is a bit test, it stores
nothing, and the answer is the same for every consumer of that plan. Within its
scope a subscription gets **every read**. `eventMask` is the same kind of thing
one level up — a stateless predicate on the event *type*.

The table is 8 entries of 16 B in main SRAM.

### 4.4 Events

```c
typedef enum {
    mbEvt_sample    = 1u << 0,  /* a point was read and decoded            */
    mbEvt_pointDesc = 1u << 1,  /* catalogue entry (§4.5)                  */
    mbEvt_config    = 1u << 2,  /* a new config went live                  */
    mbEvt_txn       = 1u << 3,  /* transaction outcome — diagnostics       */
    mbEvt_released  = 1u << 4,  /* Unsubscribe complete; ctx may be freed  */
    mbEvt_all       = 0x1Fu,
} eModbusEventType;

typedef struct {
    const char *topicPrefix;   /* MQTT's display string for the device     */
    const char *name;          /* topic suffix; links nothing, not unique  */
    int32_t     writeMin, writeMax;   /* scaled-int domain; valid if WRITE */
    uint32_t    period_sec;    /* plan period for this point; 0 = unwatched*/
    uint16_t    ptOrd;         /* point id within the device's capability  */
    uint8_t     devOrd;        /* device id (position in devices[])        */
    uint8_t     decodeType;    /* eModbusDecodeType                        */
    uint8_t     unit;          /* DLMS/COSEM physical-unit code            */
    int8_t      scalePow10;    /* real value = scaled * 10^scalePow10      */
    uint8_t     flags;         /* MB_PT_READ | MB_PT_WRITE                 */
} sModbusPointDesc;

typedef struct {
    eModbusEventType type;
    uint32_t         tick;              /* HAL_GetTick() when made         */
    union {
        struct {                        /* ---- mbEvt_sample ----          */
            const sModbusPointDesc *pt;
            int32_t     value;          /* scaled integer                  */
            const char *text;           /* ascii points only, else NULL    */
        } sample;
        struct {                        /* ---- mbEvt_pointDesc ----       */
            const sModbusPointDesc *pt; /* NULL only on an empty catalogue */
            uint8_t     last;
        } desc;
        struct {                        /* ---- mbEvt_config ----          */
            sModbusConfigCounts counts;
            uint8_t     activeRegion;
        } config;
        struct {                        /* ---- mbEvt_txn ----             */
            uint16_t    timeTableId;    /* MB_TT_REQUEST for a request     */
            uint16_t    addr;           /* first wire address of the block */
            uint16_t    regs;           /* registers requested             */
            uint16_t    elapsed_ms;
            int16_t     err;            /* eModbusErr                      */
            uint8_t     devOrd, slaveAddr;
        } txn;
    } u;
} sModbusEvent;
```

**The descriptor is carried, borrowed, and never retained by the module.** The
engine holds the record it is servicing, so handing it over costs nothing and
stores nothing; the alternative — ordinals only — forces every consumer to cache
the catalogue, which no rule assigns to anyone. It dies when the callback
returns.

**Sample semantics.** Raised once per point per successful read — *not* per
change. The module keeps no previous value and does not deduplicate; a consumer
wanting change detection does it on its own terms. Only the decoded value leaves:
raw registers never do, because a second raw path would invite consumers to
re-implement decoding and get word order, scaling or ASCII subtly wrong. For
ASCII points `text` is the decoded string and `value` is **unspecified** —
nothing in the module computes one; a consumer wanting dedup hashes `text`.

**`mbEvt_txn` is keyed by `{devOrd, timeTableId}`**, which is the identity a
timer, a sequence and the missed counter all already use. A request's
transactions carry `MB_TT_REQUEST`, so a diagnostics consumer can tell scheduled
traffic from asked-for traffic without a blind spot in either.

**`mbEvt_released` is delivered regardless of `eventMask`** — the consumer asked
for teardown, not for that event.

**Two events are deliberately absent.** A write's outcome goes to the requester
that asked for it through its completion callback, not to every subscriber; and
device availability went with the concept itself (§1.2 — the module has no basis
to judge it). A subscriber wanting availability counts failed `mbEvt_txn` for the
devices it cares about.

### 4.5 The catalogue

A consumer needs the point *list* before any value arrives. HA discovery is the
forcing case: it publishes one message per point at connect time and cannot wait
for samples — a 60 s point would take a minute, and a point on a silent device
would never appear.

So the module replays a **catalogue**: a burst of `mbEvt_pointDesc`, one per
point of every device on a subscribed plan, `last = 1` on the final entry.
Delivered after `Modbus_Subscribe`, after every config swap, and on demand via
`Modbus_RequestCatalogue`. The empty catalogue is `last = 1` with `pt == NULL`.

**The on-demand call earns its place because subscribing has side effects on the
bus.** Re-subscribing to force a replay would stop polling in the gap and use the
`mbEvt_released` teardown as a query. More generally, a consumer's "I need the
point list" moment is not guaranteed to coincide with its "I want the data"
moment — that coincidence is a fact about today's bridge, not a property of the
API, and the alternative to asking is caching what §1.2 says not to cache.

**The replay always runs on the modbus task**, never inline in the caller's.
Dispatching it inline would have `mqttTask` reading config records out of flash.
A consumer that subscribes before `Modbus_Init` gets its catalogue when a config
first loads.

**The catalogue is the requestable-set source.** It carries access bits,
`writeMin`/`writeMax`, `decodeType`, `scalePow10` and the ordinals, and
deliberately no address or function code. No second enumeration API exists.

**It carries `period_sec` because capable is not the same as monitored.** A
device's catalogue is its **capability's** points — every point it can do — while
its plan decides which arrive as samples. `period_sec` is the plan's period for
that point, or **0 for a point the plan does not watch**. Without it a consumer
cannot tell the two apart: HA discovery would create a sensor for an unmonitored
point and that entity would sit unavailable forever.

**A catalogue burst is the heaviest thing the dispatcher does** — 27 points means
27 back-to-back callbacks, at config swap and at every broker reconnect. Bounded
and infrequent, and it is the consumer's call whether to copy-and-post or to act
inline.

Note that the config is walked **from flash** and never held in RAM. That
property is load-bearing and must not leak away through the API.

### 4.6 Requests — one array of items, in and out

The config already describes every accessible register, so the module publishes
that set with its data types and access, keeps addresses and function codes
inside, and the requester submits a selection.

It is `Modbus_Request`, not `Modbus_Write`, because **the config decides what
each item means**:

| Capability access | What a request does with it | `item.value` on return |
|---|---|---|
| `r` — readable | reads the current value; the supplied value is ignored | the value just read |
| `w` — writable | writes the supplied value; no read-back is possible | unchanged |
| `rw` — both | writes, **then reads the register back** | what the register actually holds |

That is the protection, and it is stronger than a caller-side rule: a requester
cannot write a read-only register by asking harder, because asking is not how the
decision is made. Read-back on `rw` is automatic, not requested — the cost is a
second round trip, and a per-request opt-in would have defaulted to "on" anyway.

**A request reaches the whole capability, not just the plan.** `ids` may name any
point the device's capability declares, monitored or not. A diagnostic register
can be read on demand without joining anyone's plan, and a setpoint can be
written without being watched.

```c
typedef struct {
    int32_t  value;    /* in: value to write · out: read or read-back      */
    uint16_t id;       /* in: pointId within the device's capability       */
    int16_t  result;   /* out: eModbusErr; mbErr_pending until decided     */
} sModbusReqItem;      /* exactly 8 bytes, no padding                      */

typedef struct {
    sModbusReqItem *items;      /* the array as submitted, now filled in   */
    uint16_t        count;
    uint8_t         devOrd;
} sModbusReqReply;

typedef void (*fModbusReqDone)(const sModbusReqReply *rep, void *ctx);

int Modbus_Request(uint8_t devOrd, sModbusReqItem *items, uint16_t count,
                   uint32_t timeout_ms, fModbusReqDone cb, void *ctx);
```

**One array, not three parallel ones.** It is one borrow rather than three; index
alignment stops being an unenforced invariant; and it removes a lie — `id` is
in-only, `value` in/out and `result` out-only, which a single `const` on a single
pointer cannot express. `int32 + uint16 + int16` packs to exactly 8 bytes.

**`item.id` is a `ptOrd`; `item.value` is in the scaled-integer domain** — the
same domain `mbEvt_sample.value` arrives in and `writeMin`/`writeMax` are
authored in. One device per call, so the whole batch belongs to one sequence on
one port at one baud.

**Every item carries a state, and the callback fires exactly when none is
pending:**

```
accepted        every item = mbErr_pending
in order        the item being serviced gets its definite result when its
                reply lands — mbErr_ok, or a reason
at the deadline the item in flight becomes mbErr_timedOut;
                the ones never started become mbErr_notAttempted
```

Those last two are genuinely different facts for a write: an in-flight write may
have landed on the slave, a never-started one certainly did not. Normal
completion, timeout and config-swap abandonment are then the same rule seen three
times.

**`eModbusErr` says *why*, including which exception.** Telling illegal-function
from illegal-address from illegal-value apart is most of the diagnosis when
bringing up an unfamiliar slave, and one global "last exception" cannot say which
item got which — so the codes fold into the per-item result. `mbErr_ok` stays 0,
so `if (items[i].result)` remains the idiom.

**Every item is attempted.** A failure on item 3 does not stop items 4-8. There
is no "how far it got" summary, because `item.result` says it per item, and no
rollback, because the wire cannot offer one. Interpreting the mix is the caller's
job. **Repeated ids are legal** — items run in order and each slot gets its own
result.

**`timeout_ms` bounds the whole request, and the module must respect it:**

> **The completion callback always fires, within `timeout_ms`, and it is the only
> moment at which the caller may free or reuse the item array.**

A timed-out request is *abandoned*, not merely reported: a response arriving
afterwards is discarded and never written into memory the caller has been told it
may reclaim. **The deadline runs from submission, not from first service** — a
request that waits behind others spends its own timeout waiting and may complete
with every item *not attempted*, which is what actually bounds the caller's
memory. `cb` runs in the modbus task under the same non-blocking rule as a
subscriber.

**A config swap completes outstanding requests; it never drops them.** A request
submitted against the retiring generation is finished immediately — remaining
items *not attempted*, `mbErr_idNotFound` on ids that no longer resolve — and
`cb` fires. It has to: "the callback always fires" is what lets a caller reclaim
its array.

**In flight: a FIFO of 8 submissions**, drained in arrival order, ~32 B each.
Depth 8 is a **fairness** bound, not a throughput one: because the deadline runs
from submission, the worst wait for the last entry is depth × maximum timeout.

| Bound | |
|---|---|
| `count` | **1…100** (`MB_REQ_MAX_ITEMS`) |
| `timeout_ms` | **1…60000**; `0` is rejected |
| FIFO | 8 entries |

`MB_REQ_MAX_ITEMS` is deliberately not tied to `MB_MAX_POINTS_TOTAL`: the point
ceiling is a flash-size question, the batch ceiling is about how long one caller
may hold a shared line. A caller needing more makes more requests. There is no
"wait forever" value, because an unbounded deadline is the same as no guarantee;
the 60 s ceiling is where a request stops being a request — anything wanting the
bus for longer is asking to be polled, and a plan is what polling is for.

**Validation splits across two tasks**, which falls out of §4.8 rather than being
invented here:

| Where | Checks | Reports |
|---|---|---|
| **Submit** (caller's task) | null pointers, `count` 1…100, `timeout_ms` 1…60000, FIFO space | the return value — `mbErr_badArg` / `mbErr_full` |
| **Service** (modbus task) | each id against the device's actual capability | `mbErr_idNotFound` on that item; every other item still runs |

**A request's reads are broadcast like any other read.** Every read and
read-back also raises `mbEvt_sample` to subscribers scoped to that device, and its
transactions raise `mbEvt_txn`. §1.2's rule admits no exception for reads somebody
asked for: an *outcome* belongs to its requester, but a register's value is a fact
about the plant. What the reply adds is **correlation** — value, id and result as
one object — which a broadcast cannot express.

### 4.7 Contracts

1. Callbacks run **in the modbus task, synchronously**.
2. They **must not block**. Stricter than it looks: samples arrive per read
   rather than per change, so a consumer's cost is multiplied by config size, not
   by how much the plant is moving.
3. **Every pointer in an event is borrowed** and dies when the callback returns.
4. A consumer cannot fail a sequence; there is no way to report back.
5. `Modbus_Request` and `Modbus_RequestCatalogue` are legal from inside a
   callback (they post; a self-post is processed after the current dispatch).
6. **Both ordinals are authored identities** — `devOrd` is the device's position
   in `devices[]`, `ptOrd` the point's position in its capability — and each
   survives a config swap for exactly as long as the author leaves that array
   order alone. A requester caching ordinals across a *reordered* config addresses
   the wrong entry, which is why `mbEvt_config` is followed by a fresh catalogue;
   under id linking that reordering is itself a compile error, so what remains is
   an author who deliberately renumbered.
7. **Borrowing runs both ways, and a borrow ends when the module says it ended**
   (§1.2) — the request array until the completion callback, the subscriber `ctx`
   until `mbEvt_released`.
8. **Subscribe at any time**, including before `Modbus_Init`.

**Dispatch:** the module calls the callback and returns. Delivery *is* the call.
The pattern for a consumer that does real work:

```
modbus task        consumer cb: allocate, copy the fields it needs,
                                post the pointer to its own queue, return
consumer task      pop pointer -> do the work on its own stack -> free
```

Allocation happens in the callback rather than from a static per-consumer ring
because this memory is used rarely and by more consumers than should each reserve
a worst case. The FreeRTOS heap is heap_4 in `.ccmheap`, which coalesces adjacent
free blocks, so cycling equal-sized copies recycles cleanly. `pvPortMalloc`
briefly suspends the scheduler — bounded, legal from the modbus task, and part of
what "must not block" covers.

**None of that is a contract.** A consumer that only counts, or emits one Trice
line, does it inline and allocates nothing. Two consequences: a wedged consumer is
invisible from `modbus status` (bus capacity is the module's business, consumer
health is not), and the MQTT bridge publishes from `mqttTask`, which keeps
`LOCK_TCPIP_CORE` off the sequence path.

### 4.8 Concurrency — the API is a third event source

Consumers live in the mqtt, http and cmd tasks; the engine lives in the modbus
task. The mechanism for meeting them already exists: timers post events and port
completions post events into one queue the modbus task drains, so **the API is the
third source on that queue.** Contract 1 becomes a consequence of how the API is
called rather than a rule callers work around, and there is **no mutex anywhere in
the module**.

**Posted — the call mutates engine state:**

| Call | Why it cannot run on the caller's task |
|---|---|
| `RequestCatalogue` | reads config records from flash *and* fires callbacks |
| `Request` | the submission FIFO **is** this queue |
| `Unsubscribe` | see the release protocol below |
| `ConfigApply` | tears down per-device timers and bumps generation counters |
| `ConfigErase` | erases a region that may be being walked |

**Runs on the caller's task — `ConfigCompile`, `ConfigVerify`, `ConfigExport`.**
These must *not* move: the byte source is the HTTP socket, the compiler is sized
for the 4 KB HTTP stack, and an upload takes seconds — posting it would stall the
engine for the whole transfer. They need no locking because the A/B invariant
(§7.3) provides it: a compile writes the region nobody is walking, and the header
is written last, so a torn upload never validates. `ConfigApply` re-checks that
the staged region is valid before committing, on the modbus task, which makes a
swap racing a half-finished upload a no-op instead of a corruption.

**Synchronous and unserialized — `Stats`, `LogStatus`, `ConfigStatus`,
`Get`/`SetMonitor`.** Word-sized counters are atomic on Cortex-M4. A stats
snapshot may be internally inconsistent, and that is accepted: nothing acts on the
relationship between two counters.

**`Subscribe` does not post, deliberately.** A posted subscribe cannot return
"table full", which is a real init-time error. Instead a brief critical section
claims a slot, the entry is filled, and **`inUse` is written last, behind a
barrier** — so the dispatcher sees either a complete entry or no entry. That is at
most 8 critical sections in the life of the system. Only the catalogue replay that
follows is posted.

**Unsubscribe: a borrow ends when the module says it ended.** Clearing `inUse`
from another task is unsafe for a reason no shared flag can fix — a dispatcher that
has already read the entry is committed to calling it, so check-then-call always
leaves a window. So release is notified:

1. `Modbus_Unsubscribe(handle)` posts and returns. Legal from inside a callback.
2. On the modbus task the module clears `inUse`. No dispatch can be in flight,
   because dispatch happens on that task.
3. The module makes one final call to the consumer's callback with
   **`mbEvt_released`**, regardless of `eventMask`.
4. **That call is the release point.** After it returns the module never calls
   again and the slot is reusable. A consumer with a static `ctx` ignores it.

**Three rules fall out of the queue:**

- **An API post must never block.** Contract 5 permits a self-post from the very
  task draining the queue; a blocking post on a full queue would deadlock. A full
  queue returns `mbErr_full` immediately.
- **A submission carries the config generation it was made against**, which is
  what makes "a config swap completes outstanding requests" implementable.
- **Queue depth is the timer count plus a few API slots**, and a post that fails
  because the queue is full is reported through the same dropped-event counter as
  a stacked timer event.

### 4.9 Configuration calls

```c
int Modbus_ConfigVerify(fModbusByteSource src, void *srcCtx,
                        sModbusCompileResult *err);
int Modbus_ConfigCompile(fModbusByteSource src, void *srcCtx,
                         sModbusCompileResult *err);
int Modbus_ConfigApply(void);
int Modbus_ConfigErase(void);
int Modbus_ConfigExport(fModbusByteSink sink, void *ctx);
int Modbus_ConfigStatus(sModbusConfigStatus *out);
```

**Verify exists because "compile is validation" conflates two things.** Not
*activating* is already true — `apply` is a separate call. Not *writing flash* is
not: an upload consumes the inactive region unconditionally, on success and on
failure, and that region holds the previous config. A fat-fingered upload destroys
the fallback while telling you it failed.

`Modbus_ConfigVerify` is the same signature, the same compiler and the same single
pass, with **the record writes going to a counting sink instead of flash**. There
is never a second validator to keep in sync.

**It is `Erase`, not `Reset`** — with no built-in default there is nothing to
reset *to*, so "reset" named the wrong operation.

`sModbusCompileResult` is the compiler's own type (§7.4), not a copy of it: the
result is **data** and lives in `modbus_records.h` where consumers may name it,
while `MbCfgCompile()` is a **flash accessor** and stays where they cannot call
it.

### 4.10 What the consumers become

```c
Modbus_Subscribe(MB_PLAN_ALL, mbEvt_all, trice_sink, NULL);

Modbus_Subscribe(MB_PLAN_ALL, mbEvt_sample | mbEvt_pointDesc |
                              mbEvt_txn | mbEvt_config,
                 mqtt_modbus_cb, NULL);     /* on broker connect */

Modbus_Subscribe(0x03, mbEvt_sample | mbEvt_txn,
                 bms_fusion_cb, NULL);      /* pack_fast + pack_lazy */
```

- `mbEvt_pointDesc` → one HA discovery message per point.
- `mbEvt_sample` → copy into an allocation, post to `mqttTask`, format and
  `MqttBridge_Publish` there **if an entity exists for that point**. No state: the
  one test is a catalogue lookup, never a judgement about the value.
- `mbEvt_txn` → the bridge's own per-device failure count, and from it the
  retained `<topicPrefix>/availability` topic. HA availability is MQTT's semantic
  and is computed where it is published.
- `mbEvt_config` → nothing to do; the catalogue that follows carries the new set.
- Inbound `<topicPrefix>/<name>/set` → resolve the name to `{devOrd, ptOrd}`
  against the map built from the catalogue, then `Modbus_Request` with
  `count = 1`. **The bridge does not publish the read-back itself** — it arrives
  as an ordinary sample, so there is one publish path and no dedupe question. The
  completion callback is only a success/failure line, so the bridge must keep its
  one-item array alive until it fires but never has to read it.

**The bridge publishes a sample if it created an entity for that point**, and
that one rule covers the awkward cases: sensors for `period_sec > 0`, number
entities for writable points regardless — so a set's read-back on an unmonitored
setpoint updates its number entity, while a diagnostic read of a point nothing
listens to is dropped rather than published to a topic with no discovery message
behind it.

`http_server.c` keeps its endpoints and calls only `Modbus_Config*`.
`cmd_parser.c` calls only `Modbus_*`, and loses its `jk` command tree.

**Device identity is `devOrd`**, the device's position in `devices[]` — not a
string, and deliberately not what a subscription scopes to. `topicPrefix` survives
as exactly what it says it is: MQTT's display string, rendered into topics, HA
`unique_id`s and the device grouping by the one consumer that needs it. The
module has no name-based entry point at all; its whole addressable surface is two
ordinals.

---

## 5. The engine — ports, scheduling, dispatch

### 5.1 The port contract

The module owns a **set of peripherals** and services them. It knows nothing about
any of them: not pins, not UARTs, not sockets, not whether a frame leaves the
board. What it holds per port is a **function pointer, its own buffers, and a
callback it exports**.

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
`modbus.h` and never see either.

- **The unit is a frame, not a byte.** `submit` means *send what is in tx, wait
  out the line's own idea of a frame boundary, then tell me how it ended*.
  End-of-frame detection — the 3.5-character silence that defines an RTU frame —
  lives in the driver, the only thing that knows the line. The engine never runs a
  character timer.
- **The response timeout is the driver's too.** It is handed down per frame and
  the driver arms it, so the engine waits for exactly one thing — a completion —
  and runs no deadline arithmetic. This is a different timeout from a request's
  `timeout_ms`, which bounds a whole batch and stays with the engine.
- **Only `baud`, `format` and the timeout travel.** Inter-frame gap and
  end-of-frame silence are derived from baud *inside* the driver. Nothing about
  expected reply length travels either — that would make the contract
  protocol-level instead of frame-level.
- **Completion is asynchronous, and the callback only posts.** `Modbus_PortDone`
  runs in whatever context the driver completes in — an ISR for a DMA UART, a
  stack thread for a socket — and does nothing but post. No decode, no dispatch.
- **A failed `submit` is reported as `mbPortDone_txFailed`, not through the
  return value.** A transaction is exactly one event, and the engine waits for
  exactly one thing; a synchronous error with no completion would break both. The
  return value is advisory only, and the engine must not treat it as the outcome.

Because completion is asynchronous, several ports genuinely overlap: the engine is
bounded by the CPU it takes to form requests and decode replies, not by the wire
speed of any one line. A second RS485 bus buys real throughput.

**The buffers are the module's, one tx and one rx per port.** Driver-owned buffers
lent upward were the alternative — a loan with no stated end, and therefore a
use-after-free waiting to be written. Module-owned buffers delete the question. A
frame is at most 256 bytes, so two ports cost about 1 KB, and it must be **main
SRAM**: a DMA driver writes into these, and CCM is CPU-only.

**The driver reports how *reception* ended, never what the frame *means*.** A
short frame and a CRC-bad frame are both `mbPortDone_frame`; the engine parses and
rejects them, so one place decides what a valid reply is. What the driver does own
is the distinction the engine cannot make: silence is `timeout`, and a **line
error is its own outcome**.

**A port with no driver registered is disabled**, and that is structural rather
than a mode — there is no `mbPort_disabled` and no port-selection API. The enum is
`mbPort_rs485 = 0` and `mbPort_test = 1`; a second real bus later is a third slot
with a third driver.

**The test port is a registered driver like any other, and it lives outside the
module.** Every module-side test hook is gone. What the harness talks to is an
App-layer driver in slot 1, which the module cannot distinguish from a UART. Its
transport is deliberately **not decided here** — the driver must deliver replies
the harness supplies; where they arrive from is an App-layer question. Today it is
the `modbus inject` CLI command; HTTP is the obvious next form. Either way it is
an App-layer change with no module, config or contract change, which is the payoff
of the module knowing nothing.

**This is the integration-test mechanism, not a stop-gap** (§9). Two consequences
worth stating rather than discovering: asserting on the request the engine *formed*
is a bonus rather than the point, and **RTU line framing is never covered by a test
peripheral**, whatever its transport — the 3.5-character silence, inter-octet
timing and DE turnaround stay hardware-only facts.

Whether a test port is present is a **configuration** question, not a build
question: a device names the port it lives on, so one image serves a bench board
and a real one.

### 5.2 Scheduling — timers, sequences, slipping

There is **no polling loop**. Scheduling is an independent, event-driven instance
that emits events at the periods the config asks for; the engine services them.
Nothing walks the config looking for work.

**One timer per device per time table, and only while something is subscribed.**
A device's plan holds some set of time tables, and the device gets one timer for
each, not one per derived read block. Creating and destroying them is what
subscribing and unsubscribing does. A timer is named `{deviceId, timeTableId}`
rather than by a period value recovered by scanning. FreeRTOS software timers are
cheap enough that there is no reason to build something else; the discipline is
that the timer callback runs in the timer service task, so like the ISR case it
**only posts an event**.

**A sequence is what one timer fires:** the derived read blocks of one device's
time table, run back to back on that device's port. Two things fall out. Every
block goes to one device, so a sequence costs **one line reconfiguration** rather
than one per block. And a pending request has an obvious injection point —
between blocks *within* a sequence — which keeps requests responsive without ever
interleaving on a half-duplex wire.

**Read blocks are derived at device construction**, by grouping a time table's
points by function code and ascending address, splitting at the capability's
`maxReadRegs` and never crossing a block boundary. They are the only per-device
derived state the engine holds; **point records themselves are re-read from
flash** when a reply is decoded, because the config is never RAM-resident and that
property must not leak away. Derivation is a pure function of records and dialect
constants, so it lives in `Shared/Modbus/` and is host-tested (§2.2, §9).

**A request batch runs whole, at one injection point.** Splitting it across
openings would break "one sequence, one port, one baud" and complicate abandoning
it on timeout. The cost is stated rather than hidden: an 8-item batch is 8 round
trips, ~0.5 s at 9600, and that time comes out of the device whose sequence it
interrupted — so a large batch on a short period can show in the missed counter.
That is the counter doing its job. It is bounded: `MB_REQ_MAX_ITEMS` and the 60 s
timeout ceiling cap a single request at about 12 s of a 9600-baud line.

**Timers free-run; service time never feeds back.** Nothing rearms a timer on
completion, so a period is a period and drift has nowhere to accumulate.

**Stacking is dropped and counted.** If the engine is handed a device's 5 s event
while its previous one is unserviced, the new one is dropped and recorded; that
device's 60 s table is a different identity and is unaffected. This is coalescing
with an observable, and it keeps the queue small.

**The missed counter is a capacity signal.** A non-zero count means the config is
asking for more than the wire can deliver — exactly the fact that says "this line
needs splitting". Per-timer granularity says *which* read is starving. Exposed
through `Modbus_Stats`, the config status JSON, and one Trice line on first
occurrence.

**There is no failure backoff, and no retries at any level.** The module does not
decide that a slave is dead, so it has no state to filter on, and it never repeats
a read block or a request item on its own initiative. The cost is stated: **a slave
that stops answering while something is still subscribed is retried at its plan's
cadence indefinitely**, spending one response timeout per attempt — about 20 % of a
line at a 5 s period with a 1 s timeout. Unsubscribing fixes it when a whole plan
is dead. One dead device among healthy siblings is a **config** change: move it to
a plan with no time tables and apply it hot.

**Config and device lifecycle drive timers directly.** Setting a device's
parameters starts its process; destroying a device, changing its port, or swapping
the config kills its timers. Stop the timers, let anything in flight land,
construct the new devices. A completion arriving for a device being torn down is
discarded on a per-device generation counter — no waiting, no locks.

**Not staggered, and first-come-first-served.** All of a device's timers start at
device init, so everything is due at once at startup and stays phase-locked;
sequences due together are serviced in arrival order. Both are accepted: they are
latency effects on a line that serialises anyway, and they are visible through the
missed counter if they ever stop being acceptable.

### 5.3 Decode and encode

**Float never propagates.** A `float32_*` point is decoded as IEEE754 at the wire
and immediately quantized into the same scaled-integer domain as everything else.
Nothing downstream — event, formatting, consumer comparison — ever sees a float.

**Formatting stays in the module's world; throttling does not.** Turning a
register into `-1234` or `23.7` is translation, so `MbFormat_Scaled` and the
bitfield-as-decimal rule are the single correct answer and stay in
`Shared/Modbus` for consumers to call. Deciding whether that string is worth
sending is judgement, and belongs to whoever sends it.

**Encoding is decode's inverse and lives beside it.** A write converts a scaled
`int32` back into 1..N registers per `decodeType`, using the same word-order rules
decode uses, and the count must match what `writeFc` permits (§3.2). A value that
does not fit the point's register width is rejected before a frame is formed
(`mbErr_badArg` on that item); the module does not truncate silently.

---

## 6. Config JSON reference

Three arrays, in dependency order: **capabilities** describe hardware, **plans**
describe what to watch, **devices** bind a plan to a slave. Everything links by
`id`, never by name.

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

- **`id`, everywhere** — every object carries one and it must equal its position:
  `0, 1, 2 …`, no gaps, no reordering. Authored rather than inferred so that an
  insertion or deletion fails loudly instead of silently re-pointing every
  reference after it.
- **Capability** — `id`, `name` (≤15 chars, display only), the dialect
  (`addrStride`, `writeFc`, `maxReadRegs`), `blocks[]`, `points[]`. It describes
  what the hardware can do and contains **no periods**.
- **Block** — `{ base, regs }`: the first wire address of a range the slave
  implements, and its length **in registers**. Every point must fall inside one, a
  derived read never crosses one, and `regs` is the read-span limit the slave
  enforces. One block covering the whole map is the normal case; the JK needs
  three.
- **Point** — `id`, `addr` (the wire address exactly as the slave's register table
  gives it), `fc` (`"holding"`/`"input"`), `decodeType` (`u16` `s16` `u32_be`
  `u32_le` `s32_be` `s32_le` `float32_be` `float32_le` `bitfield` `ascii`),
  `scale` (**exact power of ten**, 0.001…1000), `unit` (`""` `V` `A` `W` `VA`
  `var` `Hz` `Wh` `kWh` `varh` `VAh` `%` `Ah` `C` `min` `s`), `name` (≤23 chars —
  the MQTT topic suffix, and **only** that: it links nothing and need not be
  unique), `length` (registers, ASCII only), `access`, `writeMin`/`writeMax`.
  **No `offset` and no enclosing transaction.**
- **`access`** — `"r"` (default), `"w"`, `"rw"`. It states what the **silicon
  supports**, not what this deployment does with it. `"w"`/`"rw"` require
  `fc: "holding"` and, under `writeFc: 6`, a 1-register type. A `"w"` point may
  not appear in a time table.
- **Plan** — `id`, `name` (≤15 chars, display only), `capability` (the `capId` its
  point ids index into), `timeTables[]`. **`timeTables` may be empty.**
- **Time table** — `id`, `everySec` (≥1), `points[]` of point ids into the plan's
  capability. A point may appear in only one time table of a plan.
- **Device** — `id`, `slaveAddr` (1-247), `baud` (optional, default 9600; one of
  1200 / 2400 / 4800 / 9600 / 19200 / 38400 / 57600 / 115200), `format` (optional,
  default `"8N1"`; one of `8N1` `8E1` `8O1` `8N2`), `port` (optional, default
  `"rs485"`; one of the ports this firmware was built with), `plan` (a `planId`),
  `topicPrefix` (≤15 chars of `[A-Za-z0-9_-]`; the MQTT namespace and HA device
  identity — **not** the module's identity, which is `id`).

**Sharing is the normal case, not an option.** Four battery packs are four
devices, one capability, one or two plans:

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

The spare pack watches one register every 5 minutes off the **same** capability.
The JK capability is also what all four dialect fields are for at once:
byte-addressed registers, a slave with no FC06 handler, a 123-register quantity
ceiling, and three address blocks whose 147-register length is the read-span limit
the BMS enforces. None of it is a code path.

**There is no `publish` object.** How often a value is worth forwarding belongs to
the consumer forwarding it; a config carrying `"publish"` is rejected as an
unknown key rather than silently ignored, which is the useful failure mode. Every
monitored point is read at its plan period and emitted every time.

**`writeMin`/`writeMax` are authored in the scaled-integer domain**, which is the
raw register domain — not in display units. This is the most common authoring
mistake. They are `int32_t`, so the full `u16` range and 32-bit setpoints are
expressible.

**Bounds:** ≤8 devices, ≤8 capabilities, ≤8 plans, **≤384 points total across
capabilities**, ≤384 time-table entries total, ≤8 blocks per capability, ≤64
derived read blocks per device, ≤125 registers per read. **The caps count distinct
records, not instances** — four packs on a 50-point capability cost 50 points, not
200. Instances govern only what a catalogue burst and HA discovery produce, which
costs flash reads and MQTT messages, not RAM.

The point ceiling is a **flash** limit: a point costs its 40 bytes of stream and
nothing in RAM, and 384 is about what a 16 KB region holds. Device count is the
exception — pinned at 8 by the `uint8_t` subscription mask.

**ASCII and bitfield semantics, frozen:** an ASCII point emits its decoded string
on every read; a bitfield emits the raw `u16` formatted as unsigned decimal;
neither gets an HA `device_class` or unit.

---

## 7. Record format (v2)

### 7.1 The stream

Flat, sentinel-terminated, no stored pointers or offsets — a linear scan is
self-describing, and nothing is ever RAM-resident.

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
    uint8_t  addrStride;      /* address units per register (§3.3)         */
    uint8_t  writeFc;         /* 6 = FC06, 16 = FC16 — a dialect fact      */
    uint16_t maxReadRegs;     /* per-request quantity cap; 0 = 125         */
    uint8_t  blockCount;      /* sModbusBlockRecord × blockCount follow    */
    uint8_t  reserved;
} sModbusCapabilityRecord;                                    /* 22 bytes */

typedef struct __attribute__((packed)) {
    uint16_t base;            /* first wire address of the block           */
    uint16_t regs;            /* length in REGISTERS, not address units    */
} sModbusBlockRecord;                                          /* 4 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  decodeType;      /* eModbusDecodeType; 0 = end-of-points      */
    uint8_t  flags;           /* MB_PT_READ | MB_PT_WRITE  (access, §3.2)  */
    uint8_t  functionCode;    /* holding(3) | input(4) — per point         */
    uint8_t  length;          /* ASCII register length; unused otherwise   */
    uint16_t addr;            /* wire address, verbatim as authored        */
    int8_t   scalePow10;
    uint8_t  unit;            /* DLMS/COSEM physical-unit code             */
    int32_t  writeMin, writeMax;   /* scaled-int domain                    */
    char     name[24];        /* MQTT topic suffix, NUL-terminated        */
} sModbusPointRecord;                                         /* 40 bytes */

typedef struct __attribute__((packed)) {
    char     name[16];        /* display only; name[0] == 0 = end-of-plans */
    uint16_t capId;           /* the capability its point ids index into   */
} sModbusPlanRecord;                                          /* 18 bytes */

typedef struct __attribute__((packed)) {
    uint32_t period_sec;      /* ≥1; uint32 so 24 h is expressible         */
    uint16_t entryCount;      /* 0 = end-of-time-tables; uint16 pointIds
                                 follow this record, entryCount of them    */
} sModbusTimeTableRecord;                                      /* 6 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  slaveAddr;       /* 1-247; 0 = end-of-devices                 */
    uint8_t  baudCode;        /* rate-table index; 0 = 9600                */
    uint8_t  portId;          /* eModbusPortId — index into the port table */
    uint8_t  format;          /* eModbusLineFormat; 0 = 8N1                */
    uint16_t planId;          /* index into the plan section               */
    char     topicPrefix[16];
} sModbusDeviceRecord;                                        /* 22 bytes */
```

Sentinels are full-size all-zero records, so readers always consume whole records.
`slaveAddr` 0 is the Modbus broadcast address (never a real slave), `decodeType` 0
is reserved, and `name[0] == 0` cannot be a legal display name.

Each 16 KB region starts with a 256-byte header page,
`sModbusLutHeader{magic "MBCF", version, streamLen, crc32}` — **written last** by
the compiler, so a torn upload never yields a valid region.

### 7.2 Why it is shaped this way

**No record stores its own id.** Ids are dense ordinals equal to array position,
and a linear scan over a self-describing stream already knows the position of what
it is reading. The `"id"` in the JSON is an authoring assertion the compiler
checks, not a field it stores.

**Sentinel or count, decided by one rule: does anything address it?** A
capability, point, plan, time table and device are all reachable by id, so they
are records and a reader must consume whole ones — sentinel. A block base and a
time table's point id are *payload*: nothing refers to "block 2 of capability 1",
so they are plain arrays behind a count on the record that owns them. This also
sidesteps a sentinel that could not exist — under dense numbering `pointId 0` is
legal.

**A time table nests inside its plan** because the engine already treats it as one
object: a timer, a sequence and a missed counter are all per (device, time table).

**`functionCode` sits on the point** because there is no transaction record left
to hold it, and it was always a property of the register anyway — holding and
input are different address spaces. It also puts the `w`/`rw`-requires-holding
rule next to the field it constrains.

**`addr` is authored verbatim.** Offsets existed to be relative to a transaction's
`startAddr`; with transactions derived there is nothing to be relative to.
`addrStride` is applied by the engine, never baked in here.

**References are ids, never names and never byte offsets.** That keeps the
compiler **single-pass with no backpatching and no index**: it emits capabilities
while counting their points, then plans — checking each `capId` against the
capability count and each `pointId` against that capability's point count — then
devices, checking `planId` the same way. **Section order is forced by the single
pass**: each section references only what precedes it.

**Read blocks are not stored.** They are a pure function of records already in the
stream, so caching them inside the stream would only create two things that can
disagree.

**`baudCode` and `format` are persisted enum indices, so both tables are
append-only.** `baudCode` 0 is 9600 (the default, so an omitted key costs no
special case); the remaining entries are assigned in ascending rate order and a
new rate is appended, never inserted.

### 7.3 Flash layout

```
0x000F_9000  Modbus LUT A   16 KB  ─┐ roles, not fixed addresses: the selector
0x000F_D000  Modbus LUT B   16 KB  ─┘ says which is active; uploads compile
0x0010_1000  Selector        4 KB    into the inactive one; apply flips it
```

The selector follows the `boot_status.c` NOR pattern: CRC'd header
`{magic "MBSL", version, activeRegion, header_crc32}` plus a flags word
**outside** the CRC so `swap_pending` can be bit-cleared without an erase.
Recovery from a blank or corrupt selector prefers whichever region holds a valid
config.

**Invariant: a region is never erased while it may still be walked.** The retiring
region is only overwritten by the *next* upload, so a reader that races a swap
still sees coherent (old) data. The corollary is what motivates verify (§4.9): the
retiring region *is* the previous config, and any upload — successful or not —
destroys it.

Raising the point ceiling further is a flash-map move, not a constant change: the
LUT regions cannot grow in place, because WG Time sits immediately above the
selector at 0x102000.

### 7.4 Compiler and the compile result

One streaming pass, hand-rolled pull parser over a byte-source callback (no heap,
~1.3 KB of static state in CCM, runs on the 4 KB HTTP task stack). **The compile
pass is the validation**, and verify is that same pass with the record writes
discarded.

Derivation: register width per `decodeType`; `scalePow10` = log10 of the JSON
`scale` (rejected unless an exact power of ten); `unit` = DLMS table lookup.

Rejection covers: the bounds of §6; ids that do not run `0, 1, 2 …` within an
array; a `capId`, `planId` or `pointId` at or above the count of the section it
indexes; a point outside every declared block of its capability or ending past
that block's `regs`; unknown keys (including `publish`, which is how an old config
fails by name rather than silently losing a setting); unknown units;
non-power-of-ten scales; `writeMin`/`writeMax` without write access or overflowing
`int32`; `length` outside ASCII; name and prefix lengths and charset; `slaveAddr`
1-247; `fc` ∈ {holding, input}; `access` outside `r`/`w`/`rw`; `w`/`rw` on a
non-holding point, and under `writeFc: 6` on a multi-register type; a `w` point
listed in a time table; periods < 1; a `baud` outside the rate table; a `format`
outside `8N1`/`8E1`/`8O1`/`8N2`; a `port` this firmware does not have.

Because section order is forced, every count the compiler needs for a range check
is a running total it already holds.

**The compile result carries no text at all:**

```c
typedef struct {
    sModbusConfigCounts counts;  /* what compiled before the failure      */
    uint16_t err;                /* eModbusCfgErr;   0 = none             */
    uint16_t field;              /* eModbusCfgField; 0 = not a field      */
    int16_t  idx[3];             /* -1 = n/a; meaning given by section    */
    uint8_t  section;            /* eModbusCfgSection                     */
    uint8_t  ok;                 /* == (err == 0)                         */
} sModbusCompileResult;          /* 20 bytes, no padding                  */

const char *ModbusCfg_ErrString(uint16_t err);
const char *ModbusCfg_FieldString(uint16_t field);
int         ModbusCfg_PathString(const sModbusCompileResult *r,
                                 char *out, uint32_t size);
```

`sModbusConfigCounts` is `{capabilities, plans, devices, points}`.
`ModbusCfg_PathString` is what makes `idx[3]` legible: `section` says what the
indices mean and the renderer assembles them with `field` as the last segment —
`capabilities[0].points[2].scale`, `plans[1].timeTables[0].everySec`,
`devices[2].slaveAddr`, or `plans[1].timeTables[0].points[3]` where the leaf is a
bare id rather than a key.

**`field` stays a separate enum from `err`** because they compose: *out of range*,
*unknown key* and *missing required key* apply across ~40 fields, and folding them
would duplicate every reason. **`eModbusCfgErr` is append-only** — not because it
is persisted (it is not), but because it crosses the HTTP boundary into a body a
harness pins.

The response carries the code and the rendered forms together, so a harness
asserts on the number and an operator reads the sentence:

```json
{"err":12,"message":"scale must be an exact power of ten",
 "path":"capabilities[0].points[2].scale"}
```

The strings cost ~2.2 KB of `.rodata`. Emitting bare codes and mapping them
host-side was rejected: a `curl` that returns `{"err":12}` is not the useful 422
this exists to produce.

### 7.5 Units

`unit` is the `uint8` DLMS/COSEM physical-unit code from IEC 62056-6-2 — the same
`{scaler, unit}` shape as this design's `{scalePow10, unit}`. One table serves
both directions (string→code, code→(unit, HA device_class, state_class)), which is
what makes the export round trip exact.

**kWh has a private code (130).** COSEM has no distinct kWh code (kWh = Wh plus a
scaler), but `scalePow10` here places the decimal point rather than converting
units, and export/HA must round-trip kWh distinctly from Wh. Codes 73..252 are
reserved by the spec; 130 is documented in `modbus_units.h`.

---

## 8. Operator reference

### 8.1 HTTP

```bash
# Validate only — no flash written, the previous config survives (§4.9)
curl -X POST --data-binary @my_config.json \
  http://10.42.0.203/api/modbus/config/verify

# Upload → compiles into the INACTIVE region; compile IS validation
curl -X POST --data-binary @my_config.json \
  http://10.42.0.203/api/modbus/config/upload
# on error: 422 {"err":12,"message":"scale must be an exact power of ten",
#                "path":"capabilities[0].points[2].scale"}

curl -X POST http://10.42.0.203/api/modbus/config/apply     # hot swap, no reboot
curl      http://10.42.0.203/api/modbus/config/status
curl      http://10.42.0.203/api/modbus/config/download -o modbus_config.json
curl -X DELETE http://10.42.0.203/api/modbus/config          # erase -> unprovisioned
```

Uploads are refused with 409 while an apply is pending. The uploaded JSON is not
retained — download regenerates it from the records (field order and whitespace
may differ; recompiling a download yields a byte-identical config).

### 8.2 CLI

| Command | Effect |
|---|---|
| `modbus read` / `modbus status` | engine + config status (**`unprovisioned`** when no valid config), per-device port, polled state, failure and missed-event counters |
| `modbus monitor <on\|off>` | raw TX/RX frame dump via Trice |
| `modbus dump <on\|off>` | the Trice subscriber — every decoded reading |
| `modbus inject …` | byte source of the **test peripheral driver** in port slot 1 |

**No command touches the bus.** There is no `modbus write` (no raw write path), no
`modbus probe` (no direct bus access at all), no `modbus start`/`stop` (no such
lifecycle), no `modbus set baud` (a device parameter), no `modbus port` (a device
lives on a port; that is config), and no `jk` tree. Reading a register that is in
no plan is a `Modbus_Request` against a capability that declares it; reading one
that is in no *capability* is a bench job with a USB-RS485 adapter.

`modbus inject` is **not** a hole beneath the port — it feeds an App-layer driver
the module cannot distinguish from a UART. Feeding replies in through a port *is*
the integration-test mechanism.

> **Out of scope here.** The HTTP API will want the same feed — tests driving the
> test peripheral over HTTP rather than a CLI line. That is an `http_server.c`
> design question.

### 8.3 MQTT topics

| Topic | Content |
|---|---|
| `<topicPrefix>/<name>` | point value, plain text, retained, QoS 0 |
| `<topicPrefix>/<name>/set` | inbound writes for points with `w`/`rw` access |
| `<topicPrefix>/availability` | retained `online`/`offline` per device |
| `<bridgePrefix>/status` | retained bridge-wide LWT |

HA discovery: `homeassistant/sensor/<topicPrefix>/<name>/config` per **monitored**
point (`period_sec > 0`), plus
`homeassistant/number/<topicPrefix>/<name>_set/config` for `w`/`rw` points
(`command_topic`, min/max from the write bounds, step from the scale). Entities
carry an `availability` array (bridge status AND device availability, mode `all`).
Discovery and subscriptions re-run automatically after a config swap.

**`/availability` is deliberately not an LWT** — an LWT is a property of the one
TCP session for the whole bridge and cannot express "this slave stopped answering
while the bridge is fine". The suffix is `/availability` rather than `/status`
because a device identity can equal the bridge prefix, which would collide with
the bridge-wide LWT topic; it also matches HA's `availability_topic` convention.
**A device answering exceptions is answering**, so exception replies must not
count towards being offline.

### 8.4 Trice landmarks

```
Modbus: unprovisioned (no valid config)
Modbus: started                            <- at Modbus_Init, not on command
MQTT: device online: periphnet             <- bridge-derived
Modbus config: staged 1 capability 1 plan 1 device 29 points
Modbus: config swapped, active region 1
MQTT: device offline: periphnet            <- bridge-derived
Modbus: missed periphnet 5s (n=1)          <- sequence still running when due again
MQTT: set periphnet/max_charge_soc = 95 -> req 1 item
```

The missed line fires on **first** occurrence per timer, not every time — the
running count belongs in `modbus status`, not in the log. It prints the period for
a human, but the counter keys on `{deviceId, timeTableId}`.

### 8.5 RS485 timing

All of it lives in the driver, which is the only layer that knows the line, driven
by parameters handed down with each frame: `max(3.5 char times, 1.75 ms)` for the
inter-frame gap, the same floor rule for end-of-frame silence, character = 11 bits
(§3.4). The 1.75 ms floor above 19200 baud keeps every value ≥2 ms, so millisecond
granularity suffices at 115200. The engine never runs a character timer.

On 8N1 every gap comes out about 10 % longer than strictly needed, which is safe
in both directions: the master owns the bus and there is exactly one reply per
request, so a longer silence can never split a frame or merge two.

**Implementation note, because it fails quietly.** On STM32, **8E1 is
`UART_WORDLENGTH_9B` + `UART_PARITY_EVEN`**. The HAL counts the parity bit inside
the word length, so `WORDLENGTH_8B` + `PARITY_EVEN` gives **7 data bits plus
parity** — a device configured for 8E1 then decodes some bytes correctly and
mangles the rest, which reads like a wiring fault rather than a configuration one.

---

## 9. Testing

### Host (`tests/`, NOR-faithful flash mock) — where most correctness lives

```bash
cmake -B tests/build -S tests && cmake --build tests/build -j8
ctest --test-dir tests/build --output-on-failure
```

Covers the store/selector/cursor, the compiler accept+reject matrix, the export
round trip, decode and **encode** vectors, and units. **The v2 record change lands
here first** (§10 step 6), before any firmware moves: LUT version bump; the
capability, block, plan and time-table sections; **id linking** — a broken run, an
id past its section's count, and a forward reference, all rejected;
`baud`/`format`/`port`/dialect accept+reject; a point outside every declared block
or past its block's `regs`, rejected; **derived read blocks** (contiguity in the
register domain under `addrStride`, the `maxReadRegs` ceiling, never crossing a
block boundary); an empty plan; a time table listing a `w` point; a device naming a
nonexistent plan or unknown port; `publish` rejected as an unknown key; `access`
accept+reject; `int32` write bounds round-tripping past ±32767; and the export
round trip over every new field, since a field the exporter forgets is invisible
until someone downloads a config and re-uploads it.

**Reject cases assert on `res.err`, not on prose** — a code survives rewording,
a `strcmp` against a reason does not.

The stride cases want a **JK-shaped vector specifically**: `addrStride: 2` is where
a contiguity or quantity computed in the address domain passes every stride-1 test
and is wrong by a factor of two.

**Read-block derivation and the scaled ↔ register conversions are host-tested**,
which is why §2.2 places them in `Shared/Modbus/`: they are pure functions of
records and dialect constants, and the alternative — reaching into `App/Modbus/`
from the test build — would make an engine internal a test dependency.

### Integration (`tests/integration/`, live board)

Host-side C++ harness driving a board over USB/UART/UDP. Cases live in
`tests/integration/src/core/ModbusTests.cpp` and `MqttTests.cpp`, which are the
source of truth for what is asserted. `modbus_hw_*` and `mqtt_hw_*` — anything
needing a real slave or a real broker — are registered as immediate-skips, so the
rest is CI-runnable with no bus and no broker.

**The suite is not rebuilt — it is re-pointed, at step 9.** It already does the
right thing: fabricate a reply, then assert on what comes up out of the module.
What changes is only where the reply enters.

**Integration testing is upward-only, deliberately.** What the suite proves is
that a reply arriving at a port becomes the right decoded value, the right event,
the right MQTT topic and the right scheduling behaviour. What it does *not* prove
is that the frame the engine sent downward was correct — that is trusted, and
checked once against real hardware at the point the module is first trusted. Two
things follow: **timeout and malformed-frame paths become reachable without
hardware** (the driver simply does not answer, or answers badly) with the harness
controlling *when* a reply lands; and a richer driver could hand the request up
too, which is a bonus rather than the reason the mechanism exists.

**RTU line framing is never covered by a test peripheral**, whatever transport
feeds it.

| Event | String |
|---|---|
| Monitor on/off | `Modbus monitor: on` / `Modbus monitor: off` |
| TX / RX frame (monitor) | `Modbus TX[N]: <hex>` / `Modbus RX[N]: <hex>` |
| Start | `Modbus: started` |
| Device up / down | `MQTT: device online: <id>` / `MQTT: device offline: <id>` — **bridge-emitted** |
| Missed sequence | `Modbus: missed <id> <period> (n=N)` |
| MQTT pub / sub (monitor) | `MQTT pub: <topic> = <value>` / `MQTT sub: …` |
| MQTT inject ack | `MQTT inject: <topic>` |
| Request submitted / rejected | `MQTT: set <topic> = <value>` / `MQTT: set <topic> rejected` — emitted by the requester |

MQTT monitor lines are emitted even with no broker connected, otherwise the bridge
is unobservable in broker-less CI.

Three caveats worth stating rather than discovering:

- **Step 5 changes which task emits `MQTT pub:`, and therefore its ordering.**
  Content, topics, payloads and retain flags are unchanged, but any assertion that
  depends on interleaving rather than on the lines themselves will break there, and
  that is expected.
- **A test peripheral is a config binding, not a build flag.** The same image
  serves a bench board and a real one — which also means a config binding a real
  device to the test port makes that device answer when the real one would not.
  `modbus status` shows each device's port and whether it is polled.
- **Deleting the publish policy is behaviour-neutral only for configs that
  authored none.** A config that did author thresholds publishes more often
  afterwards. Deliberate feature removal, not a regression.

---

## 10. Sequencing

The engine rewrite and the API extraction are separable, and the API goes first so
that consumers stop moving while the insides change. The visible milestone is
deliberately small: **a Trice subscriber that dumps every decoded reading**,
proving data flows out through the API with no MQTT involved.

1. **`modbus.h` as a thin facade** over the existing `ModbusWalker_*`, `Modbus_*`
   (rtu) and `MbCfg*` calls. Carries the type move: `sMbCompileResult` and
   `sMbCfgCounts` become `sModbusCompileResult` / `sModbusConfigCounts` in
   `modbus_records.h` (a rename over the compiler, the store, `http_server.c`,
   `modbus_walker.c` and three host tests; shapes unchanged, so `ctest` is the
   whole check). `Modbus_Request`'s **bounds land with its shape** — `count`
   1…100, `timeout_ms` 1…60000 with 0 rejected — so no consumer is ever written
   against an unbounded version, even while the facade satisfies it with
   `count = 1` over the existing single slot.
2. **Subscription table + dispatch** — 8 × 16 B in main SRAM, scoped by **plan**
   mask, dispatched synchronously. The event structs of §4.4 land here, and this
   is the step everything from 3 onwards is written against. `publish_point()`
   stops calling `MqttBridge_Publish` and raises an event for every decoded point.
   Demand-driven polling lands here too — under the step-1 facade that means the
   walker skips devices nothing subscribes to. Carries the concurrency model:
   `Subscribe` claims its slot in a critical section writing `inUse` last,
   `Unsubscribe` posts and completes with `mbEvt_released`. The facade's posted
   calls land on whatever queue the walker has until step 10 builds the real one —
   the *contract* is what must be right here.
3. **`modbus_trice_sink.c`** — the first subscriber, plus `modbus dump on|off`.
   *The visible milestone: readings in Trice with no MQTT in the picture.*
4. **Catalogue replay** — on subscribe, on config swap, on request.
5. **MQTT bridge becomes a subscriber** — its callback copies and posts to
   `mqttTask`, which formats and publishes there; HA discovery onto the catalogue;
   set-topic writes onto `Modbus_Request` with names resolved from the catalogue,
   which is what deletes `MbCfg_FindWritablePoint()`. `mqtt_bridge.c` drops every
   `Shared/Modbus` include and its `MbCfgStore_ActiveBase()` poll. **This is where
   publishing leaves the modbus task**, so it is also where `s_topic`/`s_payload`
   stop being touched by two tasks.
6. **The v2 record format, in one move** (§7). Everything the on-flash layout will
   ever need lands here: `publish` out of the point record and the schema; the
   capability, block, plan and time-table sections; the transaction record
   deleted; `portId`/`planId`/`baudCode`/`format` on the device record; periods
   widened to `uint32_t`; `writeMin`/`writeMax` widened to `int32_t` and `fc`
   moved onto the point; the four dialect fields; id linking with its run check;
   the point ceiling raised to 384; `MODBUS_LUT_VERSION` → 2. The compile result
   goes numeric with it. Compiler, exporter, store cursor and host-test vectors
   move with it; the walker gains capability/plan resolution and derived read
   blocks and loses its tracking arrays. **The new fields are stored, exported and
   validated here but not yet acted on** — one baud, one port and stride 1 still,
   so behaviour is unchanged. **The built-in default dies here**:
   `modbus_default_config.c/h` and `ModbusConfig_EnsureDefault()` deleted,
   `Modbus_ConfigReset` becomes `Modbus_ConfigErase`, invalid regions erased at
   init, *unprovisioned* a reported state, ~3.25 KB of `.rodata` returned. This is
   the step that invalidates every region, so the change deciding what happens next
   arrives with it rather than after it.
7. **`Modbus_ConfigVerify`** + `POST /api/modbus/config/verify`.

   *Everything above holds behaviour constant. Everything below changes the
   engine.*

8. **The port contract** (§5.1) — `modbus_port.h`, `Modbus_PortRegister` /
   `Modbus_PortDone`, module-owned frame buffers in main SRAM, the four completion
   outcomes. The RS485 driver first, behind the existing synchronous engine, so
   the contract is proven before anything depends on its asynchrony.
   `Modbus_InjectResponse`, `mbPort_disabled`, `mbPort_uart6` and the
   port-selection call are deleted here.
9. **The test peripheral** — an App-layer driver registered into slot 1, **outside
   `App/Modbus/`**, whose byte source is the existing `modbus inject` command. The
   integration suite keeps working unchanged. `Modbus_Probe`, `modbus probe` and
   `modbus port` are deleted here.
10. **Event-driven scheduling** — per-device timers, sequences, drop-and-count,
    per-device teardown on config swap, and timers created and destroyed by
    subscription. The 100 ms tick, the traversal and `s_lastPollTick` go with it.
    **Measure CCM across this step**: the timers come off the 48 KB `.ccmheap`.
11. **The write path becomes real** (§4.6). The *shape* landed at step 1; the
    engine lands here: batch execution, per-item results, the submission FIFO, the
    scaled ↔ register conversions, and the address taken verbatim so a
    byte-addressed slave is written where it is read.
12. **Act on the device model** — `baudCode` and `format` drive the line
    parameters handed down with each frame (and `format` is where §8.5's
    `WORDLENGTH_9B` note earns its keep), `portId` selects the port, and several
    devices share one capability. The dialect group starts being obeyed:
    `addrStride` divides in all three places, `writeFc` selects the write frame,
    `maxReadRegs` plus the block bounds split derived reads. No record change —
    step 6 already wrote the format. Host tests first.
13. **Delete `jk_bms.c/h`**, drop the `jk` CLI tree, land the JK config, and read
    DeviceInfo through it at 115200 — an ordinary capability, plan and request.
14. **`http_server.c` / `cmd_parser.c`** onto `Modbus_*`; `Modbus_Init()` moves to
    `App_DefaultTaskEntry`.
15. **Dependency check in CMake** (§2.2).

**Acceptance for steps 1–7:** MQTT topics, payloads, retain flags, HA entity set
and every Trice string in §9 unchanged — the integration suite is the check;
`ctest` stays green; `modbus dump on` prints every decoded point with the broker
down.

**Acceptance for steps 8–15:** the same MQTT and HA surface, reached through a
different engine. The suite is **re-pointed** at step 9 rather than rewritten, so
those strings stay the check for the new engine too.

---

## 11. As-built — what is on the board

**This section is not authority.** It is here for the three jobs the design cannot
do: bisecting a regression, surviving the migration, and measuring. Where it and
§1-§10 disagree, §1-§10 wins.

### 11.1 What ships today

The v1 engine: `modbus_rtu.c` (RTU master, FC03/04/06, CRC, DE pin, monitor,
inject) and `modbus_walker.c` (one traversal per 100 ms tick, publishing straight
to MQTT), over `Shared/Modbus/` (records, A/B store, streaming compiler, exporter,
decode, units) and `jk_bms.c` beside it. It works against a Solis inverter and its
host tests are green.

Behaviour it has that the design deliberately removes: per-point publish
threshold/heartbeat, module-side device availability with a ≥30 s offline
throttle, `Modbus_Probe`, `Modbus_InjectResponse`, `start`/`stop`, one global baud
and one hardcoded port. **Do not carry any of it forward**, and do not read a
silence in §1-§10 as a licence to keep it.

Bugs it carries which the design retires rather than fixes:

| Defect | Where | Retired by |
|---|---|---|
| write address assumed `startAddr + offset`, wrong at any non-zero offset | `modbus_config_store.c:334` | `addr` authored verbatim (§3.3) |
| a writable point is never checked against its function code, so an FC06 write lands on a *holding* register of the same number in a different space | `modbus_config_compiler.c:605-609` | `w`/`rw` requires holding, at compile (§3.2) |
| `readPeriodS` is `uint16_t`, so a 24 h read cannot be expressed | `modbus_records.h:89` | `uint32_t` on the time-table record (§7.1) |
| a UART overrun is indistinguishable from "no byte" | `modbus_rtu.c:151` | `mbPortDone_lineError` (§5.1) |
| a slave answering *exceptions* counts as failing, so three misconfigured reads mark a healthy device offline | `modbus_walker.c:287-296` | availability leaves the module (§4.4, §8.3) |

### 11.2 The migration is a wipe

`MbCfgStore_RegionValid()` gates on `hdr.version` by strict equality, so on the
first boot of v2 firmware both regions are invalid, both are erased, and the board
comes up **unprovisioned** — it does not fall back to anything. **The uploaded
config is lost.** Download it with `GET /api/modbus/config/download` *before* the
update and re-upload after; afterwards the JSON exists nowhere on the board.

**No v1→v2 translator will be written.** One-shot migration code has to be written,
tested against streams nobody produces any more, and then either carried forever or
deliberately deleted — a poor trade while the device is in development and its
configs are files in the operator's hands.

### 11.3 Evidence the derivations are right

- **Framing.** `max(3.5 char times, 1.75 ms)` at 11 bits and 9600 baud is 4.01 ms
  — which is exactly the constant hardcoded in `modbus_rtu.c:122` today. That
  agreement is why §3.4's formula is trusted before it runs.
- **Publish policy.** Deleting it is behaviour-neutral because the shipped Solis
  config authors **no `publish` block on any of its 27 points**, and
  `threshold == 0` already means "publish every read" (`modbus_walker.c:156`).

### 11.4 Resource baseline

Measure with `arm-none-eabi-size -A build/application.elf` — plain `size` sums
`.bss` + `.ccmram` + `.ccmheap` into one column and makes main SRAM look nearly
full when it is not.

| Resource | Usage |
|---|---|
| Flash | ~263 KB of 480 KB |
| Main SRAM | ~67 KB of 128 KB |
| CCM | ~58 KB of 64 KB — **~91 %**: 48 KB FreeRTOS heap (`.ccmheap`) + ~11.4 KB `.ccmram` |
| Ext flash | 2×16 KB LUT + 4 KB selector at 0xF9000-0x101FFF |
| USART2, PD5/PD6/PD7 | exclusive to the module; after §5.1 it is a registered driver in port slot 0 |

**CCM is the binding constraint — ~6 KB free.** What the design moves:

- **Returned:** ~1.9 KB, deleting `s_lastValue`, `s_lastPublishTick` and
  `s_hasPublished` outright (no consumer takes the tracking over), plus ~3.25 KB
  of `.rodata` with the built-in default. It also removes the
  `MB_MAX_POINTS_TOTAL` → CCM coupling entirely: a point now costs only flash.
- **Added, in main SRAM deliberately:** ~1 KB of frame buffers (two ports × 256 B
  tx+rx, which *must* leave CCM because DMA writes into them), a 128 B
  subscription table and a 256 B request FIFO. Nothing about any of them is
  latency-critical.
- **Added, in CCM, and the number to watch:** the per-device derived read-block
  tables and the FreeRTOS software timers, which come off the 48 KB `.ccmheap`
  that task stacks also come from. Nothing in the design bounds this in advance —
  measure it across step 10 with `size -A` before and after, and treat a claimed
  saving that does not show up as evidence something else grew.

Consumer event copies are not module memory, but they do transit the same
`.ccmheap`, so a consumer leaking allocations exhausts the heap task stacks come
from. That is the consumer's bug to find, and the instrument is
`xPortGetFreeHeapSize()`, not `modbus status`.

---

## 12. Undesigned

Named deliberately, with the constraint any answer must satisfy.

**Open, and they need deciding before the step that meets them:**

- **Who enforces `writeMin`/`writeMax`** — *needed by step 11.* Two rules in §1.2
  point opposite ways with equal force. *The config decides what an item means*
  says the module enforces: the bounds are config, in the point record, in the same
  scaled domain as the value, and a caller-side rule is exactly what §4.6 refuses
  to rely on elsewhere. *Policy belongs to whoever acts* says the consumer
  enforces: the catalogue publishes the bounds so a requester can respect them, and
  the shipped bridge already does. Deciding it also decides whether `eModbusErr`
  gains a range member. Do not split the difference — enforcing in both places
  means two things that can disagree.
- **Whether plan identity is discoverable at runtime** — *needed by step 2.* A
  subscription is a plan mask, but nothing the module emits maps a plan's name or
  capability to its bit, so §4.10's fusion consumer hardcodes `0x03`. Either that
  mapping is a build-time contract between the config author and the consumer
  firmware — which should then be *stated*, since it makes a re-authored config a
  consumer-visible change — or plans join the catalogue as descriptors, which is
  the shape §1.2 requires of any answer (one data path, delivered as events on the
  modbus task, storing nothing).

**Deliberately not designed, and not blocking:**

- **A fifth dialect fact.** The group is closed at four, which covers both slaves
  on the bench. Whatever a third slave needs must be expressible as **data on the
  capability**, never a code path, or it is refused.
- **MQTT publish rate control.** Every sample is published today. If broker or
  recorder load makes that a problem, the answer is a policy on the MQTT side,
  configured on the MQTT side — not a field back in the Modbus config.
- **Sequence fairness and startup phasing.** Sequences due at the same moment are
  serviced first-come-first-served, and every device's timers start together at
  init, so periods stay phase-locked. The missed counter is the instrument that
  says when that stops being acceptable.
- **Multiple compile failures per upload.** At 20 bytes a result, the first four
  cost 80 B, so an operator could fix four problems in one cycle. Not free: the
  compiler aborts at the first failure today, and continuing means separating
  semantic errors it can skip past from structural ones it cannot.

JK PB-series integration is otherwise a config problem, not a firmware one. Its
protocol dossier lives in `~/Projects/JK_BMS` (`src/core/JkRegisters.h`,
`FW/decompiled/protocol-rs485-modbus.md` — read out of the BMS binaries, and they
override the vendor PDF). Running a DeviceInfo read against real hardware is still
the cheapest next fact to acquire, and after step 13 it is a config upload plus a
`Modbus_Request`.

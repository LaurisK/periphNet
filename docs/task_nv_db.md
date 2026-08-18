# Task — `nvDb`, the application's only path to non-volatile storage

**Status:** direction. The pattern and the criteria below are the binding part;
storage details and phasing are proposals to be confirmed when this is planned.

**Read in this order — the order is the priority.** §1 is the whole of what
`nvDb` is, and it is meant to be readable alone: §1.1 states the one rule the
module exists to serve, §1.2–§1.5 the pattern and the use case that motivate it,
and §1.4–§1.9 the four properties that follow — **configurable**,
**self-healing**, **deferred reclaim**, **wear measured**. §1.10 is where the
reference implementation broke them. §2 turns every one of those into something
checkable; §3 is the surface they imply; §4 is mechanism, and is **provisional**
where §1.6–§1.9 have outrun it. §5 is the one place where existing firmware
constrains the design. §6 is inventory — reference only. A reader who stops
after §2 should already be able to reject a wrong implementation.

Reference implementation: `lusety-lamp-hw/ZhagaFW` @ `f6a69f4` —
`Dri/Settings.c/.h`, `Dri/Calibration.c/.h`, and the accessor layer in
`__Lamp_V6_02/Aplication.c`.

---

## 1. What `nvDb` is

### 1.1 The two rules

> **Rule 1 — a user of `nvDb` knows nothing it does not need in order to
> perform its operation.**
>
> **Rule 2 — the application reaches non-volatile storage only through `nvDb`.
> There is no second path.**

**They are complements, and neither works alone.** Rule 2 is what turns Rule 1
from a convention into an invariant: hiding flash from *some* callers means
nothing while any module can still call `W25Q128_*` for itself. And Rule 1 is
what makes Rule 2 tolerable: a single door is only acceptable if going through
it costs a client no knowledge and no capability it actually needed.

So `nvDb` is **not the parameter store**. It is the application's non-volatile
layer, and parameters are merely its smallest client (§1.2).

Rule 2 has exactly two exceptions — the bootloader and the crash handler — and
neither is an escape from `nvDb`, only from its machinery. Both remain under its
authority through the contract in §1.3.

#### Rule 1 in detail

What a client needs depends on which class it belongs to (§1.2): a parameter
owner needs its section id, its struct type, and whether the call succeeded; a
streaming client needs a handle, a cursor and a result. **What no client of any
class ever needs is the same list**, and it is therefore not merely hidden but
**unreachable** — there is no API through which a consumer could learn it:

- that the store is flash at all, let alone external, SPI, or NOR;
- any absolute address, sector, page or bank;
- erase granularity (4 KB), program granularity (256 B), or the page-wrap
  hazard between them;
- buffer alignment, or any size rounded to a write granularity;
- the CRC, the A/B regions, the sequence number, the header;
- the blob's layout — where its own section sits, or that any other section
  exists;
- how large its allocation is, how much of it is spare, or how much of the
  store is unused;
- the schema version, the layout directory, or whether a restructure ran at
  boot and moved its data;
- the RAM mirror;
- which task commits, and when;
- SPI bus contention, the driver mutex, and wear.

**The rule cuts both ways.** It says *exactly* what is needed — not *as little
as possible*. Hiding something the caller does need is the same violation as
exposing something it does not, and it is the more dangerous direction, because
the caller cannot see what it is missing. Zhaga's one real defect is precisely
this: durability is something a caller needs to know about the operation it just
performed, and it was hidden behind a different function in a different header
(§1.10, D6). Nine mechanisms correctly hidden and one answer wrongly hidden are
the same rule broken twice.

**Scope: external flash only.** In periphNet `nvDb` lives entirely on the W25Q64
SPI NOR. Internal flash is fully allocated — 32 KB bootloader plus 480 KB
application — has no data sector, and is programmed by the bootloader during
FWU; the application has no business writing it. This is a constraint, not a
preference, and it works *for* the rule rather than against it: the part is not
memory-mapped, so there is no address a caller could dereference even if it
somehow had one. Every access is an SPI transaction, on a bus shared with the
FWU blob store, the crash log and the Modbus LUT (§4.1).

§2 is this rule made checkable.

### 1.2 What `nvDb` holds — clients and object classes

Everything in the application that outlives a reboot. Not a parameter blob with
neighbours, but the whole medium:

| Client | Object | Size | Shape |
|--------|--------|------|-------|
| FW images | stored + golden `.pnfw` blob, image meta | 488 KB × 2 | **streamed**; write-once, then delete + replace |
| Modbus config | LUT A/B + selector | 16 KB × 2 | **streamed** compile, A/B hot-swap at a lap boundary |
| Parameters | `wg_cfg`, MQTT, Trice UDP, identity | tens of bytes | **mirrored**; random access, rarely written |
| Time base | `wg_time` slot ring | 16 B slots | **append**; one write per 15 min |
| Crash log | one crash record | ~KB | written from fault context (§1.3) |
| Boot status | FWU flags | 4 KB sector | NOR bit-clear, shared with the BL (§1.3) |
| *later* | BMS/CAN state, provisioning | — | — |

**Three access shapes, therefore three API shapes** — mirrored, streamed,
append. This is Rule 1 doing work, not a convenience: forcing a 488 KB blob
through a `Load/Store(offset, length)` call would hand its client an interface
that does not fit what it is doing, and forcing a 60-byte record through a
stream handle would hand a parameter owner ceremony it does not need. *One API
for everything* would violate the rule as surely as exposing sector numbers
does.

What unifies them is not the call shape but the authority: **one module owns the
medium, its address space, its reclamation and its wear.** That is what Rule 2
buys, and it is why the clients above are clients rather than co-owners.

### 1.3 The exception contract — bypass execution, never authority

Two clients cannot use `nvDb`'s machinery, for reasons that are physical rather
than architectural:

- **The bootloader** cannot link `nvDb` at all — 32 KB, no RTOS, and it must
  read boot status and install from the blob areas before the application exists
  (C33).
- **The crash handler** runs in fault context, where the RTOS may be dead and an
  SPI transaction may have been in flight when the fault hit. `crash.c:239-246`
  resets the peripheral by hand — CS high, `__HAL_UNLOCK`, `State = READY` —
  and the driver's own mutex deliberately yields to it (`bus_lock` returns early
  when `__get_IPSR() != 0`). No queued, mutex-guarded, task-based path can serve
  this.

**What they are exempt from is performing their own I/O. What they are not
exempt from is `nvDb` deciding where.** The exception is a bypass of the
mechanism, never of the authority — otherwise Rule 2 decays into a preference
the moment anything inconvenient appears.

The contract, both directions:

| `nvDb` owes | The exception client owes |
|---|---|
| Publish a **locator** at a fixed anchor: a small self-describing record naming where each exception client's region currently lives | **Resolve its address through the locator before every operation.** A compiled-in address is a defect, not an optimisation |
| Keep the locator readable by minimal code — no RTOS, no mutex, no allocation, no `nvDb` | Tolerate an unreadable locator by refusing to write, never by guessing |
| Keep it valid at every instant across a relocation, so a power cut never leaves it naming nothing | Never widen its region, never write outside what the locator granted |
| Update it as part of any move | Obey a move without needing to be rebuilt |

**The hazard that constrains all of this:** *you cannot relocate a region out
from under a bootloader that does not read the locator.* A deployed BL is old
firmware and may predate the contract entirely; move the golden blob and such a
device does not boot. So **BL-visible regions stay pinned by default**, and stay
pinned until every deployed bootloader resolves through the locator — after
which the locator format is frozen at v1 permanently, because the BL that reads
it can never be assumed to have been updated. The crash handler carries no such
risk: it ships inside the application and is always in step with the `nvDb` that
moved it.

### 1.4 The pattern — three layers, one blob

Zhaga's store is not "a settings struct". It is three layers with a hard
boundary between each, and the boundary is the whole point.

```
   subsystem            nvDb_Load / nvDb_Store          section id + offset + length
   (luminare, time,  ──────────────────────────────►    bounds-checked, typed by
    apn, server, …)                                     the CALLER's own struct
                                                                  │
   ───────────────────────────────────────────────────────────────┼──────────────
                                                                  ▼
   RAM mirror          one global blob                  the single live copy;
                       Settings_t settings              loaded once at init
                                                                  │
   ───────────────────────────────────────────────────────────────┼──────────────
                                                                  ▼
   storage             Read_ / Write_ / SetDefault      one sector, one CRC,
                       (Dri/Settings.c)                 nobody else touches flash
```

**Layer 3 — storage.** One struct wrapped with one CRC, one sector, three
functions:

```c
typedef struct { Settings_n_t settings; uint32_t crc; } Settings_t;

int Read_Settings       (Settings_t *s);   /* read sector, verify CRC */
int Write_Settings      (Settings_t *s);   /* compute CRC, write sector */
int Settings_Set_Default(Settings_t *s);   /* memset + fill defaults */
```

**Layer 2 — the RAM mirror.** Exactly one instance (`Settings_t settings` in
`Aplication.c`), loaded once in `App_InitCfg()`: read → on failure, defaults →
write. Every subsequent access is a RAM access.

**Layer 1 — the accessor.** Subsystems address the store as
**(section, offset, length)**, never as a struct:

```c
eResult nvDb_Load (eNvDbUser user, void* buff, uint16_t offset, uint16_t dataLen);
eResult nvDb_Store(eNvDbUser user, void* buff, uint16_t offset, uint16_t dataLen);
```

One private function — `GetSettingsPtr()` — is the *only* code that translates
a section id into a pointer into the blob, and it bounds-checks
`offset + dataLen` against that section's size before returning. Consumers know
their own sub-struct type and use `offsetof()` inside it; they do not know
`Settings_n_t` exists:

```c
nvDb_Load (nvbd_timeParam, &timeParam, 0, sizeof(sTimeParam));
nvDb_Store(nvbd_timeParam, &riseTime, offsetof(sTimeParam, riseTime), sizeof(sDateTime));
nvDb_Load (nvbd_apn, &checkingApn, sizeof(telit_app_apn_t) * i, sizeof(telit_app_apn_t));
```

That is what makes the blob's layout private, makes a partial field update a
one-liner, and makes remote get/set generic: `LAMP_aplication.c` forwards a
wire command's `(section, offset, length)` straight into `nvDb_Load`/`Store`
without knowing what any of it means.

**Consumer idiom** — load into a module-local copy at init; if the load failed
*or* the section is still all-zero, use the module's own defaults:

```c
eResult dbRes = nvDb_Load(nvbd_luminare, &lumParam, 0, sizeof(sLuminareParameters));
if ((dbRes != res_ok) || (sizeof(sLuminareParameters) == utl_MemFillCheck(...0x00...))) {
    Lum_ResetParameters();
}
```

**A second store, deliberately separate.** `Dri/Calibration.c` holds per-device
identity (`Lamp_ID`, seeded from `HAL_GetUIDw0() + HAL_GetUIDw2()`) and radio
calibration in its own sector with its own CRC. Different write authority —
only reachable in calibration mode over a jig's RS-232 link — and a different
lifecycle. **Provisioning data is not settings data**, and Zhaga separates them
at the storage layer, not by convention.

### 1.5 The use case — users address bytes, the store owns the flash

The reason to build this is not tidiness. It is that **every parameter today is
written by a subsystem that had to learn NOR flash to store a number.** The
`nvDb_*()` API exists to end that, and the measure of the design is how much of
the hardware it makes unsayable.

#### What the hardware demands

Underneath Zhaga's store is `Dri/Flash_g0.c`, and its API is entirely made of
flash mechanics:

```c
int Flash_g0_WriteSector(uint32_t sector, uint64_t *data, uint32_t dataSize);
int Flash_g0_ReadSector (uint32_t sector, uint64_t *data, uint32_t dataSize);
```

To call it correctly a caller must know, all at once:

| Obligation | Detail |
|------------|--------|
| Which page number is free | `240` and `241`, hand-picked in `SYSTEM_config.h` |
| That a page number *is* an address | `FLASH_BASE + sector * FLASH_PAGE_SIZE` |
| That the part is dual-bank | `sector < 128 ? BANK_1 : BANK_2`, page = `sector & 0x7F` |
| Buffer alignment | the parameter is `uint64_t *` — the buffer must be 8-byte aligned |
| Size granularity | `if (dataSize % 8) return -1;` — hence `PACK_8B(x) ((x + 7) & ~7)` |
| That a write erases | `Flash_g0_WriteSector` unconditionally erases the whole 2 KB page first |
| That there is no partial write | erase granularity is a page; program granularity is a doubleword |
| Unlock / lock | `HAL_FLASH_Unlock()` … `HAL_FLASH_Lock()` around every write |
| Verification | the driver reads back and compares, doubleword by doubleword |

Nine obligations, every one of which is a way to silently corrupt a neighbour's
data if you get it wrong.

#### What a user writes

```c
/* time.c:590 — a 2-byte field                                                  */
nvDb_Store(nvbd_timeParam, &localDeviation,
           offsetof(sTimeParam, localDeviation), sizeof(int16_t));

/* rtc.c:498 — ONE byte, at an odd offset, inside an array                      */
nvDb_Store(nvbd_timeCalibr, &calibrData[i].asynchPrediv,
           (CALIBR_DATA_SET_SIZE * sizeof(uint16_t)) + (i * sizeof(uint8_t)),
           sizeof(uint8_t));

/* profile.c:305 — random access to record `profId` of an array in the blob     */
nvDb_Store(nvbd_profiles, profInfo, sizeof(sProfInfo) * profId, sizeof(sProfInfo));

/* TELIT_app.c:838 — 2 bytes at offset 26, straight after a char[26]            */
nvDb_Load(nvbd_srvInfo, &telit_app_port, offsetof(sServerInfo, port), sizeof(uint16_t));
```

Read `rtc.c:498` against the table above. It stores **one byte at an unaligned
offset** on a device whose flash cannot program less than eight bytes and cannot
erase less than two thousand and forty-eight. Nothing in that call is a lie —
the byte really does become durable — and nothing in it is true of the hardware
either. That gap is the abstraction.

**To its users the store is byte-addressable random-access memory with a bounds
check.** That is precisely what NOR flash is not, and it is the only model a
subsystem author should ever need.

#### Where each obligation dies

| Obligation | Absorbed by | Reaches the user? |
|------------|-------------|-------------------|
| Page number `240`/`241` | `Settings.c` / `Calibration.c` (one `#define` each) | never |
| Absolute address | `Flash_g0.h` — the macro lives there and nowhere above | never |
| Bank selection | `Flash_g0.c` | never |
| 8-byte buffer alignment | the one `(uint64_t*)` cast in `Settings.c` | never |
| `% 8` size rounding | one `PACK_8B()` in `Settings.c` | never |
| Erase-before-write | `Flash_g0_WriteSector` | never |
| Doubleword program granularity | `Flash_g0_WriteSector` | never |
| Unlock / lock | `Flash_g0_WriteSector` | never |
| Readback verify | `Flash_g0_WriteSector` | never |
| CRC compute / check | `Read_Settings` / `Write_Settings` | never |
| *When* flash is touched at all | the RAM mirror — read once at boot | never |
| Bounds of my own data | `GetSettingsPtr()` | as a return code |
| **Whether my value is durable yet** | **nobody** | **yes — see below** |

Eleven of thirteen die below the API. A subsystem author writes `offsetof()` and
`sizeof()` and is finished.

#### The section id is a capability, not a convenience

`GetSettingsPtr()` bounds-checks `offset + dataLen` against *that section's*
size before returning a pointer. So the id is not merely a lookup key — it is
the limit of what a caller can reach. A subsystem with an arithmetic bug
corrupts its own parameters and cannot touch anyone else's, in a shared blob
with no MPU behind it. Two subsystems living in the same flash page never learn
that they do.

#### Where the abstraction leaks — and it is one leak, twice

The list above has two rows that do not die, and they are the same failure seen
from two sides:

1. **Durability is left to the user, through a different API, at a different
   layer.** `nvDb_Store` writes the RAM mirror; making it survive a reset needs
   `Write_Settings()` — which is declared in `Settings.h`, not in the
   `Aplication.h` where `nvDb_Store` lives, and which takes the blob the user
   was never supposed to know about. The abstraction hides the mechanism but
   keeps the *responsibility*, which is the worst of both. The subsystems that
   believed the abstraction — `luminare.c`, `profile.c`, `time.c`, `binXfer.c` —
   are exactly the four that never flush (D6).
2. **The failure modes don't map.** `nvDb_Store` can only return argument errors
   (`res_invalidArg1/2/4`); it cannot report a flash failure because it never
   touches flash. `Flash_g0_WriteSector`'s real diagnostics — `-3` erase failed,
   `-4` readback mismatch — are collapsed by `Write_Settings` into `0`, and
   reach the user, if at all, as a boolean from an unrelated call. So the caller
   gets full checking on the arguments and none at all on the outcome.

The fix is not more discipline. It is that **the API that accepts the write owes
the answer about the write** — which is why §3 has `NvDb_Service()` and
`NvDb_IsDirty()`, and why C5 is a criterion rather than a note.

One further limit worth naming rather than fixing: the section is bounds-checked
but not type-checked. `void*` in, `memcpy` out — passing an `sProfInfo` to
`nvbd_luminare` is accepted if it fits. That is the price of a byte-addressed
interface, and it is the right price, but it should be paid knowingly.

#### What this has to hide in periphNet

Different silicon, same job, and the numbers are worse. The parameter store
would sit on the W25Q64 over SPI (`Shared/Drivers/w25q128.c`):

- **4 KB sector erase**, and `W25Q128_EraseSector` takes an absolute address.
- **256-byte page program**, and `W25Q128_WritePage` checks `len > 256` but
  **not whether `addr + len` crosses a page boundary** — on this part the
  program wraps to the *start of the page* and overwrites what is already
  there. A silent corruption, one argument away.
- **Absolute addresses everywhere**: `EXT_FLASH_WG_CFG_ADDR` appears four times
  in `wg_cfg.c`; `wg_time.c` does its own slot arithmetic
  (`EXT_FLASH_WG_TIME_ADDR + i * WG_TIME_SLOT_SIZE`) and its own erase.
- **The bus is shared** with the FWU blob store and the crash log, serialized by
  a driver mutex that every caller must simply not defeat.

Today every subsystem carries all of this itself. `sWgCfgRecord` is 60 bytes and
so fits one page — but its `reserved[32]` is commented *"room for a provisioned
private key"*, so it is a struct designed to grow, sitting on a call that will
wrap silently at 256. That is the bug `nvDb` has to make unwritable, not the
bug it has to remember to avoid.

#### What that buys, stated plainly

1. **One place to add a parameter** — a field in a section struct plus a
   default. No address, no page budget, no alignment, no new validity rule.
2. **Settings survive reboot** by construction, not by each author remembering.
3. **Factory reset is one call and is always complete.**
4. **Backup / restore / provisioning a spare board** is one blob, not a
   per-subsystem scavenger hunt.
5. **Remote get/set is generic** — one `(section, offset, length)` command pair
   serves every parameter that will ever exist, which is why Zhaga's radio
   protocol needs no new opcode per setting.
6. **A parameter has one owner** — the section struct is its schema; the owning
   module supplies defaults, validation and serialization.
7. **A flash-mechanics bug has one place to be** — and it is a place with host
   unit tests, instead of nine places without.

### 1.6 Configurable — the layout is data, not code

Which parameters exist, and how much room each is given, is **configuration the
store reads**, not structure the store is compiled around. A section's
allocation is a number the build supplies; the stored blob carries a directory
recording the layout it was written under (§4.3 has the record).

Two consequences follow, and they are the point:

- **An image can read a blob laid out by a firmware it has never met.** Layout
  change and corruption stop being the same event, which is what makes the
  difference between migrating a field setting and silently resetting it.
- **Adding, resizing, reordering or removing a parameter needs no code.** The
  store moves opaque bytes; it never learns what they mean. Compare Zhaga, where
  offsets are compiled in and so a removed field must live on forever as
  `unusedN` — nine of them by now (§1.10, D1).

The store therefore has to know what space is **occupied** and what is spare.
Occupancy is *declared by the owner*, never inferred from content: an erased NOR
byte is `0xFF` and a defaulted one is `0x00`, but a parameter legitimately
holding 0 or 255 is indistinguishable from either. Zhaga infers, and a luminaire
whose parameters are all genuinely zero re-defaults on every boot.

### 1.7 Self-healing where possible — and honest where not

A parameter store that can be broken by an event the field will certainly
produce is not a parameter store. `nvDb` repairs what it can, and the recurring
shape is that **damage is bounded to the smallest thing that was actually
damaged**:

| Damage | Response |
|--------|----------|
| Power cut mid-write | the other region is still valid; the write is retried (C14) |
| One region corrupt | the survivor serves, the corrupt one is rewritten on the next commit |
| Both regions invalid | defaults, and the device **still boots** (C16) |
| Stored layout ≠ built layout | restructure in place, preserving everything that fits (§4.3) |
| One section too big for its new allocation | that section alone resets — not the blob (C23) |
| A read that fails validation | discarded into scratch; the running configuration is untouched (C15) |

**Where it stops, stated plainly.** A physically failed chip is not recoverable.
Neither is a blob whose regions are both gone — defaults are a survival mode,
not a repair. The store's obligation there is to **fail loudly and boot anyway**:
report what was lost through the diagnostics in §3, and never pretend the
parameters are the ones the operator set. Silent recovery from unrecoverable
damage is the failure mode this principle exists to prevent, not an aspiration.

### 1.8 Deferred reclaim — delete marks, the collector cleans

Erasing is the slow, destructive operation, so it does not happen on the
caller's thread. `NvDb_Delete` **marks** the space and returns; a low-priority
collector reclaims it later.

The vocabulary, since these are standard: the mark is a **tombstone**; the
background reclaim is **garbage collection** (also *compaction* or *cleaning*);
deferring the erase until the space is actually needed is **lazy erase**.

**This project already ships the pattern, twice.** `App/Img/image_store.c`:

- `ImgStore_Delete` (`:361`) is a tombstone delete. Removing a 488 KB stored
  blob erases only the *first* 4 KB sector — killing the manifest — and leaves
  ~484 KB of stale ciphertext physically present. Delete costs 2 erases instead
  of ~122, and the blob is logically gone the moment it returns.
- `flush_upload_page` (`:198`) is the reclaim: a lazy 4 KB sector erase
  immediately before the first write to each sector.
- `imgRes_busy` + `read_held` is already the write-after-delete guard, expressed
  as a **refusal**.

`nvDb` differs in one way, and it is the way that matters: the collector is a
**background, low-priority process** rather than reclaim-by-the-next-writer.
That is what makes the hazard real — a write can arrive for space that is marked
but not yet clean.

**The hazard, and why nobody blocks.** Such a write must not proceed until the
reclaim of that space has finished. It must also not stall its caller, because
callers never block on flash (§4.2). Both hold at once because the two happen at
different layers: the **RAM mirror absorbs the write immediately**, and the
single flash agent simply orders the erase ahead of the commit. Collector and
committer being the *same task* is what makes this ordering structural rather
than a lock — and it is why "one writer to flash" (C36) survives the addition of
a collector intact.

**Reclaim is incremental and preemptible** — one erase per service slice — so a
commit that becomes due never waits behind a long compaction. The precedent is
in this codebase too: the Modbus walker commits config swaps at lap boundaries,
never mid-lap.

### 1.9 Wear is measured — it is the thing that runs out

Flash endurance is finite and erases are what spend it. A store that writes
parameters for years without counting has no way to know whether its own
assumptions still hold.

**Monitoring is not levelling**, and only the first is claimed here. Levelling
spreads erases to extend life and costs a mapping layer;
`Shared/Fwu/bl_app_contract.h:59` already records the judgement that config
edits are rare enough not to need it. Monitoring is what tells you whether that
judgement is still true in the field — which is precisely the thing that
assumption cannot tell you about itself.

**Counting must not itself cost erases.** NOR flash clears bits `1 → 0` without
an erase, so the count rides on bit-clears: one page of wear metadata per
erasable unit, holding a counter plus a run of bits; each erase clears one more
bit; only when the bits are exhausted does the counter increment, which is the
single event that costs an erase.

```
effective erase count = counter × bits_per_page + popcount(cleared bits)
```

The property that makes it work: **the metadata page is erased once per N
tracked erases, so its own wear is N× lower than what it measures.** The
technique is not new here — `sBootFlags`' `boot_attempt_0/1/2` is the same
idiom at three bits, and CLAUDE.md documents NOR bit-clearing as a project
technique.

Like free space, wear reaches **operators and never consumers** (§1.1): a
subsystem storing a parameter has no use for an erase count, and handing it one
is how the rule erodes.

Detail deliberately left open (§7): which granularity is tracked — the W25Q64's
minimum erase is the 4 KB sector (`W25Q128_SECTOR_SIZE`), with 32 KB and 64 KB
block erases also available — and whether a bit or a byte is cleared per erase,
which depends on the part's limit on partial-page programs between erases.

### 1.10 What the reference implementation gets wrong

Copy the shape; do not copy the decay. Every item below is visible in
`ZhagaFW` today and each one becomes a criterion in §2.

| # | Defect | Where |
|---|--------|-------|
| D1 | `unused`, `unused5` … `unused9` — a raw struct with no schema can only ever append; removed fields must stay forever as padding | `Settings.h` |
| D2 | Opaque `uint32_t` arrays with hand-computed lengths cast back at use: `profiles[144]`, `apnList[165]`, `luminare[13]`, `timeCalibr[11]`. The size arithmetic lives in a trailing comment — and `timeCalibr`'s already reads `sizeof(uint8_t * 13)`, which sizes a *pointer*. The comment is already a lie | `Settings.h:29-36` |
| D3 | **No version field** → no migration path. A layout change is indistinguishable from corruption, so the recovery is a silent full reset to defaults | `Settings.c` |
| D4 | The `sizeof(Settings_t) % 8 == 0` requirement (G0 doubleword writes) is enforced by a comment, not a `_Static_assert` | `Settings.h:41` |
| D5 | Single sector rewritten in place — a power cut mid-write loses **all** settings | `Write_Settings` |
| D6 | **Store without commit.** `nvDb_Store` writes the RAM mirror only; flash needs a separate explicit `Write_Settings()`. `luminare.c`, `profile.c`, `time.c`, `binXfer.c` all store and never flush — those values persist only if some *unrelated* subsystem happens to save later. This is the exact failure the store exists to prevent, reintroduced one layer up | `luminare.c:731,786,902`, `profile.c:305`, `time.c:514,520,590`, `binXfer.c:538,778,833` |
| D7 | The accessor lives in `Aplication.c`, the app's main file, while storage lives in `Dri/Settings.c`. The store's own header declares neither `nvDb_Load` nor `nvDb_Store` | `Aplication.h` |
| D8 | `Read_Calibration()` reads the sector **straight over the live global**, so a failed CRC leaves the running device holding flash garbage — including a different `Lamp_ID`. Recognised and worked around by adding a second reader (`Read_CalibrationRadio`) rather than fixing the first | `Calibration.c` |
| D9 | `Settings_Set_Default()` (a driver) calls `Lum_ResetParameters()` and pokes `telit_app_apn_t` — the storage layer reaching up into app modules | `Settings.c` |

---

## 2. Criteria

Every criterion below is an instance of §1.1 — *a user knows nothing it does not
need to perform its operation* — restated so it can be checked in review or in a
host unit test. An implementation is `nvDb` if and only if it satisfies all of
them. **C1 is Rule 2** and is the one criterion a single grep settles. The rest
follow §1: C2–C5 are Rule 1 applied to the medium (§1.5), C6–C8 the exception
contract (§1.3), C17–C24 a configurable layout (§1.6), C25–C29 deferred reclaim
(§1.8), C30–C32 wear (§1.9). C10 applies Rule 1 to the blob layout, C13 to
encoding, and C5 to the one thing that must **not** be hidden. C14–C16 and C23
are where self-healing (§1.7) is actually enforced.

**Access and opacity** — `nvDb` is the only door (Rule 2), the store behind it is
external SPI NOR, and nothing above the module may depend on that or on its
being flash at all

- **C1 — `nvDb` is the only path to the medium (Rule 2).** Outside `nvDb`'s own
  translation units, no application code calls `W25Q128_*`, and none contains an
  absolute flash address, a sector or page number, an erase call, an alignment
  cast, or a size rounded to a write granularity. One grep is the whole test:
  `grep -rn "W25Q128_" App/ Shared/` must return `nvDb` and the driver only.
  **Today it returns ten other files** (§6) — this criterion is the migration.
- **C2 — Any offset, any length.** `NvDb_Load`/`NvDb_Store` accept any byte
  offset and any byte length inside a section — one byte at an odd offset
  included. Alignment, page boundaries and the 256-byte program limit are the
  store's problem, never the caller's.
- **C3 — Erase, wear and granularity are invisible.** A caller cannot tell
  whether its store cost an erase cycle, and must never need to.
- **C4 — A section is a bound, not a label.** `offset + len_bytes` is checked
  against that section's size before any access, so a subsystem's arithmetic bug
  can only damage its own parameters.
- **C5 — Whoever accepts the write owes the answer about it.** Durability is
  reported through the same API that took the data — never delegated to a
  different function in a different header, which is how Zhaga lost four
  subsystems' settings (D6, §1.10). `NvDb_IsDirty()` makes the pending state
  observable and `NvDb_Service()` makes the commit automatic.

**The exception contract** (§1.3) — two clients bypass the machinery, neither
bypasses the authority

- **C6 — An exception client performs its own I/O but never chooses its own
  location.** It resolves through the locator before every operation; a
  compiled-in region address is a defect. `nvDb` may relocate it, and it obeys
  without being rebuilt.
- **C7 — The locator is readable without `nvDb`** — no RTOS, no mutex, no
  allocation, minimal code — **validated**, and valid at every instant across a
  relocation, so a power cut never leaves it naming nothing. A client that
  cannot read it refuses to write rather than guessing.
- **C8 — A bootloader-visible region is pinned until every deployed bootloader
  resolves through the locator**, and the locator format is frozen at v1 from
  that day on. Relocating a region out from under a BL that predates the
  contract does not degrade the device, it bricks it.

**Pattern integrity**

- **C9 — One store, one blob, one CRC, one owner.** A second parameter region is
  a design failure, not a feature.
- **C10 — No caller sees the layout.** There is no public whole-blob type and no
  pointer into the mirror ever escapes the module. Access is
  `(section, offset, length)` only, bounds-checked in one place.
  *(This rejects the `const sNvParams *NvDb_Get()` accessor sketched in the
  earlier revision of this note: handing out a typed pointer to the whole blob
  makes every consumer a layout dependency and deletes the property that makes
  the pattern worth copying.)*
- **C11 — Every section is a real typed struct.** No `uint32_t x[N]` with a size
  comment (D2). A `_Static_assert` pins every section's size and the total.
- **C12 — Defaults are complete and owned.** `NvDb_SetDefaults()` zeroes the
  blob and then fills every section; a field added without a default is a bug.
  The values come from the owning module through a defaults callback — `nvDb`
  does not call up into App code (D9).
- **C13 — Serialization is not `nvDb`'s job.** Sections are opaque bytes to the
  store; JSON get/set for a section belongs to its owner. `/api/config`
  composes those, it does not reach into the blob.

**Durability**

- **C14 — No single power cut loses the store** (D5). A/B regions, alternating
  writes, highest valid sequence wins; at every instant at least one region
  validates.
- **C15 — Flash is never read over the live mirror** (D8). Read into scratch,
  validate, publish — a rejected read leaves the running configuration
  untouched.
- **C16 — Boot always succeeds.** Neither region valid → defaults → the device
  runs. Loss of parameters never costs a boot.

**Layout and change over time**

- **C17 — Versioned, with an explicit migration step per bump** (D3). A CRC
  mismatch means corruption. A version change means migrate. The two must never
  share a recovery path, because "reset to defaults" as a response to a layout
  change is silent config loss on every OTA that touches the blob.
- **C18 — Removing a field must be possible** without leaving a permanent hole
  (D1). Whatever the encoding, deleting a parameter must be a normal edit.
- **C19 — The layout is configuration, not scattered constants.** Every
  section's allocation comes from one table, and `_Static_assert` proves each
  owner's struct fits its allocation and that the allocations fit the payload.
- **C20 — The stored blob is self-describing.** A directory travels with the
  data, so an image can always read a blob written under a different layout —
  including one written by a firmware it has never met.
- **C21 — Restructuring is generic.** Adding, resizing, reordering or removing a
  section involves no per-section and no per-version code: to `nvDb` a section's
  contents are opaque bytes it moves without knowing what they mean.
- **C22 — Occupancy is declared, never inferred.** How much of a section is real
  data comes from its owner, not from scanning for `0x00`/`0xFF` fill — a
  parameter legitimately holding 0 or 255 is indistinguishable from unset
  (Zhaga infers, and re-defaults a luminaire whose values are all genuinely
  zero; §4.3).
- **C23 — Data that fits is kept; data that does not costs only its own
  section.** A restructure never discards a section the new layout can hold, a
  section that cannot fit is reset alone rather than taking the blob with it,
  and the outcome is retained and reportable — not merely logged.
- **C24 — Free space is known, reportable, and invisible to consumers.** Both
  headroom within a section and unallocated payload are exact; both reach
  operators through diagnostics, and neither reaches a section owner (§1.1).

**Reclaim**

- **C25 — Delete marks and returns.** `NvDb_Delete` never erases on the caller's
  thread; the erase is the collector's job (§1.8).
- **C26 — A tombstone takes effect immediately.** Space is logically gone the
  instant delete returns, whatever its physical state — a subsequent read must
  never see the bytes that are still sitting there.
- **C27 — Reclaim is incremental and preemptible.** One erase per service slice,
  so a commit that becomes due is never stuck behind a long compaction.
  Precedent: the Modbus walker commits swaps only at lap boundaries.
- **C28 — A write to unreclaimed space waits, and its caller does not.** The RAM
  mirror absorbs the write at once; the flash agent orders the erase ahead of
  the commit. Both halves are required — dropping either breaks §1.8 or §4.2.
- **C29 — Collector and committer are the same task**, so C36 holds by
  construction rather than by locking.

**Wear**

- **C30 — Every erase is counted, and counting costs no erase** in the common
  case (§1.9). A wear scheme whose own writes dominate what it measures is
  worse than none.
- **C31 — Wear is reportable to operators and unreachable by consumers** — the
  same split already applied to free space (C24).
- **C32 — An erase or verify failure is a recorded event**, not a silent retry.
  Without this, §1.7 has no signal to act on and no way to be honest about what
  it could not repair.

**Placement and cost**

- **C33 — `nvDb` never links into the bootloader.** Same rule as
  `Shared/Modbus/`: host-testable, its own source list, out of
  `${SHARED_SOURCES}`. The 32 KB budget is not negotiable.
- **C34 — `nvDb` depends downward only** — flash driver and libc. Not on App/,
  not on lwIP (D9). Its API is declared in its own header (D7).
- **C35 — The mirror is bounded and lives in main SRAM.** A few hundred bytes,
  `_Static_assert`-ed. Not CCM: CCM is ~91 % full and parameter reads are not
  latency-critical — the same call already made for the Modbus subscription
  table (`docs/modbus.md` §2, Q3).
- **C36 — One writer to flash.** The mirror is guarded; the flash write happens
  from one task only, so concurrent `http` / `cmd` / `mqtt` / `modbus` writers
  cannot interleave a commit.

## 3. API surface

Module prefix `nvdb`. Naming per `C coding standard.md`: `Module_ActionDetails`
PascalCase functions, `<modulePrefix><Category>_<value>` enum values.

```c
/* --- section ids ------------------------------------------------------- */
typedef enum {
    nvdbSect_undefined = 0,
    nvdbSect_identity,      /* device name, hwId                            */
    nvdbSect_net,           /* DHCP vs static, static addr/mask/gw          */
    nvdbSect_wg,            /* tunnel addr/mask, endpoint, keepalive_sec    */
    nvdbSect_mqtt,          /* broker addr, port, topic prefix, HA prefix   */
    nvdbSect_triceUdp,      /* Trice UDP destination                        */
    nvdbSect_last
} eNvDbSection;

/* --- results ----------------------------------------------------------- */
typedef enum {
    nvdbRes_undefined = 0,
    nvdbRes_ok,
    nvdbRes_notInit,
    nvdbRes_badSection,     /* unknown id                                   */
    nvdbRes_nullBuf,
    nvdbRes_outOfBounds,    /* offset + len_bytes exceeds the section       */
    nvdbRes_flash,          /* read/write/erase failed                      */
    nvdbRes_noSpace,        /* layout allocations exceed payload capacity   */
    nvdbRes_wontFit,        /* stored section too large for its new alloc   */
    nvdbRes_pendingReclaim, /* marked for deletion, not yet erased           */
    nvdbRes_wearLimit,      /* unit past its endurance threshold             */
    nvdbRes_last
} eNvDbRes;

/* --- lifecycle --------------------------------------------------------- */
eNvDbRes NvDb_Init       (void);   /* newest valid region, else migrate, else defaults */
eNvDbRes NvDb_SetDefaults(void);   /* whole blob; factory reset                        */

/* --- access (RAM mirror) ----------------------------------------------- */
eNvDbRes NvDb_Load (eNvDbSection sect,       void *buff, uint16_t offset, uint16_t len_bytes);
eNvDbRes NvDb_Store(eNvDbSection sect, const void *buff, uint16_t offset, uint16_t len_bytes);

/* --- persistence ------------------------------------------------------- */
eNvDbRes NvDb_Commit (void);   /* force: mirror -> inactive region -> flip. Blocks.   */
bool     NvDb_IsDirty(void);   /* uncommitted changes pending                          */
void     NvDb_Service(void);   /* deferred commit; called from the single writer task  */

/* --- diagnostics: for operators and the HTTP layer, NOT for section owners -- */
typedef struct {
    uint16_t capacity_bytes;   /* payload capacity of one region                */
    uint16_t allocated_bytes;  /* sum of alloc_bytes over all sections          */
    uint16_t used_bytes;       /* sum of used_bytes                             */
    uint16_t free_bytes;       /* capacity_bytes - allocated_bytes              */
    uint8_t  sectionCnt;
    uint8_t  layoutVer;
    eNvDbRes lastRestructure;  /* outcome of the boot-time rebuild, if any      */
} sNvDbUsage;

eNvDbRes NvDb_GetUsage      (sNvDbUsage *out);
eNvDbRes NvDb_GetSectionInfo(eNvDbSection sect,
                             uint16_t *alloc_bytes, uint16_t *used_bytes);

/* --- reclaim (§1.8) ---------------------------------------------------- */
eNvDbRes NvDb_Delete(uint32_t address, uint32_t size);  /* marks; returns at once */
/* NvDb_Service() above also drives the collector, one erase per slice.        */

/* --- streamed objects (§1.2): FW blobs, Modbus LUTs --------------------- */
typedef struct sNvDbStream sNvDbStream;                 /* opaque handle      */
eNvDbRes NvDb_OpenWrite (eNvDbObject obj, uint32_t size_bytes, sNvDbStream **st);
eNvDbRes NvDb_Write     (sNvDbStream *st, const void *buf, uint32_t len_bytes);
eNvDbRes NvDb_OpenRead  (eNvDbObject obj, sNvDbStream **st);
eNvDbRes NvDb_Read      (sNvDbStream *st, void *buf, uint32_t len_bytes);
eNvDbRes NvDb_Close     (sNvDbStream *st);              /* commits or aborts  */

/* --- the exception contract (§1.3) -------------------------------------- */
/* Resolve where a bypass client may operate. Callable with no RTOS, no mutex
   and no allocation, so the crash handler can use it in fault context.       */
eNvDbRes NvDb_Locate    (eNvDbObject obj, uint32_t *addr, uint32_t *size_bytes);
/* The single sanctioned bypass: direct write at a located region, no queue,
   no mutex, no deferral. Fault context only.                                 */
eNvDbRes NvDb_PanicWrite(eNvDbObject obj, const void *buf, uint32_t len_bytes);

/* --- wear (§1.9): operators only --------------------------------------- */
typedef struct {
    uint32_t eraseCnt;               /* counter x bits + popcount, per unit  */
    uint32_t pendingReclaim_bytes;   /* marked, not yet erased               */
    uint32_t reclaimed_bytes;        /* erased since boot                    */
    eNvDbRes lastReclaim;
} sNvDbWear;

eNvDbRes NvDb_GetWear(uint32_t address, sNvDbWear *out);
```

Notes on the surface:

- **`NvDb_Load`/`NvDb_Store` are the whole consumer-facing API.** A subsystem's
  entire relationship with persistence is: load its struct at init, store the
  field it changed, and be done. Nothing else in the list is for consumers.
- **`eNvDbSection` values are persisted and must never be renumbered.** They are
  written into the stored layout directory (§4.3) — which is what lets an image
  read a blob laid out by a different firmware — so they join `eFwuRes`,
  `eModbusDecodeType` and `eCrashType` on the never-renumber list. Append only.
  The `_last` sentinel is a count, not a stored value, and stays safe.
- **`NvDb_Store` does not write flash** (matching Zhaga), but D6 is closed by
  `NvDb_Service()`: it marks dirty, and one task commits. `NvDb_Commit()` stays
  for paths that must be durable before they return — arming an FWU install,
  a `wg` restart, a commanded reboot.
- **No `NvDb_Get`, no exported blob type, no JSON** — C10 and C13.
- **Three call shapes, because there are three object classes** (§1.2). Mirrored
  records get `Load`/`Store`; streamed objects get a handle and a cursor; the
  append class gets neither. Collapsing them into one API would violate Rule 1 —
  a 488 KB blob has no use for `offsetof()`, and a 60-byte record has no use for
  a stream handle.
- **`NvDb_Locate` and `NvDb_PanicWrite` are the exception contract in code.**
  They are the *only* sanctioned bypass (§1.3), and `NvDb_Locate` is what keeps
  it a bypass of the machinery rather than of the authority: the caller still
  asks `nvDb` where to operate. Anything else calling `W25Q128_*` is a C1
  violation, including a fault handler.
- **`NvDb_Delete` takes an address, and Rule 1 forbids addresses** — the one
  unresolved tension in this note. It is reconcilable two ways: the value is an
  `nvDb`-space token the store itself handed out (in which case the rule holds
  unchanged — a caller may know what it needs to perform its operation), or
  delete is a maintenance call outside the consumer surface the rule governs.
  Which one it is depends on the storage model, and that decision is open
  (§4, §7).
- **The diagnostics block is not consumer API.** A section owner never needs to
  know how much room it has: it stores its struct and the call either succeeds
  or returns `nvdbRes_outOfBounds`. `NvDb_GetUsage`/`NvDb_GetSectionInfo` exist
  for the operator-facing layer, and putting them in front of a subsystem author
  is how §1.1 gets eroded one convenience at a time.
- **Nothing in the surface is a flash word.** No address, no sector, no page,
  no alignment, no erase, no `% 8`. `len_bytes` is a byte count and `offset` is
  a byte offset into a section — splitting a commit across 256-byte page
  programs and 4 KB erases is entirely below this line (C1–C3).

---

## 4. Details

> **Provisional — and now visibly undersized.** §4.1 and §4.3 were written when
> `nvDb` was a parameter store: a ≤512 B payload, mirrored in RAM, rewritten
> whole. Under §1.2 its largest client is a 488 KB streamed blob, so the sizing,
> the RAM mirror and the single-blob-with-a-directory model describe **one object
> class out of three**. They also predate §1.8 (a whole-blob rewrite gives a
> collector nothing to collect) and §1.9. **The storage model is the next
> decision, not a settled one** — an allocator over an address space, with
> per-class access shapes, is where `NvDb_Delete(address, size)` and §1.2 both
> point. What follows is the shape as it stood; §1 and §2 take precedence
> wherever the two disagree.

### 4.1 Storage

Two 4 KB sectors, A/B, alternating writes, highest valid sequence wins — the
idiom already proven in `Shared/Modbus/modbus_config_store.c`, and the fix for
Zhaga's D5. Free ext-flash space starts at `0x0010_4000` (confirmed against
`Shared/Fwu/bl_app_contract.h`; `EXT_FLASH_WG_CFG` ends at `0x0010_4000`).

```
0x0010_4000  nvDb A (4 KB)   header{magic,version,seq,size,crc32} + payload
0x0010_5000  nvDb B (4 KB)   same; writes alternate, newest valid wins
```

Payload target ≤ 512 B. Mirror in main SRAM (C35).

**Being external, and only external, decides four things** (§1.1):

- **The mirror is mandatory, not an optimization.** The part is not
  memory-mapped; there is no pointer read. Without a RAM mirror every
  `NvDb_Load` would be an SPI transaction, and the rule that a caller need not
  know it is talking to flash would be unaffordable rather than merely violated.
- **A commit is slow and contends.** `w25q128.c` allows `W25Q128_ERASE_TIMEOUT_MS`
  = 5000 for a sector erase, and serializes the bus with a mutex shared with OTA
  uploads, crash-log writes and the Modbus config compiler. A commit therefore
  cannot run in `tcpip_thread`, in an lwIP callback, or inside a Modbus lap —
  which is why §4.2's single-writer commit is a consequence, not a preference.
  On `defaultTask` it is safe: that task already kicks the 16.4 s IWDG.
- **Init ordering.** `W25Q128_Init()` must precede `NvDb_Init()` — the same
  dependency `WgTime_Init()` already has — so bring-up belongs in
  `App_DefaultTaskEntry`, not in `MX_LWIP_Init()`.
- **The bootloader can reach this chip, and must still not link `nvDb`** (C33).
  Sharing the medium is not a reason to share the code.

### 4.2 Commit policy

Recommended: **deferred single-writer commit.** `NvDb_Store` marks dirty;
`defaultTask` — which already runs reboot and golden-promotion jobs — calls
`NvDb_Service()` on its existing 1 s tick and commits after a short quiescent
period. This closes three questions at once: D6 disappears (nobody can forget
to flush), a multi-field edit coalesces into one erase+write (wear), and C36 is
satisfied without callers ever blocking on flash.

### 4.3 Layout, restructuring and free space

**The layout is data, not code.** Each section's allocation is a number the
build supplies, and the stored blob carries a **directory** describing the
layout it was written under:

```c
typedef struct {
    uint8_t  id;             /* eNvDbSection — PERSISTED, never renumber     */
    uint8_t  ownerVer;       /* schema version of that section's own struct  */
    uint16_t offset;         /* byte offset of the section in the payload    */
    uint16_t alloc_bytes;    /* space reserved for it                        */
    uint16_t used_bytes;     /* bytes its owner declares meaningful          */
} __attribute__((packed)) sNvDbDirEntry;   /* 8 B                            */
```

`alloc_bytes >= used_bytes` always; the difference is that section's
**headroom**, and it is the whole reason a later firmware can grow a struct
without moving anything else.

**Occupancy is declared, not detected** (C22). `used_bytes` is the owner's
`sizeof`, recorded at commit. Content cannot be used to infer it: an erased NOR
byte is `0xFF` and a defaulted one is `0x00`, but a parameter legitimately
holding 0 or 255 is indistinguishable from either. Zhaga infers exactly this way
— `utl_MemFillCheck(..., 0x00, ...)` in `InitLuminare` — so a luminaire whose
parameters are all genuinely zero silently re-defaults on every boot. Declared
occupancy is what turns *"does the stored data fit the new layout"* into a
question with an answer.

**Restructure on boot.** `NvDb_Init()` compares the stored directory with the
layout this image was built with, and if they differ, rebuilds:

| Case | Action |
|------|--------|
| id in both, `alloc_new >= used_old` | copy `used_old` bytes to the new offset; zero the rest of the allocation |
| id in both, `alloc_new < used_old` | **will not fit** — that section alone is reset to its owner's defaults, and the outcome is recorded (C23) |
| id in the new layout only | fill from the owner's defaults |
| id in the stored blob only | dropped — this is how a parameter is removed (C18) |
| Σ `alloc_new` > payload capacity | refuse: the image is misbuilt, and `_Static_assert` (C19) should have caught it at compile time |

Section contents stay opaque bytes throughout — `nvDb` moves them without
knowing what they mean — so **adding, resizing, reordering or removing a section
needs no code at all** (C21). That is the entire difference between this and a
hand-written migration.

**Two kinds of change, two different owners.** Restructuring is `nvDb`'s job
because it is about *where bytes live*. When the meaning of bytes *inside* a
section changes — a field's units, a reinterpreted enum — that is the owner's
job, keyed off `ownerVer`, with `nvDb` handing over the old bytes and the old
version and nothing more. Keeping these apart is what stops §1.1 from eroding:
`nvDb` never learns what a parameter *is*.

**Power safety is already paid for.** The rebuild is composed in the same
staging buffer the commit path needs anyway, written to the **inactive** region,
and the selector flips (C14). A power cut mid-restructure leaves the old region
valid under the old layout, and the next boot simply tries again.

**Free space, both kinds.** The directory makes both exact:

- **within a section** — `alloc_bytes - used_bytes`: the headroom a struct can
  grow into with no restructure at all;
- **within the payload** — `capacity - Σ alloc_bytes`: the room for new
  sections.

Neither is visible to a section owner (C24, §1.1) — a subsystem stores its
struct and the store either accepts it or returns `nvdbRes_outOfBounds`. Both
are reportable to an operator through §3's diagnostics and belong in
`/api/config`: running out of parameter space should be something you watch
approach, not something you discover during an OTA.

**A restructure that reset a section is a real event** — settings were lost —
so its result is retained and reported the way `last_fwu_result` is in the boot
status, rather than emitted once into a Trice stream nobody was watching.

#### Why not the alternatives

The requirement to restructure decides an encoding question this note previously
left open:

| Option | Change the layout | Remove a field (C18) | Per-version code | Ids persisted |
|--------|-------------------|----------------------|------------------|---------------|
| Fixed struct only (Zhaga) | impossible — offsets are compiled in | never; leaves `unusedN` forever | one step per bump, knowing every historical layout | no |
| **Fixed structs + persisted directory** (required) | generic | yes, id retired | only for changes *inside* a section | **yes** |
| TLV per parameter | generic | yes | none | yes, per key |

Fixed-struct-only cannot satisfy C20 at all: with offsets compiled in, a blob
carries no record of the layout that wrote it, so a firmware cannot tell a
layout change from corruption. TLV would work, but it pays a per-parameter key
registry and per-parameter framing for a flexibility that is only ever needed
per *section* — the granularity at which parameters are actually added and
removed.

The cost is stated plainly: **`eNvDbSection` values are persisted and must never
be renumbered**, joining `eFwuRes`, `eModbusDecodeType` and `eCrashType` on that
list.

### 4.4 Suggested phasing

1. **`nvDb` core + host unit tests** over the existing NOR-faithful flash mock:
   A/B selection, torn write at every step, CRC rejection, defaults, and bounds
   rejection on `Load`/`Store`. **The restructure matrix is the bulk of it** —
   every row of §4.3's table, plus a torn write mid-restructure, a section that
   grows, one that shrinks but still fits, one that shrinks and does not, an id
   added, an id removed, and a reordered layout. All of it is host-testable with
   no board. No consumers yet.
2. **Migrate `wg_cfg` into it.** Cheapest first mover — its record is already
   versioned and CRC'd — and it *retires* a region rather than adding one.
   Decide §5's deployed-board question here (read the old region once, or
   accept one reset).
3. **Fold in MQTT and the Trice UDP destination**, which fixes
   reboot-loses-broker as a side effect.
4. **`GET`/`POST`/`DELETE /api/config`** composed from per-section serializers
   (C13), replacing the per-subsystem endpoints. This is also the backup /
   restore / provisioning path.
5. **Per-device WireGuard private key**, once there is a safe home for it —
   see §6 and Zhaga's `Calibration.c` for the "provisioning is not settings"
   precedent.

---

## 5. Boundary with the Modbus module

The module that most constrains this design, and the one whose storage `nvDb`
now takes over. Four points, all binding:

1. **`nvDb` absorbs the Modbus register config — and inherits its requirements.**
   Under Rule 2 the LUT A/B regions and the selector are `nvDb`'s like everything
   else. The reasons this note previously gave for leaving them alone — 16 KB
   compiled record streams, a hot-swap protocol, a distinct lifecycle — do not
   disappear; they become **requirements on `nvDb`**: it must offer a streamed
   object class (§1.2), A/B regions with an atomic selector flip, and a commit
   the client triggers at a boundary *it* chooses, since only the walker knows
   when a lap ends. `nvDb` owns the medium; it does not own the swap policy.
2. **Defaults: the two modules disagree on purpose.** `docs/modbus.md` Q6
   (closed 2026-08-11) removed built-in Modbus config entirely — a register map
   describes hardware this board may not have, so any built-in is a guess, and
   an empty config is a valid *unprovisioned* state. That doc explicitly grants
   the exception: *"A future unified parameter store may reasonably decide
   differently for parameters whose defaults are harmless; a register map's is
   not."* `nvDb`'s C12 (defaults are complete, always) is the harmless-defaults
   case, and a register map is not. Storage moving under `nvDb` changes nothing
   here: **`nvDb` owns where bytes live, never what they should contain.**
   Defaults remain each client's policy.
3. **Migration: also a deliberate divergence.** Modbus refused a v1→v2 migration
   because its configs are files in the operator's hands and re-uploading is
   cheaper than carrying a translator. `nvDb` parameters are *field-set with no
   file behind them* — there is nothing to re-upload — so C17 is required here
   even though it was correctly rejected there.
4. **Device parameters stay in the Modbus config.** `slaveAddr`, `baud`, `port`,
   `plan`, `topicPrefix` are per-device fields of the register-config record
   stream (`docs/modbus.md` §2.6), not system parameters. `nvDb` must never grow
   a "modbus baud" field; that would re-create the global baud that §2.6 exists
   to abolish.

---

## 6. Current state — clients to migrate

Under Rule 2 this stops being background and becomes **the migration list**:
every row is a client that today reaches the medium itself and must come to
reach it through `nvDb`. Ten files call `W25Q128_*` directly (C1).

**Persisted, each in its own private format** — seven regions, five idioms,
five separate validity checks, every address hand-assigned in
`Shared/Fwu/bl_app_contract.h` with no overlap check beyond review:

| What | Where | Owner | Idiom |
|------|-------|-------|-------|
| Boot status / FWU flags | `0x0000_0000` | `Shared/Fwu/boot_status.c` | NOR bit-clear flags outside a header CRC32 |
| Image metadata (filename) | `0x000F_5000` | `App/Img/image_store.c` | record bound by blob CRC32 |
| Crash log | `0x000F_8000` | `App/Log/crash.c` | magic + CRC32 record |
| Modbus register config | `0x000F_9000` / `0x000F_D000` | `Shared/Modbus/modbus_config_store.c` | A/B 16 KB record streams |
| Modbus active-region selector | `0x0010_1000` | same | NOR bit-clear selector sector |
| WireGuard time base | `0x0010_2000` | `App/Net/wg_time.c` | append-only 16 B slot ring |
| WireGuard network config | `0x0010_3000` | `App/Net/wg_cfg.c` | erase + rewrite one CRC32 record |

**Not persisted at all:**

| What | Where | Consequence |
|------|-------|-------------|
| MQTT broker IP / port / prefix | `App/Mqtt/mqtt_bridge.c`, RAM only | `mqtt set ip` is lost on every reboot |
| Trice UDP destination | `App/Log/trice_udp.c`, RAM only | `Trice_UdpRetarget` is lost on every reboot |

This is live, not hypothetical: the compiled-in MQTT default is `{10, 42, 0, 1}`
— the **bench** broker. A deployed board points at a nonexistent broker after
any reset until someone re-issues the CLI command over USB. `App/Net/wg_cfg.c`
(added 2026-08-08) fixed exactly this class of problem, but only for WireGuard —
which is what makes the inconsistency worth removing rather than repeating a
ninth time.

**What changes for each, and what does not:**

- **Boot status** — the one region `nvDb` does *not* take over as an ordinary
  client. The bootloader reads it, `nvDb` must never link into the 32 KB BL
  (C33), and its NOR bit-clear semantics exist precisely to mutate flags without
  an erase. It stays as it is, and it is the archetypal §1.3 case: the BL
  bypasses the machinery, the region is BL-visible and therefore **pinned**
  (C8).
- **Crash log** — stays a direct writer by necessity (fault context, §1.3), and
  becomes a locator client: `nvDb` may move it, `crash.c` must resolve where
  before writing, and `NvDb_PanicWrite()` is the sanctioned form of the bypass
  so it is a named exception rather than an undocumented hole.
- **Firmware blobs and image metadata** — become `nvDb`'s streamed class. This
  is the client that *justifies* deferred reclaim: `ImgStore_Delete` already
  tombstones a 488 KB blob for the cost of 2 erases instead of ~122 (§1.8).
- **Modbus register config** — §5, point 1.
- **WireGuard time base** — becomes the append class. Its 15-minute write rate
  is exactly why it must **not** be folded into the parameter blob, and equally
  why it is the most interesting subject for wear tracking (§1.9). Distinct
  object class, same owner.
- **Parameters** (`wg_cfg`, MQTT, Trice UDP) — the mirrored class, and the
  smallest client rather than the reason the module exists.

The line is no longer about what `nvDb` holds — it holds everything. It is about
how: **`nvDb` owns the medium, its address space, its reclamation and its wear;
each client keeps its own content policy, lifecycle and commit timing.**

---

## 7. Open questions

- **The storage model — the blocking one.** Three object classes (§1.2), a
  collector, and `NvDb_Delete(address, size)` all point at an allocator over an
  address space; §4.3's section directory assumes one blob rewritten whole,
  under which a collector has nothing to collect and a 488 KB client does not
  fit. Until this is settled, §4 is provisional and the §3 note on
  `NvDb_Delete`'s address argument cannot be closed. Everything in §1 and §2 is
  written to hold either way.
- **When does the bootloader learn to read the locator?** Until it does,
  BL-visible regions are pinned (C8) and `nvDb` cannot lay out the blob areas or
  boot status freely — which is most of the chip. This is a **bootloader change
  shipped ahead of the feature that needs it**, and the FWU rules make that
  expensive to get wrong: a BL that mis-reads the locator cannot be rolled back
  by the golden image, because finding the golden image is the thing it just
  got wrong. Sequencing this is its own task.
- **Does `nvDb` mediate reads as well as writes for exception clients**, or only
  publish locations? The BL streams a 488 KB install out of flash; routing that
  through anything is pointless, so the honest answer is probably "locations
  only" — but it should be stated, not assumed.
- **Wear tracking granularity** — the 4 KB sector is the W25Q64's minimum erase
  (`W25Q128_SECTOR_SIZE`), but 32 KB and 64 KB block erases exist too. Tracking
  the sector is finest and costs the most metadata.
- **A bit or a byte cleared per erase.** SPI-NOR parts specify a maximum number
  of partial-page programs between erases; thousands of sequential single-bit
  clears in one page is a far stronger use of the technique than
  `sBootFlags`' three. **Check the W25Q64 datasheet's NOP figure before
  committing to bit granularity** — clearing a byte instead costs only
  resolution (256 counts per page) and is unambiguously safe.
- **What tracked units are covered** — only `nvDb`'s own sectors, or the whole
  external flash? The FWU blob areas and `wg_time`'s ring wear far faster than
  parameters do, and they are the ones an operator would actually want to watch.
- **Collector pacing** — how often `NvDb_Service()` reclaims, and whether marked
  space is erased eagerly when idle or only when the space is needed (lazy
  erase, as `image_store` does). Trades wear-spreading against latency for the
  write that finds the space dirty (C28).
- **Where does the layout table come from** — a build-time table (recommended:
  `_Static_assert` can then prove every owner's struct fits its allocation, so a
  misbuilt layout cannot ship) or an uploadable one like the Modbus config? The
  restructure mechanism in §4.3 is identical either way; what differs is whether
  an allocation smaller than its owner's `sizeof` is a compile error or a
  runtime one. Note the limited upside of upload: headroom is only usable by an
  image whose struct is already bigger, and that is a build.
- **Does a section owner get a migration callback** for `ownerVer` changes
  (§4.3), or does a bumped `ownerVer` simply default that section? A callback is
  more capable; defaulting is one line and matches what the subsystems do today.
- **Directory bound** — how many sections, i.e. how large the directory may
  grow. At 8 B per entry this is cheap, but it is a persisted structure, so the
  cap wants choosing once rather than raising later.
- **Does the bootloader ever need to read any of it?** Current assumption: no,
  and C33 depends on that answer staying no.
- **Where does the per-device WireGuard private key live** — inside `nvDb`, or a
  separate provisioning region with different access rules? Zhaga's answer is a
  separate store (`Calibration.c`, §1.4) and the reasoning transfers: different
  write authority, different lifecycle. It must not reuse the FWU key
  mechanism, which is bootloader-only by design.
- **Does the AT24C02BN EEPROM (256 B, I2C) have a role?** On the board, unused
  by any code. Plausible home for immutable identity (serial, hwId) as distinct
  from mutable parameters — i.e. the `Calibration.c` role in different silicon.
- **Migration of already-deployed boards** — read the old `wg_cfg` region once
  and fold it in, or accept a one-time reset to defaults? Decide during phase 2.
- **Commit quiescence window** — how long `NvDb_Service()` waits before writing
  (§4.2). Trades flash wear against how long a setting is only in RAM.

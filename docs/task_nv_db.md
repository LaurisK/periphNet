# Task — `nvDb`, the application's only authority over non-volatile storage

**Status:** design settled through the API surface (§2). Internals in §4 are
provisional. Not implemented — no `nvDb` code exists yet.

**Supersedes** the design note on `dev_work` (`git show
origin/dev_work:docs/task_nv_db.md`), which described `nvDb` as a parameter
store with a RAM mirror, A/B regions, CRCs and per-class access shapes. That
model was abandoned: `nvDb` does not store parameters, does not protect data
and does not know what it holds. The Zhaga analysis and the defect list in
that note remain accurate and are condensed here in §1.7.

Reference implementation: `lusety-lamp-hw/ZhagaFW` @ `f6a69f4` —
`Dri/Settings.c/.h`, `Dri/Calibration.c/.h`, accessor layer in
`__Lamp_V6_02/Aplication.c`.

---

## 1. What `nvDb` is

`nvDb` owns the external flash address space. It decides where every user's
bytes live, moves them when the layout changes, refuses access outside a
user's own area, and presents each user a flat span of non-volatile memory
starting at zero. It does not read, interpret, validate, checksum, mirror or
repair anything.

### 1.1 The three rules

> **Rule 1 — a user of `nvDb` knows nothing it does not need in order to
> perform its operation.**
>
> **Rule 2 — `nvDb` is the only authority over the medium.** Where bytes live
> is its decision alone; there is no second placement authority.
>
> **Rule 3 — `nvDb` knows nothing about the data it holds that it does not
> need in order to keep it.**

**They are one idea seen from three sides.** Rule 1 bounds what a user may
know about the medium. Rule 3 bounds what the store may know about the user.
Rule 2 is what makes both invariants rather than conventions.

Rule 1 without Rule 3 gives a single door that every schema change has to be
taught, so adding a parameter becomes a store change — the coupling this
module exists to remove. Rule 3 without Rule 1 gives a store that hides
nothing and is merely ignorant. `nvDb` stores bytes, not meaning.

**Rule 2 is about authority, not execution.** The earlier note carved out two
"exception clients" that perform their own I/O; under the settled model that
carve-out is the general case. The bootloader and the crash handler are not
exceptional in kind — they simply obtain their locations from `nvDb` (§1.6)
instead of calling it to do the work.

### 1.2 The model

| Concept | Definition |
|---------|------------|
| **User** | A holder of storage, identified by `eNvDbUser`. One user, one area, always 1:1 |
| **Area** | A contiguous span of the medium belonging to exactly one user |
| **User address space** | `0x00 .. size-1`. Flat. No pages, no sectors, no alignment, no erase |
| **Absolute address** | Known only to `nvDb`, except where handed out under §1.6 |

A module may hold **any number of users** — it simply declares several. The
usual reason is **separable concerns, not redundancy**: a module's
configuration and its accumulated data differ in size, in lifecycle and in
when each should be wiped, so they are two users. Redundancy is *possible*
for a module that wants it, since two users never share an erasable unit, but
that is one use of multiplicity rather than its purpose. Typically a module
holds none to two.

Areas are **declared at build time** and never created, freed or resized at
runtime. The build table is the layout; `_Static_assert` proves it fits the
medium.

### 1.3 What `nvDb` promises, and what it does not

**It promises isolation.** Distinct areas never share an erasable unit, so no
operation by one user can reach another user's bytes — including an erase.
The user id is therefore a capability, not a label: a user with an arithmetic
bug can corrupt only itself.

**It does not promise durability of a user's own overwrite.** See the
accepted risk in §2.5. A user that needs power-cut-safe storage can declare a
second user and alternate between them — isolation is what makes that work,
and neither user learns why — but that is a use of multiplicity, not the
reason for it (§1.2).

**It does not validate anything.** Each user is responsible for the validity
of its own data. A user that stores garbage stores it durably, and that is
correct behaviour. Corruption *inside* an area is invisible to `nvDb`.

**It self-heals nothing.** There is no repair path, because there is nothing
`nvDb` could repair without understanding what it holds.

### 1.4 What a user never learns

Not merely hidden — unreachable, with no API through which a user could ask:

- that the store is flash at all, let alone external, SPI or NOR;
- any absolute address;
- erase granularity, program granularity, page boundaries, alignment;
- that an erase exists, or that its write cost one;
- where its area sits, that other areas exist, or how many;
- that its area was ever moved, grown, shrunk or newly placed;
- how much of the medium is spare;
- wear;
- SPI bus contention or the driver mutex.

**The rule cuts both ways.** It says *exactly* what is needed, not as little
as possible. Hiding something a user does need is the same violation and the
more dangerous direction, because the user cannot see what it is missing. The
things a user *does* need are in the contract table (§2.3) and nowhere else.

### 1.5 What `nvDb` never learns

- what any bytes mean, or where a field begins and ends;
- any user's type, schema or struct definition;
- whether a value is valid, in range or plausible;
- what a default should be;
- how to serialize anything;
- that two users' data are related, or that two users are one module;
- whether stored bytes are still wanted — only `NvDb_Delete` says that, and
  it is declared, never inferred.

What it **may** know: a user id, a size, a bound, a declared delete range, and
the erased state of the medium itself.

### 1.6 The two exceptions

Two clients cannot call `nvDb` to perform their I/O:

- **The crash handler** runs in fault context, where the RTOS may be dead and
  an SPI transaction may have been in flight. It resets the peripheral by hand
  and writes directly.
- **The bootloader** cannot link `nvDb` at all — 32 KB, no RTOS — and runs
  before the application exists.

**Both obtain real addresses; neither obtains them the same way.**

| Client | How it learns where to write |
|--------|------------------------------|
| Crash handler | `NvDb_GetAbsoluteAddress()` — in the application, always in step with the `nvDb` that placed it |
| Bootloader | **Not from `nvDb`.** The FWU module resolves through `nvDb` and publishes what the BL needs, in a structure the BL already understands. **The BL has no knowledge of `nvDb`** |

That indirection matters: because the BL never reads `nvDb`'s own structures,
**the directory format is not frozen** and can evolve freely. Only the
FWU→BL handoff is BL-visible, and it is small, app-owned and versioned
independently.

Everything a client does with an address after `NvDb_GetAbsoluteAddress`
returns is that client's own implementation. The call lives in its own header
(§2.2) so that "exception modules only" is enforced by what a file includes,
not by a comment.

### 1.7 What the reference implementation gets wrong

Copy the shape; do not copy the decay. Each is visible in `ZhagaFW` today.

| # | Defect | Where |
|---|--------|-------|
| D1 | `unused5` … `unused9` — a raw struct with no layout record can only append; removed fields stay forever as padding | `Settings.h` |
| D2 | Opaque `uint32_t` arrays with hand-computed lengths; `timeCalibr`'s size comment reads `sizeof(uint8_t * 13)`, which sizes a *pointer* | `Settings.h:29-36` |
| D3 | No version field, so a layout change is indistinguishable from corruption and both recover by silent full reset | `Settings.c` |
| D4 | The `sizeof(Settings_t) % 8 == 0` requirement is enforced by a comment, not a `_Static_assert` | `Settings.h:41` |
| D5 | Single sector rewritten in place — a power cut loses **all** settings | `Write_Settings` |
| D6 | **Store without commit.** `nvDb_Store` wrote only the RAM mirror; flash needed a separate `Write_Settings()`. Four subsystems store and never flush | `luminare.c`, `profile.c`, `time.c`, `binXfer.c` |
| D7 | The accessor lived in `Aplication.c`; the store's own header declared neither `nvDb_Load` nor `nvDb_Store` | `Aplication.h` |
| D8 | `Read_Calibration()` reads the sector straight over the live global, so a failed CRC leaves the device running on flash garbage | `Calibration.c` |
| D9 | `Settings_Set_Default()` — a driver — calls up into `Lum_ResetParameters()` | `Settings.c` |

**How the settled design answers them.** D6 cannot occur inside `nvDb`
because there is no mirror and no commit: `NvDb_Write` returns when the bytes
are on the medium. It can recur in a *user* that keeps its own cache, so a
user holding a RAM copy owns that obligation explicitly. D1, D2, D3 and D8
are user-side concerns now (Rule 3). D5 is answered by two users, not by
`nvDb`. D7 and D9 are answered by §2 and by the port (§4.6).

---

## 2. API

Module prefix `nvdb`. Naming per `C coding standard.md` — `Module_ActionDetails`
PascalCase functions, `<modulePrefix><Category>_<value>` enum values,
mandatory unit suffixes on quantifiable parameters.

### 2.1 Surface

```c
/* --- identity ---------------------------------------------------------- */
typedef enum {
    nvdbUser_undefined = 0,     /* sentinel                                 */
    nvdbUser_crashLog,
    nvdbUser_fwuStored,
    nvdbUser_fwuGolden,
    /* ... declared by the build layout table ...                          */
    nvdbUser_last               /* sentinel — a count, never stored         */
} eNvDbUser;

/* --- results ----------------------------------------------------------- */
typedef enum {
    nvdbRes_undefined = 0,
    nvdbRes_ok,
    nvdbRes_notInit,            /* called before NvDb_Init()                */
    nvdbRes_badUser,            /* unknown id, or not permitted for the call */
    nvdbRes_outOfBounds,        /* offset_bytes + len_bytes exceeds the area */
    nvdbRes_noOperation,        /* len_bytes == 0 — nothing was asked for    */
    nvdbRes_refused,            /* layout rejected; the previous one stands  */
    nvdbRes_flash,              /* the medium failed                        */
    nvdbRes_last
} eNvDbRes;

/* --- lifecycle --------------------------------------------------------- */
eNvDbRes NvDb_Init       (void);

/* --- access ------------------------------------------------------------ */
eNvDbRes NvDb_Read       (eNvDbUser user,       void *buff,
                          uint32_t offset_bytes, uint32_t len_bytes);
eNvDbRes NvDb_Write      (eNvDbUser user, const void *buff,
                          uint32_t offset_bytes, uint32_t len_bytes);
eNvDbRes NvDb_Delete     (eNvDbUser user,
                          uint32_t offset_bytes, uint32_t len_bytes,
                          fNvDbEraseDone onDone);
eNvDbRes NvDb_Wipe       (eNvDbUser user, fNvDbEraseDone onDone);
eNvDbRes NvDb_GetSize    (eNvDbUser user, uint32_t *size_bytes);
```

**The completion callback.** A delete is deferred, so the only way a user can
know its bytes are actually gone is to be told:

```c
typedef void (*fNvDbEraseDone)(eNvDbUser user, uint32_t offset_bytes,
                               uint32_t len_bytes, eNvDbRes result);
```

`NULL` for a user that does not care, which is most of them. The callback
runs in the collector's lowest-priority context and **must not block**; it is
invoked outside `nvDb`'s lock, so calling back into `nvDb` from it is
permitted. When `NvDb_Delete` had to do the erase inline (§4.2), the callback
fires **before that call returns** — a user that keeps state in it must
tolerate re-entry.

**What it does and does not give you.** It confirms an erase *happened*; it
never promises one *will*. Marks do not survive a reset (§2.5), so a reboot
before the collector arrives means the callback simply never fires. A module
that must guarantee its bytes are gone — a key holder — implements that
itself; `nvDb` gives it the feedback to build on, not the guarantee.

### 2.2 The exception header

`NvDb_GetAbsoluteAddress()` is **not in `nvdb.h`**. It lives in its own
header, included only by the two modules of §1.6, so an ordinary user cannot
reach it by including the normal one — scope limited structurally rather than
by convention.

```c
/* nvdb_exceptions.h — crash handler and FWU only */
eNvDbRes NvDb_GetAbsoluteAddress(eNvDbUser user, uint32_t *addr_bytes,
                                                 uint32_t *size_bytes);
```

That is the whole consumer surface. **Nothing in it is a flash word** — no
address, no sector, no page, no alignment, no erase, no CRC, no commit.

`uint32_t` throughout: the largest area is a 488 KB firmware blob, so a
16-bit offset cannot address it, and one size vocabulary across the surface
is worth more than a few saved bytes.

### 2.3 Contract table

The standard puts Doxygen in the `.c`, so this table — not the header — is
where a user learns what it needs (§1.4, Rule 1's other direction).

| Call | Blocks? | Context | Before `Init`? | Notes |
|------|---------|---------|----------------|-------|
| `NvDb_Init` | yes | task | — | requires `W25Q128_Init()` first |
| `NvDb_Read` | **yes, up to seconds** | task | no | may wait behind an erase in progress |
| `NvDb_Write` | **yes, up to seconds** | task | no | may perform an erase; see §4.1 |
| `NvDb_Delete` | usually no | task | no | marks and returns; blocks only if the mark list is full (§4.2) |
| *(callback)* | — | collector task | — | must not block; may fire before `Delete` returns |
| `NvDb_Wipe` | no | task | no | marks and returns |
| `NvDb_GetSize` | no | task | no | |
| `NvDb_GetAbsoluteAddress` | no | **any, incl. fault** | no → `notInit` | separate header; RAM lookup once `Init` has run; no RTOS, no mutex, no allocation |

**Task context only**, except `NvDb_GetAbsoluteAddress`. No ISRs, no lwIP callbacks.
Nothing enforces this; it is a contract.

**Reads block too.** A read can wait behind a collector erase, so a
latency-sensitive caller — an HTTP handler serving a page, say — must treat
`NvDb_Read` as a blocking call, not a memory access.

### 2.4 Behaviour worth stating

- **A user that has no space has `size_bytes == 0`**, and every access fails
  `nvdbRes_outOfBounds`. Absence and emptiness are the same state; there is no
  "unprovisioned" flag, and none is needed. `NvDb_GetSize` is how a user
  discovers this without having to fail a call to learn it.
- **A read inside bounds always succeeds.** Never-written space reads as the
  erased value. A user distinguishes "mine" from "never written" by its own
  validation, never by asking `nvDb`.
- **Relocation is invisible.** If an area moved, its contents came with it. If
  it grew, the tail is erased space. If it shrank and truncated, the user's own
  validation fails and it defaults — which is the right outcome, reached
  without `nvDb` knowing anything.
- **`NvDb_Delete` and `NvDb_Wipe` normally return immediately.** The erase is
  deferred to the collector (§4.2), so a subsequent read may still see the old
  bytes. The one exception is below.
- **A write into space with a pending delete waits** for that erase, then
  writes, then returns.
- **`NvDb_Delete` blocks only when the mark list is full**, in which case it
  performs its own erase before returning. Its contract is therefore "returns
  immediately, except when the mark list is full, in which case it does the
  work" — the user that caused the pressure is the one that pays for it, and a
  delete is never silently dropped.
- **`len_bytes == 0` returns `nvdbRes_noOperation`.** Nothing was asked for,
  and reporting that is more useful than silently succeeding.
- **`offset_bytes == size_bytes` on a non-empty area returns
  `nvdbRes_outOfBounds`**, like any other offset past the end.
- **`NvDb_Init()` requires `W25Q128_Init()`**, which runs in `defaultTask` —
  so a user initialising from an earlier task sees `nvdbRes_notInit`. Accepted:
  users initialise after `nvDb`, not around it.

### 2.5 Accepted risks — documented, not fixed

> **A reboot during an overwrite loses that area's erasable unit.** Rewriting
> bytes that already hold data requires erasing the minimum erasable unit and
> writing it back. A reset in that window loses the other data sharing that
> unit — all of it belonging to the same user, since areas are unit-aligned
> and no user can reach another's. `nvDb` guarantees isolation between users,
> never durability of a user's own overwrite. **Writes into never-written or
> deleted space carry no such risk**, as they need no erase.

> **A delete is eventual, and does not survive a reset.** `NvDb_Delete` is
> byte-granular and real: the collector erases the affected erasable units and
> writes back whatever was *not* deleted, so a partial delete behaves exactly
> like a whole one, only more expensively. But the marks live in RAM — a reset
> before the collector reaches them loses the request, and those bytes are
> still readable afterwards. **A user that requires bytes to be gone before a
> call returns has no call that provides it** — see §6.

---

### 2.6 Layout identity, mode and status

A layout is not anonymous. It carries a **name and a version**, and the
configuration that supplies it carries the **mode** it is to be applied
under.

```c
typedef enum {
    nvdbMode_undefined = 0,
    nvdbMode_normal,            /* refuse unless the relayout is clean       */
    nvdbMode_forced,            /* apply anyway; truncation is accepted      */
    nvdbMode_last
} eNvDbApplyMode;

typedef struct {
    uint16_t       nvdbVer;             /* the module's own version          */
    char           layoutName[16];      /* name of the layout in force       */
    uint16_t       layoutVer;
    eNvDbApplyMode lastApplyMode;
    eNvDbRes       lastApplyResult;     /* outcome of the last application   */
} sNvDbStatus;

eNvDbRes NvDb_GetStatus(sNvDbStatus *out);
```

**Configuration arrives as JSON, is stored as a structure**, and lives in
`nvDb`'s own config area alongside the layout in force and the status
(§4.3) — the same shape as the Modbus register config, and for the same
reason: a human writes the source, the device holds the compiled form.

**`nvDb` is versioned, and so is the layout.** The module's version and the
layout's name plus version are separate facts — a new `nvDb` may be shipped
with an unchanged layout, and a new layout may be supplied to an unchanged
`nvDb`.

**Mode travels with the configuration, not with a call.** A layout is applied
at init (§4.4), so there is no runtime moment at which "forced" could be
requested. It is set by whoever authored the layout, where a reviewer can see
it.

**This is for the operator, not for a user.** An `nvDb` user never asks any of
this. The person who loaded a configuration and rebooted the board asks it —
because **loading can fail even when validation passed.** Validation at supply
time is advisory: users keep writing between then and the next init, so a
shrink that was safe when the configuration was checked may not be safe when
it is applied, and flash can fail during the copy besides. The status is how
the outcome is learned; `lastApplyResult` of `nvdbRes_refused` means the
previous layout is still in force.

**The status is persisted**, since its whole purpose is to be read after the
reboot that applied the layout. That makes `eNvDbRes` a stored type: like
`eNvDbUser`, its values are **never renumbered**.

### 2.7 The layout configuration

**Two shapes, deliberately almost the same.** What an operator supplies and
what the board reports back share a structure, so a config and its read-back
can be diffed by eye.

#### What is supplied

```c
typedef struct {
    eNvDbUser user;
    uint32_t  size_bytes;       /* as requested; rounding is internal       */
} sNvDbUserEntry;

typedef struct {
    char            name[NVDB_LAYOUT_NAME_LEN];
    uint16_t        version;
    eNvDbApplyMode  operation;          /* normal | forced                  */
    uint8_t         userCnt;
    sNvDbUserEntry  users[];
} sNvDbLayoutCfg;
```

```json
{
  "name": "periphnet",
  "version": 3,
  "operation": "normal",
  "users": [
    { "user": "crashLog",  "size": 4096 },
    { "user": "fwuStored", "size": 499712 }
  ]
}
```

#### What is read back

The same structure, with four differences:

1. **No `operation`.** Once a layout is in force the mode is history; the
   status (§2.6) carries `lastApplyMode`.
2. **Every user the image knows is listed.** One that the layout in force does
   not place appears with `"size": 0` — the same fact `NvDb_GetSize` reports
   (§2.4), said in the same way.
3. **A final `freeSpace` entry**, in the ordinary entry shape. It is
   **read-only**: supplying it is a parse error.
4. **An onboarding state**, and when a configuration has been taken aboard but
   not yet applied, its header — **name, version and operation only**, with no
   user list and no free space of its own.

```c
typedef enum {
    nvdbOnboard_undefined = 0,
    nvdbOnboard_none,           /* nothing is waiting                       */
    nvdbOnboard_validated,      /* one is waiting; it passed the advisory check */
    nvdbOnboard_forced,         /* one is waiting; it is marked forced      */
    nvdbOnboard_last
} eNvDbOnboardState;
```

```json
{
  "name": "periphnet",
  "version": 3,
  "users": [
    { "user": "crashLog",  "size": 4096 },
    { "user": "mqtt",      "size": 0 },
    { "user": "freeSpace", "size": 7340032 }
  ],
  "onboarding": "validated",
  "received": { "name": "periphnet", "version": 4, "operation": "normal" }
}
```

**`validated` does not mean "will apply."** It means the configuration passed
the advisory check when it was supplied. The binding check runs at the next
init and can still refuse (§4.4) — which is exactly why the status exists.

**One source of truth.** `NvDb_GetStatus()` fills the C structure of §2.6;
this JSON is that same state rendered for an operator, with the user table
added. Neither is derived from the other twice.

**Terminology.** "Onboarding" here means *a configuration coming aboard* —
supplied, not yet applied. It is unrelated to §4.4.1, which is the one-time
adoption of a board that predates `nvDb`; that section is named "first
adoption" to keep the two apart.

## 3. Criteria

An implementation is `nvDb` if and only if it satisfies all of these.

**Authority and opacity**

- **C1** — `nvDb` is the only authority over placement. No code outside it
  computes where anything lives; the exceptions receive locations *from* it.
- **C2** — Any offset, any length, inside a user's area. One byte at an odd
  offset included.
- **C3** — No granularity is visible anywhere in the surface: no page, sector,
  erase, alignment or wear.
- **C4** — The user id is the bound. `offset_bytes + len_bytes` is checked
  against that user's size before any access.
- **C5** — Distinct users never share an erasable unit.
- **C6** — `nvDb` never reads user content for meaning (Rule 3).
- **C7** — `nvDb` never validates, protects or repairs user data.
- **C8** — A user's absence is `size_bytes == 0`, never a special state.
- **C9** — Absolute addresses leave `nvDb` only through
  `NvDb_GetAbsoluteAddress`, declared in its own header, only for users
  flagged in the build table.

**Behaviour**

- **C10** — Relocation is invisible to users; no call reports it.
- **C11** — Delete and wipe return immediately; a write into space with a
  pending delete waits for that erase and then proceeds. Delete is
  byte-granular: collecting a partially deleted unit preserves the bytes that
  were not deleted.
- **C12** — A write into already-erased space performs no erase.
- **C13** — Wear is indication only. It never influences an allocation, a
  relocation, a write or a result code.
- **C14** — Occupancy is **observed**, at erasable-unit granularity: written
  units versus erased ones. It is used for reporting and to decide whether a
  shrink is safe (§4.4), and for nothing else. `nvDb` never asks what a user
  considers meaningful, and never infers meaning from fill.
- **C15** — Boot always succeeds. A refused layout leaves the layout in force
  unchanged; a user the layout in force does not place has size 0.
- **C15a** — A layout is validated before it is adopted, never while it is
  being applied. Refusal is never a route to data loss.

**Structure**

- **C16** — The layout is data: loaded and validated at `NvDb_Init()` and
  only there, and applied only there. `_Static_assert` proves a built layout
  fits the medium and does not overlap; a loaded one is checked at runtime for
  the same properties.
- **C17** — `eNvDbUser` values are persisted in the directory and are **never
  renumbered**. Append only. The `_last` sentinel is a count, not a stored
  value. `eNvDbRes` is persisted too (§2.6) and carries the same rule.
- **C17b** — `nvDb`'s own config area is pinned and is the bootstrap. A
  layout that places it elsewhere is refused in **any** mode: a forced layout
  may cost a user its data, never `nvDb` its ability to find anything.
- **C17a** — The outcome of every layout application is recorded and readable
  after the reboot that performed it. A configuration that was loaded but not
  applied must be discoverable by the operator, never inferred.
- **C18** — A layout change needs no per-user code. Areas are opaque bytes
  that `nvDb` moves without knowing what they mean.
- **C19** — The core is RTOS-free and host-testable; RTOS services arrive
  through a port (§4.6). Its API is declared in its own header.
- **C20** — `nvDb` never links into the bootloader. Own source list, out of
  `${SHARED_SOURCES}`, like `Shared/Modbus/`. The 32 KB budget is not
  negotiable.

---

## 4. Internals — **provisional**

Nothing in this section is visible through §2. It is recorded so the API can
be reviewed against a plausible implementation, not because these choices are
settled.

### 4.1 Read-modify-write

NOR clears bits but cannot set them, so overwriting bytes that already hold
data means: preserve the erasable unit, erase it, write it back. `nvDb` does
this **blind** — it preserves bytes without knowing what any of them are, so
Rule 3 is untouched.

- **The unit is buffered in main SRAM** (one erasable unit, ~4 KB), never
  CCM — CCM is CPU-only memory, so a buffer there could not be filled by SPI
  DMA, and CCM is ~91 % full besides. A small flash scratch holds
  only an atomicity marker, and that marker is **internal bookkeeping**: it
  protects `nvDb`'s own operations — relocation and directory updates, whose
  loss would strand every user at once — not user overwrites, which carry the
  accepted risk in §2.5.
- **Every write reads its target first.** If those bytes are already in the
  erased state, program directly and skip the erase entirely; otherwise the
  unit must be erased and its untouched bytes written back. This content check
  is mandatory, not an optimisation — it is what makes append-shaped use cheap
  and what a user buys by deleting ahead of time. It reads the *state* of the
  medium, never a wear counter (C13) and never the meaning of the bytes
  (Rule 3).

### 4.2 The collector

Deleted space is erased in the background so that erases stay off the write
path. That — not reclaiming freed areas — is the whole reason it exists: a
user that discards data it no longer needs is buying cheap writes later.

- **Marks are byte ranges, not whole units.** Collecting a partially deleted
  unit means erasing it and writing back the bytes that were *not* deleted —
  the same read-modify-write as §4.1, performed off the write path. Byte
  granularity is what makes `NvDb_Delete` a real operation rather than a hint.
- **Marks live in RAM only.** Losing them costs a slow write and an
  uncollected delete, never correctness; persisting them would mean writing
  metadata to track deletions, spending the very erases the scheme avoids.
- **The mark list is bounded and coalescing.** Adjacent and overlapping
  ranges merge, and a `NvDb_Wipe` collapses that user's marks into one. When
  the list is nevertheless full, the incoming `NvDb_Delete` performs its erase
  inline rather than being dropped or refused. Provisional size: 64 ranges,
  ~12 bytes each — an internal number, tunable without touching the API.
- **Lowest priority in the system**, one erasable unit per lock acquisition
  so a waiting writer gets in between units.
- **A blocked write pulls its own erase forward** rather than waiting for a
  lowest-priority task to reach it — otherwise "lowest priority" turns a
  bounded wait into an unbounded one. Worst-case write latency is then about
  two erases.
- The lock must be **priority-inheriting**, or a lowest-priority collector
  mid-erase blocks a high-priority writer with no way to be boosted.

### 4.3 `nvDb`'s own areas

`nvDb` is a user of itself, twice. It declares **two areas for its own
operation**, kept apart because they are written at wildly different rates
and one must not endanger the other.

| Area | Holds | Written |
|------|-------|---------|
| **Config / status** | the layout in force, a supplied layout awaiting the next init, the status of §2.6, and any other internal bookkeeping | rarely — a layout change |
| **Wear** | erase counts per tracked unit | constantly — every erase |

Separating them is the point: wear churn is the highest-write traffic on the
chip and it must not sit next to the one structure whose loss is
unrecoverable. Losing the wear area costs a statistic (C13); losing the
config area makes every user's intact bytes unreachable.

**The config area is at a fixed anchor and never moves.** It is the bootstrap:
everything else is placed by the layout that lives inside it, so its own
placement cannot come from there. It is the one immovable address in the
design.

**Both areas appear in the layout**, so their sizes are known and no user can
be placed over them — but their placement is pinned, not chosen. **A layout
that describes `nvDb`'s own areas as being somewhere other than where they
are is refused**, whatever the mode: a forced layout may destroy a user's
data, never `nvDb`'s ability to find anything.

**Configuration arrives as JSON and is stored as a structure** — the pattern
already used for the Modbus register config. Parsing is a supply-time
operation with a supply-time error (a bad field points at the field);
structural validation of the resulting layout is separate and follows it
(§4.4). `nvDb` serializes **its own** configuration and nothing else — a
user's bytes remain opaque (Rule 3, C13).

Not frozen (§1.6), so any of this can change without a bootloader in
lockstep.

### 4.4 Relocation

**The layout is loaded and validated at `NvDb_Init()`, and only there.** A new
layout may be *supplied* at any time; it is *applied* at the next init.
Validation runs on every init regardless of whether anything changed.

**Validation is structural only, never about content** (Rule 3): every area is
aligned to an erasable unit, no two overlap, each fits inside the medium, the
total fits, `nvDb`'s own areas are where they actually are (§4.3), and the
record itself checks out.

**Feasibility is part of validation.** A relayout is achievable only if free
space is at least as large as the largest area that has to move — both copies
of a moving area exist for the duration of its copy. That is the simple rule,
chosen deliberately over an ordering algorithm with a staging area; it may be
revisited if a layout ever needs to move a large area on a nearly full
medium.

**A refused layout changes nothing.** The layout currently in force stays in
force and every user keeps the area it already had. Refusal is never a route
to data loss — that is the whole reason a layout is validated before it is
adopted rather than while it is being applied.

**Validation happens twice, and only the second one binds.** A layout can be
checked when it is supplied, which is what lets an operator see a mistake
immediately. That check is advisory: users keep writing between then and the
next init, so a shrink that was safe when the layout was checked may not be
safe when it is applied — and flash can fail during the copy regardless. The
binding check runs at init, and its outcome is recorded in the status (§2.6)
so the operator can confirm after rebooting that the layout actually took.

**Two apply modes.**

| Mode | Behaviour |
|------|-----------|
| **Normal** | The new layout is adopted only if a **clean relayout** is achievable — every user's occupied extent fits its new area, and the moves can be performed. Otherwise the layout is refused and the current one stays |
| **Forced** | Overrides that protection: the layout is adopted even where data cannot be preserved. Users whose areas shrink below their occupied extent are truncated |

Forced exists because "refuse and keep the old layout" is the wrong answer
when the old layout is the thing being deliberately abandoned. It is a
decision made when the layout is authored, not at runtime.

**Resizing is normal, including shrinking.** An area may grow or shrink
between layouts without the layout being refused, provided the user's
**occupied extent** fits the new size. Occupancy is observed, not declared —
`nvDb` counts written units against erased ones (§4.5), which is the one thing
about a user's bytes it is allowed to look at.

**Data offsets are preserved.** A move relocates a user's extent as it stands;
offset `0x00` stays offset `0x00` and nothing is repacked. That is what makes
relocation invisible to users (C10) — their addressing is unchanged whatever
happened to the placement underneath.

**Areas are moved as opaque bytes**, so adding, resizing, reordering or
removing a user needs no code (C18).

**Relocation is entirely `nvDb`'s internal business**, and the constraint it
implies is accepted: moving a large area needs somewhere to move it to, so
the medium cannot be filled to the brim. That is a sizing rule on the layout,
not a feature to design around.

**Known limit of observed occupancy.** Trailing bytes a user legitimately
wrote as the erased value are indistinguishable from never-written space, so
a shrink may truncate them. They read the same either way while the area
holds them; what changes is that afterwards they are out of bounds. A user
that cares writes a marker, or declares a bigger area.

### 4.4.1 First adoption — how the first `nvDb` image takes over an existing board

**This needs no migration code and no "did it already run" flag.** The first
image carrying `nvDb` ships **two** layouts:

1. an **assumed-current layout**, hand-written to describe the board's
   existing hand-assigned map — boot status, blob areas, crash log, Modbus
   LUTs, WG time, WG config — as ordinary users;
2. the **target layout** this image actually wants.

At init, finding no directory on the medium, `nvDb` adopts the
assumed-current layout as the state of the world and then performs an
ordinary relayout to the target. Existing data is carried across by the same
mechanism that serves every later layout change. On the next boot the
directory already records the target layout, so nothing happens — the
"already ran" question answers itself, and resumability after a power cut is
whatever relocation already guarantees.

A factory-fresh board costs nothing: the assumed areas hold erased space, so
the relayout moves nothing.

**Sequencing note.** The assumed-current layout can only be dropped from the
image once no board can still be running the pre-`nvDb` map — a board that
skips the onboarding image and lands on a later one finds no directory and
adopts whatever assumed-current layout *that* image carries.

### 4.5 Wear and occupancy

Both are reporting only. Erase counting rides in the one function that
performs every erase, so it needs no cooperation from users and no separate
mechanism. Occupancy is erased-unit versus written-unit counts. Neither ever
influences an operation (C13), which is what allows both to be cheap and
lossy.

### 4.6 The port

The core is RTOS-free so the host unit tests can drive it with no board.
RTOS services arrive through a small port in the application — a lock, an
unlock, and a lowest-priority context for the collector — the pattern
`App/Net/wg_platform.c` already uses for WireGuard.

---

## 5. Users — the migration list

Every row reaches the medium itself today and must come to reach it through
`nvDb`, or to obtain its location from it.

| What | Owner today | Becomes |
|------|-------------|---------|
| Boot status / FWU flags | `Shared/Fwu/boot_status.c` | BL-visible; location published by FWU (§1.6) |
| Stored + golden `.pnfw` blobs | `App/Img/image_store.c` | two users; BL learns where from FWU |
| Image metadata | `App/Img/image_store.c` | a user |
| Crash log | `App/Log/crash.c` | a user; writes in fault context after `NvDb_GetAbsoluteAddress` |
| Modbus config A/B + selector | `Shared/Modbus/modbus_config_store.c` | two or three users; the swap is the module's own policy |
| WireGuard time base | `App/Net/wg_time.c` | a user; append-shaped, so its writes stay on the fast path |
| WireGuard network config | `App/Net/wg_cfg.c` | a user (two if it wants power-cut safety) |
| MQTT broker / topic prefix | RAM only today — lost on every reboot | a user |
| — | — | **`nvDb`'s own config/status area** — pinned, never moves (§4.3) |
| — | — | **`nvDb`'s own wear area** — separate so its write rate cannot endanger the config (§4.3) |
| Trice UDP destination | RAM only today — lost on every reboot | a user |

The last two are live defects: the compiled-in MQTT default is the bench
broker, so a deployed board points at a nonexistent broker after any reset
until someone re-issues a CLI command over USB.

**Large users must delete before they write.** Uploading a 488 KB blob into
an area still holding the previous one means every write finds occupied bytes
and drags an erase onto the write path (§4.1). `NvDb_Wipe` first, and the
collector turns those erases into background work — which is the whole point
of the deferred delete and the reason `image_store`'s existing lazy
sector-erase behaviour is preserved rather than regressed.

---

## 6. Open items

**Still to decide:**

- **How a layout is supplied.** By analogy with the Modbus config this is an
  HTTP endpoint plus a CLI command, with JSON parse errors pointing at the
  offending field and the structural check reported separately. The schema is
  settled (§2.7); the transport is not.
- **Whether `freeSpace` gets a reserved `eNvDbUser` id.** It appears in the
  read-back in the ordinary entry shape but is never supplied and never
  stored, so a reserved id keeps the shape uniform at the cost of a value in
  a persisted enum that names nothing on the medium.

**Belongs to another module, parked with its owner:**

- **How the bootloader learns where the firmware blobs are** — the FWU module
  reads their locations from `nvDb` and writes them somewhere the BL already
  understands. What that structure looks like, and what the BL does when it is
  missing or stale, is FWU's business, not `nvDb`'s (§1.6).

**Internal, deliberately left until the core runs** — none of these change a
signature, so none of them blocks the header:

- Operator-facing reporting of occupancy and wear.
- How hard the collector works and how often.
- The directory's exact fields, its CRC and its anchor address (§4.3).
- Whether 32 KB / 64 KB block erases are used for large areas.

**Limits accepted rather than solved:**

- A user cannot tell a cheap operation from an expensive one. A small delete
  costs *more* than a large one — it forces the collector to preserve its
  neighbours — and nothing tells the user so. That is the price of hiding
  granularity, paid knowingly.
- The bound is checked but the type is not: `void *` in, `memcpy` out. A user
  passing the wrong struct to the right id is accepted if it fits. That is the
  price of a byte-addressed interface, and it is the right price.
- A guaranteed erase is the user's own problem. `nvDb` reports when an erase
  completed (§2.1) and never promises that one will.

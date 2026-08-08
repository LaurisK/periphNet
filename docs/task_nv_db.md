# Task note — `nvDb`, one non-volatile store for all system parameters

Planning note only. Design decisions are deliberately left open at the end;
the detail gets settled when this is actually planned and implemented.

**Problem in one line:** system parameters are scattered across per-subsystem
flash regions, each with its own record format, its own validity rules and its
own API — and some parameters are not persisted at all.

## 1. Evidence — the current state

### Persisted, but each in its own private format

| What | Where | Owner | Idiom |
|------|-------|-------|-------|
| Boot status / FWU flags | `0x0000_0000` | `Shared/Fwu/boot_status.c` | NOR bit-clear flags outside a header CRC32 |
| Image metadata (filename) | `0x000F_5000` | `App/Img/image_store.c` | record bound by blob CRC32 |
| Crash log | `0x000F_8000` | `App/Log/crash.c` | magic + CRC32 record |
| Modbus register config | `0x000F_9000` / `0x000F_D000` | `Shared/Modbus/modbus_config_store.c` | A/B 16 KB record streams |
| Modbus active-region selector | `0x0010_1000` | same | NOR bit-clear selector sector |
| WireGuard time base | `0x0010_2000` | `App/Net/wg_time.c` | append-only 16 B slot ring |
| WireGuard network config | `0x0010_3000` | `App/Net/wg_cfg.c` | erase + rewrite one CRC32 record |

Seven regions, **five different persistence idioms**, five separate "is this
valid" checks. Every addition costs a hand-assigned address in
`Shared/Fwu/bl_app_contract.h` — a manual allocation table with no overlap
check beyond review.

### Not persisted at all — parameters that should be

| What | Where | Consequence |
|------|-------|-------------|
| MQTT broker IP / port / prefix | `App/Mqtt/mqtt_bridge.c` RAM only | `mqtt set ip` is lost on every reboot |
| Trice UDP destination | `App/Log/trice_udp.c` RAM only | `Trice_UdpRetarget` is lost on every reboot |

This is live, not hypothetical: the compiled-in MQTT default is
`{10, 42, 0, 1}` — the **bench** broker. A deployed board points at a
nonexistent broker after any reset until someone re-issues the CLI command over
USB. `App/Net/wg_cfg.c` (added 2026-08-08) fixed exactly this class of problem
for WireGuard, but only for WireGuard — which is what makes the inconsistency
worth removing rather than repeating a ninth time.

### Consequences

1. **No versioning or migration anywhere.** Change a persisted struct and an
   OTA'd image reads old bytes. The magic/version checks catch it and fall back
   to defaults — i.e. the failure mode is *silent config loss*, not corruption.
   Acceptable once; not acceptable as the device accumulates field settings.
2. **No backup/restore.** There is no "dump all parameters" or "restore all
   parameters" operation, so provisioning a replacement board is manual and
   error-prone.
3. **Config surface is split across three transports** — CLI, HTTP, and
   build constants — with a different subset in each.
4. **Per-device provisioning has nowhere to live.** The WireGuard private key
   is still a build constant shared by every unit; that needs a home, and it
   should not be a ninth bespoke region.

## 2. The Zhaga pattern — what to copy, what to fix

Reference: `~/Projects/Lusety/lusety-lamp-hw/ZhagaFW/Dri/Settings.c` / `.h`.

One struct holds everything, one CRC covers it, three functions serve it:

```c
typedef struct { Settings_n_t settings; uint32_t crc; } Settings_t;

int Read_Settings       (Settings_t *s);   /* read sector, verify CRC */
int Write_Settings      (Settings_t *s);   /* compute CRC, write sector */
int Settings_Set_Default(Settings_t *s);   /* memset + fill defaults */
```

**Worth copying:** one struct, one CRC, one API, one place. Callers never touch
flash. Defaults live in exactly one function, so "factory reset" is trivial and
always complete.

**Worth fixing — visible decay in that same file:**

- `unused1` … `unused9` scattered through the struct. With a raw struct and no
  schema, fields can only ever be appended; removed ones must stay forever as
  padding. Nine of them already.
- Opaque `uint32_t` arrays with hand-computed lengths — `profiles[144]`,
  `apnList[165]`, `timeParam[7]`, `luminare[13]` — cast back to real types at
  use, with the size arithmetic living in a trailing comment. Change a
  sub-struct and the comment silently becomes a lie.
- **No version field**, so no migration path — only "CRC mismatch → defaults".
- The size constraint (`sizeof % 8 == 0`, for G0 doubleword writes) is enforced
  by a comment rather than a compile-time assert.
- Single sector rewritten in place: a power cut mid-write loses all settings.

`nvDb` should be the Zhaga pattern with a version field, real sub-structs, a
static assert, and A/B sectors.

## 3. Proposed shape

### Scope — what belongs in it

**In:** WireGuard (tunnel addr/mask, endpoint, keepalive, later the private
key), MQTT (broker addr/port/prefix, HA discovery prefix), Trice UDP
destination, network prefs (DHCP vs static), device identity (name, hwId), and
future BMS/CAN parameters.

**Out — deliberately:**

- **Boot status.** The bootloader reads it, `nvDb` must never link into the
  32 KB BL, and its NOR bit-clear semantics exist precisely to mutate flags
  without an erase. Leave it exactly as is.
- **Firmware blobs, image metadata, crash log.** Bulk artifacts, not parameters.
- **Modbus register config.** 16 KB compiled record streams with a hot-swap
  protocol — different size class, different lifecycle. Keep A/B as is.
- **WireGuard time base.** Writes every 15 minutes; config writes are rare. Its
  wear profile is the reason it is a ring, and mixing the two would drag the
  whole parameter blob into that write rate.

The line: **`nvDb` owns small, rarely-written, human-settable parameters.**
Bulk data and anything with special write semantics stays where it is.

### Storage

Two sectors, A/B, alternating writes, highest valid sequence number wins — the
same idea already proven in `modbus_config_store.c`, and the fix for Zhaga's
single-sector torn-write exposure. Free space starts at `0x0010_4000`.

```
0x0010_4000  nvDb A (4 KB)   header{magic,version,seq,size,crc32} + payload
0x0010_5000  nvDb B (4 KB)   same; writes alternate, newest valid wins
```

Neither valid → defaults, and the device still boots. Same rule as today's
`wg_cfg`.

### API sketch

```c
int                NvDb_Init(void);        /* load newest valid, else defaults */
const sNvParams   *NvDb_Get(void);         /* RAM mirror, read-only            */
int                NvDb_Commit(sNvParams *p); /* CRC + write inactive + flip   */
int                NvDb_SetDefaults(void); /* factory reset, whole blob        */
```

Subsystems read through `NvDb_Get()` and never touch flash. The RAM mirror must
stay small — **CCM is ~91 % full**, so this is a hard budget, not a
preference; a few hundred bytes, not kilobytes.

### Versioning

A `version` field plus one migration step per bump, so an OTA that changes the
layout upgrades field settings instead of silently resetting them. Sub-structs
get their own types (`sNvWgParams`, `sNvMqttParams`, …) rather than `uint32_t`
arrays with size comments, and a `_Static_assert` pins the total size.

## 4. Suggested phasing

1. `nvDb` core + host unit tests over the existing NOR-faithful flash mock
   (A/B selection, torn write, CRC rejection, version migration, defaults).
2. Migrate `wg_cfg` into it — its record is already versioned and CRC'd, so it
   is the cheapest first mover, and it retires a region rather than adding one.
3. Fold in MQTT and Trice UDP destination, which fixes the reboot-loses-broker
   bug as a side effect.
4. One `GET`/`POST /api/config` covering everything, plus `DELETE` for factory
   reset — replacing the per-subsystem endpoints. This is also the backup /
   restore / provisioning path.
5. Per-device WireGuard private key, once there is a safe home for it.

## 5. Open questions — for the planning discussion

- **Single versioned struct (Zhaga-style) or tag-length-value records?** The
  struct is simpler and matches the requested pattern; TLV survives
  add/remove/downgrade without migration code and preserves unknown keys, at
  the cost of a key registry. Recommendation: start with the struct, since the
  parameter set is small and the migration discipline is cheap while it stays
  that way.
- **Concurrency.** `http`, `cmd`, `mqtt` and `modbus` tasks can all write.
  Mutex inside `nvDb`, or a single writer task?
- **Commit granularity.** Whole blob per commit (simple, matches Zhaga) versus
  dirty-field tracking. Whole blob is likely fine given how rarely config
  changes.
- **Does the bootloader ever need to read any of it?** If yes, that part must
  stay in `Shared/` and stay tiny. Current assumption: no.
- **Where does the per-device private key live** — inside `nvDb`, or a separate
  provisioning region with different access rules? It must not reuse the FWU
  key mechanism, which is bootloader-only by design.
- **Does the AT24C02BN EEPROM (256 B, I2C) have a role?** It is on the board and
  currently unused by any code. Plausible home for immutable identity (serial,
  hwId) as distinct from mutable parameters.
- **Migration of already-deployed boards** — read the old `wg_cfg` region once
  and fold it in, or accept a one-time reset to defaults?

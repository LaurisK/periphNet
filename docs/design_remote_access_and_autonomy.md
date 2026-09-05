# Remote Access & Autonomy — Direction

> **Status: direction, substantially overtaken.** Written 2026-08-03. §1 and §2
> still frame the problem correctly and are why autonomy is a requirement. Most
> of the rest has either shipped or been superseded, and those sections have
> been replaced by pointers rather than kept as history:
>
> | Was | Now |
> |---|---|
> | §3 device as a WireGuard peer | **Done and live on both boards.** Hub facts: [reference_wireguard_hub.md](reference_wireguard_hub.md). Firmware behaviour: `CLAUDE.md` → *Remote Access*. The one open piece: [task_wg_endpoint_by_name.md](task_wg_endpoint_by_name.md) |
> | §3a field state (2026-08-07) | Stale — boards are `10.77.0.5` and `10.77.0.64` on the tunnel; see [design_solis_modbus_link.md](design_solis_modbus_link.md) §2 |
> | §4 MQTT not Modbus over the WAN | **Superseded.** Its premise (a pymodbus retry storm) was fixed upstream, and sunSale cannot be reached that way at all — [design_solis_modbus_link.md](design_solis_modbus_link.md) §1, §6.1 |
> | §5 bus budget | **Measured, and it fits.** sodas runs the Solis at 9600 alongside two JK at 115200 on one pair at 75 permille duty. Per-device baud multiplexing (modbus.md §2.6) deleted the problem instead of the second UART |
> | §8/§9 open questions and order | Answered or moved; what remains is below |

---

## 1. What the device is becoming

The long-term goal in `CLAUDE.md` says "RS485/Modbus-RTU to Ethernet/MQTT
bridge". The intended next step changes that materially:

- Poll the **Solis inverter** on RS485 (slave 1) — exists today.
- Poll a **JK BMS** on RS485 (different slave address) — protocol docs in
  `~/Projects/JK_BMS` (JK-PB series RS485-Modbus V1.1).
- Fuse/analyse both, and present a **synthetic Pylontech pack** to the
  inverter over CAN — `App/Can/bms_sim.c` already transmits Pylontech frames
  on CAN1, `bms_reader.c` parses them on CAN2.

That promotes PeriphNet from a bridge to a **control-plane element**. The
inverter drops the battery if the CAN frames stop or go stale, so the loop

```
BMS poll (RS485) → analysis → Pylontech CAN TX @1 Hz
```

is an obligation the device must meet with the WAN unplugged, HA rebooting
and the broker down. **Autonomy is a requirement, not a preference**, and it
is the constraint every decision below bends around.

Direct consequence: **nothing on the WAN path may be able to stall the RS485
bus.**

## 2. Topology

| Site | Contents | Reachability |
|------|----------|--------------|
| A — home server | HA, MQTT broker, WireGuard VM, strong HW | public entry point (dynamic DNS + port forward) |
| B — installation | Solis inverter, JK BMS, PeriphNet | NAT, outbound only |

Only **one** NAT has to be traversed: the home server is the rendezvous. No
VPS or third-party relay is required for any option. (Earlier analysis in
this session assumed both sides were unreachable and over-weighted
VPS-based relays accordingly.)

---

## 3. Register map generator (independent, high value)

`solis_modbus`'s `custom_components/solis_modbus/sensor_data/hybrid_sensors.py`
(4482 lines) is plain Python dicts grouped by `register_start`, with
`poll_speed` and per-entity `register` lists, `multiplier` and units. That
maps almost 1:1 onto PeriphNet's config JSON:

| solis_modbus | PeriphNet config |
|---|---|
| `register_start` | `startAddr` — **minus 30001** (3xxxx input) or **40001** (4xxxx holding) |
| `poll_speed` | `readPeriodS` |
| `register: [...]` length | `decodeType` (`u16` / `u32_be` / …) + `offset` |
| `multiplier` | `scale` (must land on an exact power of ten) |
| HA unit / device class | `unit` → DLMS code (`Shared/Modbus/modbus_units.c`) |

A generator script in `solis_modbus` that emits PeriphNet JSON gets their
curated, protocol-versioned map in without hand-porting, and keeps it
syncable as they add registers. `/api/modbus/config/upload` with its
field-level 422 rejects is exactly the right consumer for machine-generated
JSON. It can start any time, and it is what would populate the register
blocks the gateway link needs — [design_solis_modbus_link.md](design_solis_modbus_link.md) M3.

---

## 4. Gaps this opens

- **`App/Data/telemetry.c` has no producers or consumers.** It was reserved
  for a battery channel; `App/Pack/` took that role instead, so the file is now
  dead weight rather than a gap. Delete it or give it a purpose.
- **Staleness must fail safe.** If BMS data goes stale the emulated pack
  must clamp charge/discharge limits toward zero, not keep transmitting the
  last good values. Stale-but-plausible is the dangerous failure mode.
- **FC16 exists but atomicity across a point set does not.** The walker is
  gone and `Shared/Modbus/modbus_frame.c` emits `0x10`, but `Modbus_Request`
  is an array of *points* with no promise that a run becomes one frame — and
  Solis dispatch writes are silently dropped unless the block lands atomically.
  [design_solis_modbus_link.md](design_solis_modbus_link.md) §3.5, §7.3.
- **Flash is now the binding limit, not CCM.** Measured 2026-09-05:
  `.text + .rodata + .data = 477,980 / 491,520` — **97.2 %, 13.2 KB free**.
  Main SRAM is 96.8 % (4.2 KB free) and CCM 91.3 %. Budget before adding
  anything, and remember OTA is the only delivery path.

---

## 5. What is still open

1. **The `PeriphNet ⇄ solis_modbus` link itself** — the one missing link in the
   `Solis → PeriphNet → solis_modbus → sunSale` chain.
   [design_solis_modbus_link.md](design_solis_modbus_link.md) §7 holds the
   undecided parts, of which FC16 atomicity is the sharpest.
2. **Is the Solis COM port shared with the datalogger stick?** Still unanswered.
   sodas polls the inverter successfully, which is evidence the port is usable,
   not that it is exclusive.
3. **`App/Data/telemetry.c` still has no producers or consumers** (§7 above).
   The pack module took the role it was reserved for.

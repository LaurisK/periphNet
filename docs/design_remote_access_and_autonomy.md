# Remote Access & Autonomy — Direction

> Status: **observations + direction**, from a review session on 2026-08-03.
> Not an approved implementation plan — no phase sequencing, no interfaces
> frozen. Supersedes nothing yet; see §8 for what must be decided first.

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

## 3. Transport: device as a WireGuard peer

**Priority (decided 2026-08-07).** Add `wireguard-lwip` and make PeriphNet
itself a peer of the WireGuard hub at site A. The device carrying its own
tunnel — rather than relying on a VPN box on the local network — is the
target because the real deployment is a **board on a foreign LAN we do not
own and cannot add hardware to**; the board must phone home unchanged on any
network it lands on. The one gate on this being viable is footprint (below):
board-as-peer proceeds **unless the WG stack does not fit**, in which case we
fall back to a site gateway.

Why:

- **No new infrastructure and no new exposed port.** The WG UDP port is
  already open and is the most defensible thing to expose — it does not
  answer unauthenticated packets, so it is invisible to scanners. A custom
  relay port would be new, first-party attack surface.
- **Transport-transparent.** WG plugs in as an lwIP netif, so no
  application code changes: Modbus TCP, the HTTP OTA UI, MQTT to the
  home-server broker and Trice UDP all work over one tunnel.
- **Network-agnostic firmware.** The board behaves identically on the HA LAN
  and on a remote LAN — no "if same subnet, skip the tunnel" branch. The
  tunnel is always up; on-LAN traffic to the broker just takes the short
  path. Remote reachability becomes a property the *board* carries, not one
  the *site* must provide.
- **Immune to the `192.168.0.0/24` collision.** The board reaches the broker
  over its **tunnel IP (10.x)** and never touches the site LAN's subnet, so
  it works regardless of what private range the foreign LAN uses. (The
  collision only bites a roaming laptop — see §8/memory.)
- **Solves remote OTA/debug**, which is worth the work on its own and is
  independent of the Modbus/HA data path.

**Reachability direction — keep the control path broker-mediated.** Metrics
*and* actuation (HA→board control) ride MQTT: the board opens the connection
to the broker and subscribes to its command topics, so even HA-initiated
commands arrive down an **outbound, board-initiated** socket. That means the
board never needs an inbound port or a per-site NAT hole for normal
operation — inbound reachability is a **debug-only** convenience (HTTP OTA
UI, Trice). Treat "control must not require inbound reachability to the
board" as a hard design constraint; it is the main reason to keep the
control plane broker-mediated (§4) rather than letting HA open Modbus-TCP/HTTP
connections *to* the board, which would force inbound routing at the hub and
an accept path at every remote site.

Notes:

- Set `PersistentKeepalive = 25` device-side to hold the NAT mapping open.
- Rough cost: ChaCha20-Poly1305 + Blake2s + Curve25519 + netif ≈ 25 KB
  flash, small RAM. **Unverified estimate — build the port before trusting
  it.** Flash has room (`.text` 221 KB of 480 KB as of 2026-08-07); **CCM
  does not** — only ~6 KB of the 64 KB CCM region is free (`.ccmram` 11.4 KB
  + `.ccmheap` 48 KB). The footprint experiment (task step 1) must confirm WG
  handshake/peer state and crypto scratch land somewhere that fits; if scratch
  cannot go in main SRAM without overflowing it either, that is the trigger
  for the fallback.
- New crypto required — none of it exists in `Shared/Crypto` today (which has
  AES-128/GCM, SHA-256, HMAC): **Curve25519, ChaCha20-Poly1305, BLAKE2s.**
  These come with `wireguard-lwip`'s bundled primitives; do not hand-roll.

**Fallback (only if footprint fails): a WG gateway on the remote LAN.** A
small always-on box — SBC, or a VPN-capable/OpenWrt router — becomes the WG
peer instead, bringing the site (or just the board's address) back to the
hub. The board stays a plain LAN device with **zero VPN code**, which is why
this is the safe fallback: it cannot be blocked by the CCM budget. Cost is an
extra box to power and maintain per site, and it only works where we *can*
place hardware — the exact case board-as-peer exists to cover, hence
fallback-not-primary. Second-tier fallback (no box, no room): reverse TCP
tunnel — device dials `home.<ddns>:port`, a small home-server service glues
it to a local `:502`, PSK + AES-GCM framing reusing `Shared/Crypto` rather
than mbedTLS. Gateway-side glue is identical to the WG case, so it is a
stepping stone, not a detour.

### 3a. Field state (session 2026-08-07)

Verified on the site-A LAN (currently `192.168.0.0/24`, both dev pieces
co-located here):

- **PeriphNet board — `192.168.0.116`**, MAC `00:80:e1:...`, running
  `Pl1.0.8`, confirmed, HTTP API + Modbus config all responding. (Reachable
  today only because the laptop is on the same LAN — the whole point of this
  work is to reach it when it is not.)
- **WireGuard hub VM — `192.168.0.161`**, MAC `bc:24:11:...` (Proxmox
  QEMU/KVM NIC). Only **TCP 22** answers a scan; WG is UDP/51820 and stays
  silent, so whether it is actually listening and whether the router forwards
  UDP 51820 to it are **unverified — needs an SSH login to the VM to check
  `wg show`, `wg0.conf`, `ip_forward`, NAT rules, and the router port-forward.**
- Standardise on WireGuard and **retire the OpenVPN profiles**
  (`Zaliakalnis_DDNS` / `Zaliakalnis_IP`): they are dead anyway — the router's
  server uses BF-CBC, which OpenSSL 3 / OpenVPN 2.6 refuse without the legacy
  provider + `data-ciphers-fallback`, and NetworkManager cannot pass that
  (see memory `project-zaliakalnis-remote-access`).

**Immediate goal:** get the board reachable from outside this LAN. Concrete
build steps in `docs/task_board_as_wireguard_peer.md`.

### 3b. Developer access (independent of the board path)

Two paths, both kept — usability picks per session, and the second is
deliberate break-glass redundancy:

1. **Laptop as a WG peer of the hub** → reach both boards *and* SSH the dev
   VM, once on the tunnel. This is the entry point.
2. **SSH to a dev VM on the site-A LAN** as break-glass — but only genuine
   redundancy if it has **independent exposure** (key-only SSH port-forwarded
   / its own DDNS). If it rides the same VPN, a broken WG client locks out
   both. Keep it separately reachable.

## 4. Data plane: MQTT, not Modbus over the WAN

A **transparent** Modbus TCP gateway (the "be like Waveshare" feature) is
demoted from primary data path. A remote client — or a pymodbus retry storm,
which `solis_modbus`'s own README documents as a real failure mode (their
issues #395/#406) — would monopolise the bus the CAN emulation depends on.

Therefore:

- The **walker stays sole RS485 master**, with a priority tier that
  guarantees the BMS transaction a slot every lap.
- A Modbus TCP gateway is still worth having, but as a **queued client of
  the walker**: requests served in gaps, rate-limited, droppable under load.
  Diagnostics and occasional writes — not a 5 s poller.
- **MQTT is the HA data plane**, which is what `App/Mqtt/mqtt_bridge.c`
  already does.

This is what makes modifying `solis_modbus` worthwhile: give it a connection
type whose read path is PeriphNet's MQTT state topics and whose write path
is `<prefix>/<name>/set` plus a command/ack topic. It keeps its entity
model, curated register map, services, storage-mode select and dispatch;
PeriphNet keeps sole ownership of the bus; no Modbus crosses the WAN.

## 5. Bus budget — the binding constraint

More likely to bite than anything in §3–4. At 9600 baud a 47-register read
is ~100 bytes each way ≈ 200 ms with turnaround and the 3.5-char silence
(`modbus_rtu.c:118` uses 4 ms; RX end-of-frame 5 ms at `:154`). The default
config already issues 11 transactions. Adding the full Solis map **and** a
BMS poll that must complete comfortably inside the 1 Hz CAN cadence will not
fit.

Options, in the order I would reach for them:

1. **Put the BMS on its own UART.** `MODBUS_PORT_UART6` already exists in
   `modbus_rtu.h` as "not wired up yet — deferred until schematic review".
   This deletes the arbitration problem instead of managing it, and lets
   each bus run at its own baud.

   *Since 2026-08-10 this is what the Modbus module is being built for:*
   `docs/modbus.md` §2.5 makes ports a serviced set with an engine agnostic to
   them, so moving a device to a second bus is one config field. §2.6 goes
   further — baud is a **per-device** parameter, so a JK at 115200 and a Solis
   at 9600 can share one wire by time-multiplexing, which may defer the split
   rather than force it. §2.7's missed-sequence counter is the instrument that
   says when the budget has actually run out, instead of estimating.
2. **Raise baud.** JK PB-series does well above 9600; Solis is typically
   9600 and therefore the limiter — another argument for splitting.
3. **Do not ingest a full generated Solis map wholesale.** Generate, then
   select by poll class: small fast tier, everything else 60 s or on-demand.

To verify against the schematic: whether the JK BMS shares the inverter's
physical pair, and whether the Solis COM port is already occupied by the
datalogger stick. Three masters on one pair is a different conversation.

## 6. Register map generator (independent, high value)

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
JSON. This work is independent of §3 and §4 and can start any time.

## 7. Gaps this opens

- **`App/Data/telemetry.c` has no producers or consumers** — see audit doc
  item 5. It needs a battery channel so the RS485 BMS reader becomes the
  producer and `BmsSim` the consumer; today `BmsSim` is fed from
  `BmsReader` (CAN2) directly.
- **Staleness must fail safe.** If BMS data goes stale the emulated pack
  must clamp charge/discharge limits toward zero, not keep transmitting the
  last good values. Stale-but-plausible is the dangerous failure mode.
- **Walker writes are FC06 single-register only**
  (`ModbusWalker_WriteRegister`). Solis dispatch and storage-mode writes use
  **FC16 blocks** (`write_registers` in `modbus_controller.py:237`) and need
  readback plus a result code — so an MQTT command topic with request-id →
  ack, not fire-and-forget `/set`.
- **JK BMS decode may not fit the JSON config compiler.** Depends on whether
  its map is plain scaled registers or packed/variable-length structures —
  check against the V1.1 Modbus PDF before assuming the config path covers
  it.
- **CCM is ~91 % full** (audit item 1). The walker's per-ordinal arrays
  scale with config size, and a second bus plus BMS state adds more. Budget
  before adding config capacity.

## 8. Open questions

1. Separate UART for the BMS, or shared bus with priority scheduling? (§5 —
   decides the bus budget every other decision fits inside.)
2. Does `solis_modbus` get an MQTT connection type (§4), or do we keep
   pymodbus over WG and accept the two-masters cost?
3. Does the Modbus TCP gateway ship at all in the first pass, or only after
   the CAN loop is proven?
4. Is the Solis COM port free, or shared with the datalogger stick?

## 9. Suggested order

The generator (§6) and the WireGuard peer (§3) are independent of
everything else. But the first thing to build is the **BMS poll → telemetry
→ CAN path with the staleness fail-safe**: it is the piece with a real-time
obligation, and it sets the bus budget that constrains the rest.

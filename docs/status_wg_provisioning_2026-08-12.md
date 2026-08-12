# WireGuard provisioning + Trice — status (2026-08-12)

Session notes for `Pd1.1.0` … `Pd1.1.6`: what was built, what is proven on
hardware, what is still broken, and what to do next. Supersedes the
"per-device private key" and "Trice path" items of
[`status_remote_access_2026-08-08.md`](status_remote_access_2026-08-08.md) §5.

Boards referred to here:

| | board #1 | board #2 ("sodas") |
|---|---|---|
| Site | Zaliakalnis, `192.168.0.116` | bench, `192.168.8.114` |
| Firmware | `Pd1.0.9` | `Pd1.1.4` |
| Tunnel address | `10.77.0.64` | `10.77.0.5` |
| Hub peer | `PeriphNet_1` | `PeriphNet_sodas`, `10.77.0.5/32` |
| Public key | — | `ab3CVhJxWMtTLDzrf0Z8HvRPxVq4W0T75pd1cssDiyI=` |

## 1. Releases

| Version | Contents | State |
|---|---|---|
| `Pd1.1.0` | `.conf` upload + parser, cfg record v2, keys out of the image, route hook | superseded |
| `Pd1.1.1` | netif-teardown leak fix (§4.1) | superseded |
| `Pd1.1.2` | `POST /api/trice/dest` | superseded |
| `Pd1.1.3` | `GET /api/trice/status` (first counters) | superseded |
| `Pd1.1.4` | full Trice counters (UDP sent/failed, USB TxState, heap) | **on board #2** |
| `Pd1.1.5` | Trice USB TxState latch fix (§4.2) | built, **not flashed** |
| `Pd1.1.6` | per-peer WireGuard data-path counters (§3.3) | built, **not flashed** |

`1.1.1` … `1.1.4` were all deployed **over the network** (upload → install →
confirm → golden promotion), five consecutive cycles with
`last_fwu_result: 0`. The OTA path is solid on this board.

Sizes at `1.1.6`: `.text` 279,080 · `.bss` 67,432 · `.ccmram` 11,960
(≈4.4 KB CCM free) · bootloader untouched at 23,140. Host tests 8/8.

## 2. Verified on hardware

| Behaviour | Evidence |
|---|---|
| No WireGuard data in the image | `/api/wg/status` read `provisioned:false`, `config_source:"none"`, empty keys before any upload; tunnel refused to start |
| `.conf` upload through the existing image endpoint | one `POST /api/image/upload` of `PeriphNet_sodas.conf` → parsed, applied, persisted, tunnel up |
| Content sniffing does not break the blob path | `periphnet_fwu.pnfw` re-uploaded, 325,800 B, CRC `0x72CE7493` matching the build |
| Address and routes stay separate | `tunnel_mask 255.255.255.255` (the conf's /32) alongside `allowed_ips 10.77.0.0/24` |
| Public key derived on-device | `ab3CVhJx…` computed from the uploaded private key; matches the hub's peer |
| Config survives reboot | after reset: `provisioned:true`, `config_source:"stored"`, `config_version:2`, no re-upload |
| TAI64N clock advances across reset | `time_persisted 1786549184` → `time_now 1786549218`; hub accepted the fresh handshake |
| Handshake / session | `session_up:true`, sustained, across reboots and a dozen reconfigures |
| Board's **outbound** tunnel path | see §3.2 — `udp_sendto()` returned `ERR_OK` into the tunnel |
| USB CDC command input | `wg stop` over `/dev/ttyACM0` flipped `running` true→false |

## 3. Unresolved: the tunnel carries no data

### 3.1 Symptom

The tunnel handshakes and stays "active" on WGDashboard, but no inner traffic
is observed. From a laptop peer (`10.77.0.4`) with a working tunnel:

| Destination | Result |
|---|---|
| hub `10.77.0.1` | ✅ ~7–36 ms |
| `192.168.0.161` (dashboard host, via hub NAT) | ✅ |
| `192.168.0.116` (board #1) HTTP | ✅ 200 |
| **`10.77.0.5` (board #2) ICMP + HTTP** | ❌ silent, no reply, no ICMP error |

**"Active" on the dashboard proves only that handshakes succeed.** A WireGuard
handshake carries no inner addresses and is handled entirely in the transport
layer, so it stays healthy whether or not a single data packet crosses. The
dashboard cannot distinguish the two, and neither can `session_up`.

Both ends agree on the addressing, checked directly:

- hub peer `PeriphNet_sodas`: AllowedIPs `10.77.0.5/32`, active, endpoint
  `84.15.188.210:44678` (same public IP as the laptop — both were behind the
  same router).
- board: `tunnel_ip 10.77.0.5`, `allowed_ips 10.77.0.0/24`.

### 3.2 What is ruled out

- **Not an address mismatch.** The hub returns *Destination Host Unreachable*
  for `10.77.0.65`/`.66`/`.2`/`.3` but **silently accepts** `10.77.0.5` — i.e.
  a peer entry does hold `.5`. `.65` was tried and is not allocated.
- **Not the hub's forwarding, NAT or routing** — everything else through the
  same hub works, including board #1 on the far LAN.
- **Not the board's IP stack** — ICMP and HTTP on `192.168.8.114` respond in
  ~1 ms throughout.
- **Not the board's outbound tunnel path.** `Pd1.1.4` counted
  `udp_sent: 4, udp_failed: 2` for Trice datagrams aimed at `10.77.0.4`.
  Tracing the port: `wireguardif_output()` returns `ERR_RTE` with no matching
  peer, and `wireguardif_output_to_peer()` returns `ERR_CONN` with no valid
  session keypair. So `ERR_OK` means the route hook selected the WG netif, the
  peer was found by allowed-IP, a live session existed, the packet was
  encrypted and the encapsulated UDP was sent. **Board → hub TX works.**

That leaves the hub → board direction, or the board dropping inbound decrypted
packets.

### 3.3 The test that settles it (needs `Pd1.1.6`)

`Pd1.1.6` exposes the port's own per-peer counters in `/api/wg/status`:

```
peer_last_rx_ms   sys_now() of the last DATA packet IN   (0 = never)
peer_last_tx_ms   ... OUT
tx_packets        encrypted packets sent on this session
rx_counter        highest received counter (replay window)
live_endpoint     where the port is actually sending — follows the source of
                  the last valid handshake, so a roaming/NAT-remapped hub is
                  visible here and nowhere else
now_ms            for comparison against the timestamps
```

Ping `10.77.0.5` from a tunnel peer, then read the status:

- **`peer_last_rx_ms` never advances** → the board receives no data packets.
  Loss is hub→board: hub-side handling, or the NAT mapping not surviving
  between keepalives. Board-side code is not implicated.
- **`peer_last_rx_ms` advances, nothing comes back** → the board receives and
  fails to reply. Board-side; start at `ip4_input` acceptance with the `/32`
  netmask on the tunnel netif.

### 3.4 Access constraint

Board #2 is currently unreachable: it is not on the LAN the laptop moved to,
and every remote path into it (OTA, Trice UDP, HTTP over the tunnel) depends
on the broken data path. **Recovery requires someone on its LAN** — over HTTP
at `192.168.8.114`, no cable needed; a USB cable additionally gives the CLI.

## 4. Findings

### 4.1 `netif_remove()` on the WireGuard netif leaks (fixed, `1.1.1`)

`wireguardif_init()` allocates its `wireguard_device` with `mem_calloc()`,
creates a UDP PCB, and arms a **self-rearming `sys_timeout()` holding the
device pointer**. The port exposes no shutdown entry point to undo any of it.
So every `WgLink_Stop()` leaked one of only `MEMP_NUM_UDP_PCB` (8) PCBs, and
after a handful of stop/start cycles `udp_new()` failed, `wireguardif_init()`
failed, `netif_add()` returned NULL and the tunnel stopped coming back.
Freeing from app code is not an option either — the timer callback is static
to the submodule and would fire on freed memory.

**Fix:** the netif is created once and reconfigured in place —
`netif_set_addr()` for the address, `wireguard_device_init()` to re-key (it
rewrites only key material, leaving `netif`, `udp_pcb` and `peers` intact),
peer removed and re-added. `WgLink_Stop()` now does `netif_set_down()`.
Verified by a dozen consecutive reconfigures with no degradation.

### 4.2 Trice USB output latches busy forever (fixed, `1.1.5`)

`Pd1.1.4` reported `usb_tx_state: 1`. `CDC_Transmit_FS()` returns `USBD_BUSY`
whenever the CDC handle's `TxState != 0`, and `TxState` is cleared only by the
IN-transfer completion callback. `Trice_UsbInit()` runs at the top of
`App_DefaultTaskEntry`, well before USB enumeration completes, so the first
deferred flush submits a packet to an unconfigured device; that transfer never
completes and `TxState` stays 1 for the rest of the boot. Every later write is
dropped silently.

The symptom was maximally misleading: the **CLI kept working** (that is the
OUT endpoint) while the IN endpoint returned zero bytes, which reads as
"nothing is being logged" rather than "the sink is jammed".

**Fix:** `Trice_UsbWrite()` returns early unless
`hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED`, and clears a stale
`TxState` on the unconfigured→configured edge so a host that detached
mid-transfer cannot leave the endpoint latched. **Not yet verified on
hardware.**

### 4.3 Trice UDP was never broken — correction

An earlier conclusion in this session ("Trice UDP emits nothing") was **wrong**.
`Pd1.1.4` counted `udp_sent: 4` with `ERR_OK`. It transmits.

What actually happened: the destination is **RAM-only and reverts to the
compiled-in `10.77.0.4` on every reboot**, and every OTA rebooted the board,
silently undoing each retarget — while the listener watched the LAN. The
default destination being a tunnel address means the log stream disappears
exactly when the tunnel is the thing under investigation.
`docs/task_nv_db.md` already lists this non-persistence; it cost several
rounds of misdiagnosis here.

### 4.4 `[Interface] Address` and `[Peer] AllowedIPs` are different things

Every `.conf` gives the interface a `/32`; the routes live in `AllowedIPs`.
The old code derived the peer's allowed range from the tunnel address and
mask, which worked only because board #1's mask had been widened to `/24` by
hand. Feeding it a real `.conf` would have produced a tunnel that handshakes
and drops every packet.

`Pd1.1.0` separates them: `LWIP_HOOK_IP4_ROUTE` → `WgLink_Ip4Route()` walks the
configured ranges (a `/32` netif address can never satisfy lwIP's own subnet
match). Up to `WIREGUARD_MAX_SRC_IPS` (2) ranges are honoured. The hook
refuses to route the hub's own endpoint, so an over-broad `AllowedIPs` cannot
swallow the encapsulated UDP and deadlock the link.

### 4.5 HEAD did not compile

Two CubeMX-owned files still used enum names removed by commit `eabfa67`
("coding standard and adaptation"):

- `Core/Src/stm32f4xx_it.c` — `CRASH_NMI`, `CRASH_HARDFAULT`,
  `CRASH_MEM_MANAGE`, `CRASH_BUS_FAULT`, `CRASH_USAGE_FAULT`
- `USB_DEVICE/App/usbd_cdc_if.c` — `CMD_SRC_USB`

Both fixed in the working tree. Worth a rule: enum renames must sweep
`Core/` and `USB_DEVICE/` too, since they are outside the usual grep habit.

### 4.6 Probing which addresses a hub actually routes

Useful technique. Ping a tunnel address from another peer:

- **ICMP *Destination Host Unreachable* from the hub** → no peer holds that
  address (WireGuard returns `EHOSTUNREACH` when cryptokey routing finds no
  match).
- **silent drop** → a peer entry does hold it, but nothing is reachable behind
  it.

This enumerates the hub's allocation without dashboard or SSH access.

**Do not sweep in parallel.** Linux rate-limits ICMP error generation, so a
254-way burst suppresses the "unreachable" replies and makes every address
look routed. One sequential scan produced a completely fabricated picture
before this was noticed. Sequential probes only.

## 5. To do

**Immediate, needs someone on board #2's LAN**

1. Flash `Pd1.1.6` (`upload` → `install` → `confirm`, ~40 s).
2. Run the §3.3 test and record `peer_last_rx_ms`. This is the single
   measurement that splits the WireGuard problem in half.
3. Verify the §4.2 USB fix: Trice should decode over `/dev/ttyACM0` via
   `tools/usb_console.py`.

**Design work**

4. **Persist the Trice UDP destination**, and stop defaulting it to a tunnel
   address — a log sink reachable only over the link being debugged is a
   circular dependency. Part of `nvDb` (`docs/task_nv_db.md`).
5. **MQTT persistence + autostart.** Still CLI-only, RAM-only, and defaults to
   the bench broker `10.42.0.1`. A deployed board produces no telemetry after
   any reboot. No HTTP control surface exists for it at all.
6. **MAC address from the device UID.** Still the CubeMX constant
   `00:80:E1:00:00:00` on every unit — harmless while the boards are on
   different LANs, fatal the moment two share one.
7. **Second `AllowedIPs` range is implemented but untested.** Adding
   `192.168.0.0/24` should let a board reach site-LAN hosts directly; nothing
   has exercised it.
8. **Board #1 migration.** Its stored record is v1 and carries no keys, so
   after an OTA to `1.1.x` its tunnel stays down until its own `.conf` is
   uploaded. It remains reachable at `192.168.0.116` through the hub's LAN
   route in the meantime, so this is recoverable — but do not OTA it expecting
   the tunnel to survive unattended.
9. **Rotate board #1's key.** The `0DT2Rqlr…` key is in git history and in
   every `.bin` ever built from it.

**Hygiene**

10. Commit the working tree, including the §4.5 compile fixes.
11. `~/Downloads/*.conf` are mode `664` — world-readable files holding private
    keys. Move to `/etc/wireguard/` at `600`. `PeriphNet_1.conf` is stale and
    contradicts the live hub; delete or regenerate it.
12. WGDashboard's generated `Address` field has now disagreed with the hub's
    peer AllowedIPs twice. Treat the dashboard's peer entry as authoritative
    and the generated `.conf` as a starting point only.

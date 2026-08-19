# WireGuard provisioning + Trice — status (2026-08-12)

Session notes for `Pd1.1.0` … `Pd1.1.6`: what was built, what is proven on
hardware, what is still broken, and what to do next. Supersedes the
"per-device private key" and "Trice path" items of
[`status_remote_access_2026-08-08.md`](status_remote_access_2026-08-08.md) §5.

Boards referred to here:

| | board #1 | board #2 ("sodas") |
|---|---|---|
| Site | Zaliakalnis, `192.168.0.116` | bench, `192.168.8.114` |
| Firmware | `Pd1.1.11` — tunnel **working** | `Pd1.1.12` (2026-08-18) — tunnel **working**, see §3.13 |
| Tunnel address | `10.77.0.64` | `10.77.0.5` |
| Hub peer | `namai` (was `PeriphNet_1`) | `PeriphNet_sodas`, `10.77.0.5/32` |
| Public key | `U55gBRzFuUEIhFF6e8bPK3dlhKOa6nircpnEdKwWpCA=` | `ab3CVhJxWMtTLDzrf0Z8HvRPxVq4W0T75pd1cssDiyI=` |
| Provisioned | 2026-08-17, `session_up` ✅ | 2026-08-12 |

As of 2026-08-18 **both boards are live and both tunnels carry data.** Board #2
was recovered from its own LAN (§3.13) — the last action in this whole
investigation that needed physical presence.

## 1. Releases

| Version | Contents | State |
|---|---|---|
| `Pd1.1.0` | `.conf` upload + parser, cfg record v2, keys out of the image, route hook | superseded |
| `Pd1.1.1` | netif-teardown leak fix (§4.1) | superseded |
| `Pd1.1.2` | `POST /api/trice/dest` | superseded |
| `Pd1.1.3` | `GET /api/trice/status` (first counters) | superseded |
| `Pd1.1.4` | full Trice counters (UDP sent/failed, USB TxState, heap) | superseded |
| `Pd1.1.5` | Trice USB TxState latch fix (§4.2) | superseded (folded into `1.1.6`) |
| `Pd1.1.6` | per-peer WireGuard data-path counters (§3.3) | superseded |
| `Pd1.1.7` | **inner-packet checksums (§3.11)** — the fix that made the tunnel carry data; Trice destination list + `POST /api/trice/subscribe` | superseded |
| `Pd1.1.8` | pbuf-per-destination fix (§3.12) | superseded; was deployed **through the tunnel** |
| `Pd1.1.11` | Modbus module rebuild (unrelated to WG) | **on board #1**, confirmed + golden |
| `Pd1.1.12` | **ephemeral WireGuard source port (to-do 6b)** — `listen_port = 0` | **on board #2**, confirmed + golden (§3.13) |

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

## 3. SOLVED 2026-08-17: the tunnel carries no data

**Read §3.11 first — it is the answer.** Two independent faults, both ours:
missing inner-packet checksums (§3.11, the real one) and an endpoint collision
from binding the client socket to the server's port (§3.9). §3.1–§3.8 are the
investigation trail, kept because the eliminations in them are sound and the
misattributions are instructive — the hub was blamed twice and was innocent
both times.

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

### 3.5 SETTLED (2026-08-14): the board is exonerated, loss is board → hub

The §3.3 test was run on board #2 after OTA'ing it to `Pd1.1.6` from its own
LAN. Trice was first retargeted off the tunnel (`POST /api/trice/dest`
→ `192.168.8.100`) so `tx_packets` counted nothing but ICMP replies:

| | `peer_last_rx_ms` | `peer_last_tx_ms` | `tx_packets` | `rx_counter` |
|---|---|---|---|---|
| before | 117628 | 132413 | 1 | 0 |
| after 5 × `ping 10.77.0.5` | 139891 | 139892 | 6 | 5 |

`rx_counter` +5 and `tx_packets` +5, with `tx_ms` **1 ms after** `rx_ms`.
So the board **receives every echo request and answers every one of them**.
Not one reply reached the laptop.

The second half, measured at the same time: the laptop's own
`/sys/class/net/wgpn/statistics/rx_packets` did **not move** over 15 s while
the board's `udp_sent` rose 69 → 84 with `udp_failed` flat at 2 — i.e. lwIP
and the WG port accepted, encrypted and transmitted 15 datagrams that the
laptop's tunnel never decrypted.

**Both directions of §3.3's fork are now answered:** hub → board delivers
(§3.1's assumption was wrong), and board → hub → peer does not. Board-side
code is not implicated in either direction — `ip4_input` acceptance and the
`/32` netmask are ruled out, since replies are generated.

What remains is hub-side, and the prior "not the hub's forwarding" in §3.2 was
**too strong**: it rested on board #1, which sits on the hub's *LAN* and is
routed, not on peer-to-peer forwarding between two WireGuard peers. That path
was never actually tested. Note laptop → board forwarding demonstrably works,
so the failure is asymmetric — a plain "no `FORWARD` rule" does not explain it.

Leading candidate: at the time of the test the laptop (`192.168.8.100`) and
board #2 (`192.168.8.114`) were **behind the same NAT**, presenting one public
IP to the hub for two peers. Next test is therefore free: repeat from a
different site, where the two peers no longer share a public IP.

Needs hub-side evidence to close: `wg show` peer counters and a `tcpdump` on
the hub's `wg0`. No SSH credentials for the hub are recorded anywhere in this
repo.

### 3.6 (2026-08-17) same-NAT hypothesis disproven; fault isolated to the hub

Board #1 was provisioned as hub peer `namai` (`10.77.0.64`, dashboard-issued
`.conf` uploaded through `/api/image/upload`), giving a **second** board on a
**different site behind a different NAT** from the laptop — the variable §3.5
named as the leading suspect. It reproduces board #2 exactly:

| | `rx_counter` | `tx_packets` |
|---|---|---|
| before | 13 | 116 |
| after 5 × `ping 10.77.0.64` | 19 (+6) | 126 (+10) |

Laptop `wgpn rx_packets` **flat at 63** across the burst while the board pushed
~76 packets into the tunnel. **The same-NAT theory is dead.**

The same session narrows the fault decisively. Board #1's HTTP works over the
tunnel via the hub's LAN route (`192.168.0.116/32`), and that path *requires*
the hub to take a reply addressed to `10.77.0.4`, encrypt it to the laptop peer
and send it. So **hub → laptop encryption and forwarding work.** What fails is
only:

> packets arriving at the hub **from a board peer** are not forwarded.

Working, for contrast: hub → peer (both boards), peer → hub handshakes, and
hub ↔ its own LAN in both directions.

**Ruled out — the hub's `AllowedIPs`.** A receive-side cryptokey drop was the
first guess (a decrypted packet whose inner source is outside that peer's
`AllowedIPs` is discarded with no error). It does not survive the evidence:
for the ping to reach the board at all, the hub had to look `10.77.0.64` up in
`AllowedIPs`, match this peer and encrypt to it. That is the **same table** the
receive check consults, so a reply sourced from `10.77.0.64` passes it too.
Confirmed independently: `namai` is a **rename of `PeriphNet_1`**, not a second
entry, so there is no duplicate peer shadowing the address either.

**Ruled out — "the dashboard says it's up".** Per §3.1 that reports handshakes
only, and handshakes carry no inner addresses. Green is expected here and
carries no information about the data path.

What the asymmetry reduces to, given hub → laptop encryption works (proved by
board #1's HTTP over the LAN route) and laptop → board forwarding works:
the hub forwards `eth0 → wg0` fine and `wg0 → wg0` **in one direction only**.
That is not explicable by cryptokey routing, which is direction-symmetric —
it points at the hub's own `FORWARD`/conntrack rules or its `wg0` handling.

Next measurement, and it needs no SSH — **WGDashboard per-peer transfer
counters**, read while board #1 is transmitting (its Trice stream aims at
`10.77.0.4` continuously, so traffic is always available):

- `namai` **Received** rising → the hub *is* getting the board's packets;
  the loss is inside the hub, after decryption.
- `Lauris_laptop` **Sent** not rising in step → confirms the hub never
  re-encrypts them onward.

Also worth recording verbatim from the dashboard rather than inferring: the
literal `AllowedIPs` string on both peers.

Still open: no SSH credentials for the hub are recorded, so `wg show` and a
`tcpdump` on `wg0` remain ungathered.

### 3.7 The dashboard `.conf` reused the compromised key

`PeriphNet_namai.conf` (downloaded 2026-08-17) carries `PrivateKey =
0DT2Rqlr…` — the key to-do item 9 flags as being in git history and in every
`.bin` built from it. This is not dashboard misbehaviour: `namai` is
`PeriphNet_1` **renamed**, so it is the same peer and necessarily the same
key. The security consequence is unchanged, though — board #1 is live on a
key that must be treated as public. **Item 9 is NOT discharged.**
Rotation is `POST /api/wg/keygen` + pasting the new public key into the
dashboard; the tunnel is down between those two steps.

Extraction gotcha worth keeping: a base64 WireGuard key ends in `=`, so
`sed 's/.*= *//'` on a `PrivateKey = …` line silently yields the **empty
string**, and any comparison built on it reports a bogus mismatch. Split on
the literal `PrivateKey *= *` instead.

### 3.8 MEASURED (2026-08-17): packets reach the hub and die there — *cause misattributed, see §3.11*

WGDashboard v4.3 exposes per-peer transfer counters over its REST API
(`GET /api/getWireguardConfigurationInfo?configurationName=wg0`, header
`wg-dashboard-apikey`), on `192.168.0.161:10086`. That closed the last gap
without needing SSH. Over one 30 s window:

| Quantity | Δ |
|---|---|
| board `udp_sent` / `tx_packets` | **+30 packets** |
| hub **received from** `PeriphNet_namai` | **+2480 B** (≈83 B/pkt — matches) |
| laptop `wgpn` rx, quiet 25 s window | **+0 packets, +0 bytes** |

WireGuard's receive counter increments only for packets that decrypt **and
authenticate**. So the board's packets arrive at the hub, are valid, and are
lost after decryption. This is no longer inference: §3.6 reached the right
conclusion by elimination, and this measures it directly. Note the mechanism
precisely — WireGuard **ingress succeeds**; the failure is **forwarding, after
a successful receive**.

The hub's `PostUp` is correct and cannot be the cause:

```
sysctl -q -w net.ipv4.ip_forward=1
iptables -A FORWARD -i wg0 -j ACCEPT
iptables -A FORWARD -o wg0 -j ACCEPT
iptables -t nat -A POSTROUTING -s 10.77.0.0/24 -o eth0 -j MASQUERADE
```

`-i wg0 -j ACCEPT` accepts anything arriving on wg0 on first match, so both
directions hit the same rule. And both directions are `wg0 → wg0`
(laptop→board works, board→laptop does not), so the only difference between
them is the **source address**.

**Prime suspect: `rp_filter`.** It validates source against the routing table,
so it drops traffic from `10.77.0.64` if the hub has no route for it via wg0,
while `10.77.0.4` passes because the laptop's route exists. Settle it on the
hub console (Proxmox, no SSH):

```bash
sysctl net.ipv4.conf.all.rp_filter net.ipv4.conf.wg0.rp_filter
ip route get 10.77.0.4 from 10.77.0.64 iif wg0   # direct oracle
ip route show dev wg0                            # is 10.77.0.64/32 present?
```

`sysctl -w net.ipv4.conf.wg0.rp_filter=0` proves it instantly if so. Note a
wg0 restart was already tried on 2026-08-17 and did **not** fix it.

**Bug found in `PostDown`** (not today's fault, but it makes the chain drift):

```
iptables -A FORWARD -i wg0 -j ACCEPT;   <-- should be -D
iptables -D FORWARD -o wg0 -j ACCEPT;
```

Every down/up cycle leaves an extra `-i wg0 ACCEPT` behind.

Peer list also confirms `PeriphNet_sodas` is `stopped` / no handshake (board #2
powered down) and `PeriphNet_namai` holds the correct `10.77.0.64/32`.

### 3.9 ROOT CAUSE #1 (2026-08-17): endpoint collision — the board's source
### port equals the hub's listening port

`wg show wg0` on the hub (the live kernel view, which the dashboard's stored
copy had been hiding) showed board #1's peer as:

```
peer: U55gBRzFuUEIhFF6e8bPK3dlhKOa6nircpnEdKwWpCA=
  endpoint: 85.206.57.75:51820      <-- the HUB'S OWN public IP:port
  allowed ips: 10.77.0.64/32
```

**The hub believed the board lived at the hub's own public endpoint**, so every
packet it sent toward `10.77.0.64` went back out to the site router and
hairpinned into the hub's own listening socket. `ping 10.77.0.64` from the hub
returned 0/3 while `ping 10.77.0.4` returned 3/3.

Why it happens, and it will happen again on any board sharing a site with the
hub:

1. board #1 (`192.168.0.116`) and the hub (`192.168.0.161`) are on the **same
   LAN**, so they share the public IP `85.206.57.75`;
2. the board dials the hub's **public** endpoint, so the traffic hairpins
   through the router;
3. `App/Net/wg_link.c` binds the board's socket to
   `WIREGUARDIF_DEFAULT_PORT` = **51820** — the same port the router
   statically forwards to the hub.

The learned endpoint is then indistinguishable from the hub's own. Contrast the
laptop's healthy entry: `78.62.38.227:48972`, remote address, ephemeral port.

**Fix applied** (config only, no firmware change): point board #1 at the hub's
LAN address — `POST /api/wg/config {"endpoint_ip":"192.168.0.161",
"endpoint_port":51820}` — then `POST /api/wg/restart`, which is **required**:
setting the endpoint alone does not re-create the peer, and `live_endpoint`
keeps the old value until a fresh handshake. The hub then relearned
`192.168.0.116:51820`.

**Result:** hub → board data works for the first time — board `rx_counter`
8 → 15 across 5 pings (previously pinned at 0) and hub `sent` to namai +640 B
= 5 × 128 exactly.

**Better long-term fix (firmware, not yet done):** stop binding the board's
socket to 51820. A WireGuard *client* has no reason to listen on the server
port; `listen_port = 0` (ephemeral) removes the collision at any site and keeps
the portable public endpoint working. `wg_link.c:311`.

**Still broken after this fix:** board → hub → laptop. See §3.10.

### 3.10 Remaining fault after §3.9 — *not the hub's forwarding; see §3.11*

With §3.9 fixed, every leg is individually proven except one:

| Leg | Status |
|---|---|
| hub → board | ✅ measured, §3.9 |
| board → hub | ✅ rx counters climb |
| hub → laptop | ✅ +11 KB in the same window |
| **board → hub → laptop** | ❌ |

The hub receives the board's packets (`recv` +1440 B while the board's
`tx_packets` went 75 → 87) and delivers none onward: `ping 10.77.0.64` 0/5 from
the laptop, Trice listener unchanged.

Ruled out on the hub: `FORWARD` policy is `ACCEPT` and its `-i wg0` rule counted
1301 packets/442 K; `rp_filter = 2` (loose); `ip route get 10.77.0.4 from
10.77.0.64 iif wg0` resolves to `dev wg0`; `wg show` shows no overlapping
`allowed ips` (each peer holds a unique `/32`) and a valid laptop endpoint.

The oddity is that the working and failing directions are **both `wg0 → wg0`**
and differ only in addresses, which nothing examined so far distinguishes.
Next: `ping -c3 10.77.0.64` from the hub (now expected to succeed — it tests
board→hub local delivery with no forwarding), then
`tcpdump -ni wg0 -Q out 'host 10.77.0.4'` to see whether the kernel emits the
forwarded packet at all.

### 3.11 ROOT CAUSE #2 — SOLVED (2026-08-17): no checksums on inner packets

**The tunnel now carries data. The fault was ours, not the hub's.** §3.8 and
§3.10 correctly localised *where* packets died but wrongly concluded the hub
was at fault; it was doing exactly what any correct IP stack does.

The decisive measurement: the hub's `FORWARD` chain counter stayed at
**1383 → 1383** across 30 s while `tcpdump -ni wg0 -Q in` captured ~30 packets
arriving from the board. Visible to tcpdump, never reaching FORWARD, means
dropped in `ip_rcv()` — and the classic reason is a bad checksum, because
tcpdump captures *before* validation.

`LWIP/Target/lwipopts.h` (CubeMX default):

```c
#define CHECKSUM_BY_HARDWARE 1
#define CHECKSUM_GEN_IP   0
#define CHECKSUM_GEN_UDP  0
#define CHECKSUM_GEN_ICMP 0
```

Software checksum generation is off because the STM32 ETH DMA inserts
checksums in hardware. **That is only true for frames the ETH peripheral
emits.** The WireGuard netif has no hardware behind it: lwIP builds the inner
packet, wireguard-lwip encrypts it, and it travels as the *payload* of an outer
UDP datagram. The hardware checksums the outer datagram and never sees the
inner one — so every packet the board ever sent into the tunnel carried a
garbage inner checksum and was discarded by the peer's IP stack.

Every observation fits, including ones that looked contradictory:

| Observation | Explanation |
|---|---|
| handshakes fine, sessions stable | WireGuard transport packets are the *outer* UDP — hardware-checksummed |
| hub's per-peer rx counters climbed | WireGuard authenticates crypto; it does not check the inner IP checksum |
| `tcpdump` showed the packets | capture precedes checksum validation |
| `FORWARD` counter frozen | dropped in `ip_rcv()`, before the chain |
| laptop → board worked | Linux emits correct checksums, and our `CHECKSUM_CHECK_*` are all 0, so the board validates nothing |
| board #2 identical symptom | same firmware |

**Fix** (`Pd1.1.7`): compile software checksum generation **in**, enable
`LWIP_CHECKSUM_CTRL_PER_NETIF`, and clear the flags on the **ETH netif only**
(`App_DefaultTaskEntry`, via `netif_default`) so Ethernet keeps its hardware
offload unchanged. `netif_add()` defaults every netif to
`NETIF_CHECKSUM_ENABLE_ALL`, so the WireGuard netif needs no code of its own.
Cost: `.text` +880 B.

**Verified on hardware, 2026-08-17:**

- `ping 10.77.0.64` from the laptop — **5/5, ~8 ms** (was 100 % loss for a week)
- `POST /api/trice/subscribe` over the tunnel → `{"you":"10.77.0.4"}`
- Trice decoding live over WireGuard, `udp_failed 0`
- `Pd1.1.8` uploaded, installed and confirmed **through the tunnel itself**

### 3.12 Bug in the new Trice fan-out: never reuse a pbuf across sends

`Pd1.1.7` shipped the destination list with **one pbuf reused for all
destinations**, on the reasoning that `udp_sendto()` does not take ownership.
That is wrong on this platform: the STM32 ETH driver is **zero-copy** —
`low_level_output()` hands the pbuf to a DMA descriptor and holds a reference
until the TX-complete interrupt, so it is still in flight when `udp_sendto()`
returns.

Symptom: **every send that egressed the Ethernet netif failed** while the
WireGuard destination succeeded, because wireguard-lwip encrypts into a pbuf of
its own and never retains the caller's. Isolated by moving one destination at a
time — `255.255.255.255`, `192.168.0.255` and unicast `192.168.0.161` all
failed 1:1 with sends; only `10.77.0.4` (tunnel) succeeded. It was not a
broadcast problem, which was the first guess.

**Fix** (`Pd1.1.8`): allocate a fresh pbuf per destination. Verified — with
broadcast + tunnel configured, `sent +30, failed 0` over 15 s and Trice arriving
on both paths.

### 3.13 CLOSED (2026-08-18): board #2 recovered, §3.11 verified independently

Board #2 was flashed from its own LAN (`192.168.8.114`, HTTP only, no cable)
with `Pd1.1.12`. **This is the confirmation that §3.11 was the whole fault on
the board side** — board #1's recovery on 2026-08-17 changed the endpoint
(§3.9) *and* the checksums (§3.11) in the same session, so it could not
separate them. Board #2 is remote from the hub, so no endpoint collision was
ever possible for it and its endpoint was never touched: `85.206.57.75:51820`
before and after. The only variable was the checksum fix.

Before, on `Pd1.1.6`, from board #2's own LAN:

| | result |
|---|---|
| `ping 10.77.0.5` ×5 | **0/5, 100 % loss** |
| `rx_counter` / `tx_packets` after the burst | **0 / 0 — unmoved** |
| `session_up` | `true` throughout |

`rx_counter` pinned at 0 is *worse* than §3.5 measured on the same board in
August, where it climbed: with 4.3 days of uptime the hub had long since lost
whatever mapping made hub → board work back then. It restates §3.1's lesson —
`session_up` and a green dashboard track handshakes and are worth nothing as
evidence about the data path.

After, on `Pd1.1.12`, same LAN:

| | result |
|---|---|
| `ping 10.77.0.5` ×5 | **5/5, ~71 ms** |
| `rx_counter` / `tx_packets` | **14 / 15**, `rx`→`tx` 17 ms apart |
| HTTP over the tunnel | `GET /api/fwu/status` served on `10.77.0.5` |

Install took ~36 s (upload 5.9 s over LAN, `last_fwu_result: 0`). Confirm was
issued **over the tunnel**, and golden is now `Pd1.1.12` — so for the first
time board #2's rollback target is an image whose tunnel works. Previously a
rollback landed on `Pd1.1.6` and took remote access away with it.

**Remote acceptance, from a different site** (laptop moved to `192.168.0.147`,
board #2's LAN unreachable — `192.168.8.114` down, confirming the path is
genuinely the tunnel):

| Check | Result |
|---|---|
| `ping 10.77.0.5` ×40 | **40/40, 0 % loss**, 44–95 ms |
| `/api/fwu/status`, `/api/wg/status`, `/api/system/status` | all served |
| **367 KB blob upload over the tunnel** | 10.6 s (~35 KB/s), CRC `0xBDE9DF7D` matching the build byte-for-byte |
| Board health at 1.6 h uptime | no crash log, 0 stale tasks, 0 stack warnings, IWDG gap max 617 ms of 16400, heap free 16.8 KB, load 21 % |

The bulk upload matters more than the pings: it is the first thing to exercise
full-MTU TCP through the 1420-byte tunnel, which is where a checksum or
fragmentation fault would still show. **Board #2 is now fully manageable
remotely, including OTA.**

An earlier 30-ping run measured on board #2's own LAN showed 6.7 % loss; the
40-ping run from the remote site showed none, and a LAN-local ping in the same
window showed 0 % with a 127 ms outlier. That loss was the sodas wifi, not the
tunnel — worth recording so it is not later mistaken for a tunnel defect.

### 3.13b Board #2 stability baseline (2026-08-18)

Board #2 is the board nobody can reach physically, so item 10's HardFault at
~24 h uptime is its main residual risk. A heap leak is the classic cause, and
there is none: sampled five times over 100 s at ~1.7 h uptime, `free` held at
**17064 B exactly** and `free_min` at **13784 B**, both unmoved.

One reading worth not misinterpreting: a single earlier sample showed `free`
at 16864. That is not drift — the `/api/system/status` handler builds its JSON
in a transient `pvPortMalloc` block, so a sample taken while another request is
in flight sees its own overhead. Compare `free_min`, which is monotonic.

Idle load is **7 ‰**; the 213–225 ‰ figures elsewhere in this session were
measured under active tunnel traffic, not at rest.

A soak monitor samples uptime, heap, stale tasks and the IWDG gap every 5 min
and dumps the crash log if uptime ever goes backwards. It only detects the
fault; item 10 is still unexplained and still open on both boards.

### 3.14 To-do 6b implemented: ephemeral WireGuard source port (`Pd1.1.12`)

`wg_link.c` set `s_initData.listen_port = WIREGUARDIF_DEFAULT_PORT` (51820),
which is what let §3.9 happen: a board sharing a site with the hub hairpins
through the router and the endpoint the hub learns becomes indistinguishable
from the hub's own `public-IP:51820`. Now `listen_port = 0` — `udp_bind()` with
port 0 picks a free port, exactly like every other roaming client, and the
portable public endpoint keeps working.

Note the bind happens in `wireguardif_init()`, which runs only on the first
`netif_add()`; the reconfigure path (§4.1) does not re-bind, so the port is
chosen once per boot.

**Verified only in the sense that board #2's tunnel works with it.** Board #2
is remote from the hub, so it never had the collision this fixes — the change
is preventive there. **The fix is unproven against the fault it targets**,
which needs board #1: it is co-sited with the hub and is still on the §3.9
*workaround* (endpoint pointed at the hub's LAN address `192.168.0.161`
instead of its public IP). The test is to put board #1 on `Pd1.1.12`, set the
endpoint back to `85.206.57.75:51820`, restart, and see whether the hub learns
`192.168.0.116:<ephemeral>` rather than its own endpoint. Until then to-do 6b
is implemented but not validated.

### 3.15 Hub-side counters still ungathered

WGDashboard on `192.168.0.161:10086` answers `200` from the hub's LAN, but the
`wg-dashboard-apikey` value is recorded nowhere in this repo, so the per-peer
transfer counters of §3.8 could not be re-read this session. Not blocking —
the board-side counters plus end-to-end traffic settle everything current —
but the key is worth storing somewhere durable for the next investigation.

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

1. ~~Flash `Pd1.1.6`~~ — **done 2026-08-14**, both boards, confirmed + golden.
2. ~~Run the §3.3 test~~ — **done, see §3.5.** The board is exonerated; the
   loss is board → hub → peer, hub-side.

2b. ~~**Board #2 carries the §3.11 checksum bug**~~ — **DONE 2026-08-18**,
    flashed from its own LAN to `Pd1.1.12`, confirmed + golden, verified
    remotely including a 367 KB OTA upload over the tunnel (§3.13). Nothing in
    this project needs physical presence at the sodas site any more.
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
6b. ~~**Stop binding the board's WireGuard socket to 51820**~~ —
    **IMPLEMENTED 2026-08-18** in `Pd1.1.12` (`listen_port = 0`, §3.14).
    **Still needs validation against the fault it targets:** that requires
    board #1 (co-sited with the hub, still on the §3.9 endpoint workaround) to
    be put on `1.1.12` with its endpoint restored to the hub's *public* IP.
    Do it from the hub's LAN, where recovery over `192.168.0.116` is certain.
    **Deliberately not done 2026-08-18** — board #1 was working and was left
    untouched by decision; board #2 was the priority. Nothing is blocked by
    this, since board #2 is remote from the hub and cannot hit §3.9.
7. **Second `AllowedIPs` range is implemented but untested.** Adding
   `192.168.0.0/24` should let a board reach site-LAN hosts directly; nothing
   has exercised it.
8. ~~**Board #1 migration.**~~ **Done 2026-08-17** — `PeriphNet_namai.conf`
   uploaded, `session_up`. The warning stands for any future OTA: a v1 record
   carries no keys, so the tunnel stays down until a `.conf` is re-uploaded.
   Reachability via the hub's LAN route is what makes that recoverable.
9. **Rotate board #1's key — STILL OPEN.** The `0DT2Rqlr…` key is in git
   history and in every `.bin` ever built from it, and the 2026-08-17
   dashboard `.conf` **reused it** (§3.7). Fix: `POST /api/wg/keygen`, then
   paste the new public key into WGDashboard. Tunnel is down in between.
9b. **Board #1 is one release behind** — `Pd1.1.11`, i.e. it lacks the §3.14
    ephemeral-port fix and still depends on the §3.9 workaround. See 6b.

10. **Board #1 HardFault — RECURRED 2026-08-19, still unexplained.**
    Second occurrence, on `Pd1.1.14`, at `tick 4081472` ≈ **68 min** (the first
    was ≈ 24 h). Same fault site and signature: `pc 0804DBDE` →
    `vTaskDelay+0x3E`, `lr 0804DE9E` → `xTaskResumeAll+0xFA`, `cfsr 0x00008200`
    (PRECISERR+BFARVALID), `bfar 0x001D803C` (not valid memory),
    `r12 0xA5A5A5A5` (FreeRTOS stack fill). The IWDG reset it ~16 s later
    (`reset_cause 0x24000003`, bit 29) and it ran 6.9 h afterwards without
    incident. Raw log kept in
    `docs/crashlogs/board1_2026-08-19_hardfault_Pd1.1.14.json`.

    **New: the faulting context was a TASK, not an ISR** — the stacked
    `psr 0x61000000` has IPSR = 0, i.e. thread mode. So `pcTaskGetName(NULL)`
    would have named the offender, and did not, because of the bug below.

    **Why two occurrences taught us nothing about which task:** the crash log's
    per-task snapshot was silently empty. `CRASH_LOG_MAX_TASKS` was **8** while
    the system has **11** tasks, and `uxTaskGetSystemState()` returns ZERO when
    its array is too small — so `task_count` was 0 and every task's pc / lr /
    state / stack went unrecorded. It broke by GROWTH, not by an edit, and
    nothing reported it. `printTaskList()` (Trice-only) uses `tasks[16]` and
    kept working the whole time, which is why the live output looked fine and
    only the durable record was blind. Fixed in `Pd1.1.15`: capacity 16, and
    `task_name` is now recorded for every crash type rather than only for
    watchdog/assert/stack-overflow. The record grew to ~572 B, still one
    quarter of its 4 KB sector; the layout change invalidates previously
    stored logs by CRC, which is why the one above was saved off first.

    Two samples is not a trend, but the interval fell from ~24 h to ~68 min
    across a firmware that grew substantially in tasks and CCM pressure — worth
    holding as a lead, not a conclusion. Leading suspect remains list/TCB
    corruption, with the thin stacks (`tudp` 53–60 free words of 512,
    `EthLink` 79, `IDLE` 104) the obvious candidate source.

10b. **Original entry — board #1 HardFault on `Pd1.1.6`.** Crash log read
    2026-08-17: `pc 0804AF7E` → `vTaskDelay`, `lr 0804B23B` →
    `xTaskResumeAll`, `cfsr 0x00008200` (PRECISERR + BFARVALID),
    `bfar 0x0083002D` (not valid memory), `r12 0xA5A5A5A5` (FreeRTOS stack
    fill pattern), at `tick 86397689` ≈ 24 h uptime. A bad pointer
    dereferenced while walking the task lists — FreeRTOS list or CCM heap
    corruption. `sp 0x10006700` is in CCM, where the 48 KB heap lives. Not a
    WG or Trice fault; it is in the firmware currently deployed to both
    boards.

**Hygiene**

11. Commit the working tree, including the §4.5 compile fixes.
11b. **Store the WGDashboard API key** somewhere durable (§3.15) — the
     hub-side per-peer counters are the measurement that closed §3.8 and it
     could not be repeated this session.

12. `~/Downloads/*.conf` are mode `664` — world-readable files holding private
    keys. Move to `/etc/wireguard/` at `600`. This now includes
    `PeriphNet_namai.conf`, which holds the compromised `0DT2Rqlr…` key.
    `PeriphNet_1.conf` is stale (it claims `10.77.0.5`, board #2's address);
    delete it.
13. WGDashboard's generated `Address` field has now disagreed with the hub's
    peer AllowedIPs twice. Treat the dashboard's peer entry as authoritative
    and the generated `.conf` as a starting point only. Note also that
    re-downloading a `.conf` after **renaming** a peer returns the original
    key — a rename is not a re-issue (§3.7).

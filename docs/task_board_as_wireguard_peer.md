# Task — PeriphNet as a WireGuard peer (remote access from outside the LAN)

> Created 2026-08-07. Owner decision: **board-as-peer is the priority**;
> the site-gateway box is a fallback used **only if the WG stack does not fit**
> the device (CCM budget). Design context: `design_remote_access_and_autonomy.md`
> §3/§3a/§3b. Not started — this is the plan, not a log.

## Goal

Make the PeriphNet board reachable and operable **from outside its local
network**, so it works identically whether it sits on the HA LAN or on a
foreign LAN it does not own. Concrete acceptance for the first milestone:

- From the laptop **not on the board's LAN** (over the laptop's own WG peer to
  the hub), `curl http://<board-tunnel-ip>/api/fwu/status` returns, and an OTA
  `.pnfw` upload + install + confirm completes over the tunnel.
- The board's MQTT bridge reaches the site-A broker over the tunnel and HA
  sees its entities.
- **Autonomy preserved:** with the tunnel down (hub unreachable), the RS485
  walker / BMS→CAN loop keeps running unaffected; the board reconnects with
  backoff when the hub returns. No WG code path may block a FreeRTOS task or
  stall the RS485 bus.

## Hard constraints (from CLAUDE.md + design doc)

- **CCM is the binding limit** — ~6 KB free (`.ccmram` 11.4 KB + `.ccmheap`
  48 KB of 64 KB). No DMA/peripheral buffers in CCM. WG scratch that cannot
  fit CCM must fit main SRAM without overflowing lwIP pools.
- **Flash has room** — `.text` ~221 KB of 480 KB.
- New crypto (Curve25519, ChaCha20-Poly1305, BLAKE2s) is **not** in
  `Shared/Crypto` — comes bundled with the WG port; do not hand-roll, and do
  not link it into the 32 KB bootloader.
- lwIP is **2.1.2**; target `smartalock/WireGuard-lwIP` (or equivalent) which
  is built against this API. WG is an lwIP netif — no app-layer changes.
- Control path stays **broker-mediated / outbound-only**; inbound to the board
  is debug-only. Do not add an HA→board request/response path that needs
  inbound reachability.

## Step 0 — Verify the hub before touching firmware ✅ DONE (2026-08-07)

**No SSH to the hub VM was needed.** The WGDashboard web UI covers every
VM-side fact, and the rest is answered empirically by an actual handshake —
WireGuard is silent to unauthenticated probes, so a port scan proves nothing
and only a real handshake does. SSH is reserved for *why-is-it-broken*
diagnostics if a test fails.

**Confirmed values:**

| Fact | Value | How verified |
|------|-------|--------------|
| Hub public endpoint | `85.206.57.75:51820` | public IP read from the site LAN; `manochata.ddns.net` still does **not** resolve — use the raw IP |
| UDP 51820 forwarded | yes | router (`192.168.0.1`, Archer C6 v2) |
| wg0 up + listening | yes, port `51820` | live handshake in <4 s from the site LAN |
| Hub tunnel IP | `10.77.0.1` (was `10.0.0.1`) | dashboard interface panel |
| Hub public key | `wL7FWpGpruu5iKEErh/UipbW3ooQlz+ZGsw9Bcvw82k=` | dashboard + handshake (wg0 was recreated; the old `n3sKhJ5k…` key is dead) |
| `ip_forward` + NAT masquerade | working | board reached through the hub over a narrow `/32` route |
| Uplink interface | `eth0` | confirmed by the masquerade rule working |

**Tunnel subnet renumbered `10.0.0.0/24` → `10.77.0.0/24`.** The old range
satisfied the letter of "must not collide with a site LAN" but not its intent:
`10.0.0.0/24` is a common ISP-CPE default, so a board on a foreign LAN using
`10.0.0.x` would be unreachable via tunnel in exactly the deployment this
feature exists for. Renumbered while it cost only four re-issued peer configs.

**Address plan** (peers issued 2026-08-07). Allocation splits the `/24`:
**hub `.1`, humans `.2`–`.63`, devices `.64`+** — so a device's tunnel IP is
recognisable at a glance and device addresses can be assigned without
coordinating against the human range.

| Peer | Tunnel IP | Client-side `AllowedIPs` |
|------|-----------|--------------------------|
| hub | `10.77.0.1` | — |
| `Lauris_phone` | `10.77.0.2` | `10.77.0.0/24, 192.168.0.116/32, 192.168.0.161/32` |
| `Lauris_workLaptop` | `10.77.0.3` | same |
| `Lauris_laptop` | `10.77.0.4` | same |
| **`PeriphNet_1`** | **`10.77.0.64`** | `10.77.0.0/24` only — deliberately no site-LAN `/32`s |
| PeriphNet #2… | `10.77.0.65`+ | same |

Hub public key for every peer:
`wL7FWpGpruu5iKEErh/UipbW3ooQlz+ZGsw9Bcvw82k=`, endpoint `85.206.57.75:51820`,
MTU 1420, keepalive 21.

**The board's config carries no `192.168.0.x` deliberately.** The broker address
is runtime config (`s_cfg` in `App/Mqtt/mqtt_bridge.c`; the `10.42.0.1` in
`App/Cmd/cmd_parser.c` is a bench default), so we can point the board at a
broker reachable *over the tunnel* instead of at a site-LAN address. Baking a
`192.168.0.x/32` into the board would break it the moment it deploys to a
foreign LAN using that same very common range — the exact failure board-as-peer
exists to prevent. **Step 2 decision: give the broker/HA host a tunnel IP (or
run the broker on the hub at `10.77.0.1`) so the board's network config stays
entirely site-agnostic.**

Board peer private key is dashboard-generated (`0DT2Rqlr…`) — acceptable for
bring-up only; see Deferred for on-device key generation.

**Renumber procedure — in-place edit does NOT work.** Editing the wg0
`Address` from `10.0.0.1/24` to `10.77.0.1/24` in the dashboard fails; the
configuration had to be **deleted and recreated**, which mints a new hub
keypair and drops every peer. Budget for re-issuing all client configs. Fields
for the recreated config: name `wg0` (must match the `wg0` in the iptables
rules), port `51820` (the router forward is wired to it), Address
`10.77.0.1/24`, MTU 1420, `PreUp`/`PreDown` empty, `Table` empty/auto. The
masquerade source in `PostUp`/`PostDown` must be renumbered too — easy to miss.
`net.ipv4.ip_forward=1` is set from `PostUp` so it survives a VM reboot without
needing SSH.

**GOTCHA — WGDashboard mangles `PostUp` into a comma-separated list.** The
stored value was `...MASQUERADE;, iptables ... , iptables ...`. `wg-quick` runs
`PostUp` as a single shell command, so it dies on `,`
(`/usr/bin/wg-quick: line 295: ,: command not found`) and then **deletes the
interface** — the tunnel goes fully down, not degraded. Whenever wg0 refuses to
come up after a dashboard edit, check `PostUp` for commas first. Correct form is
one line, `;`-separated, no trailing `;`:

```
iptables -A FORWARD -i wg0 -j ACCEPT; iptables -A FORWARD -o wg0 -j ACCEPT; iptables -t nat -A POSTROUTING -s 10.77.0.0/24 -o eth0 -j MASQUERADE
```

`PostDown` mirrors it with `-D`. The previously present unscoped
`-t nat -A POSTROUTING -o eth0 -j MASQUERADE` was dropped as redundant and
overly broad. **Unverified:** that the VM's uplink is really `eth0` — it is a
Proxmox guest (`bc:24:11:…`), where `ens18` is common. Suspect this first if the
tunnel comes up but LAN hosts are unreachable.

**`ip_forward` + NAT are considered confirmed** without SSH: the `PostUp` rules
above exist, and the oldest peer shows ~1.28 GB transferred over 48 days.

**End-to-end verified 2026-08-07** from the site LAN, all three checks green:
handshake against the **public** endpoint `85.206.57.75:51820`, hub tunnel IP
`10.77.0.1` replying, and `GET /api/fwu/status` on the board `192.168.0.116`
answering **through** the hub (`Pl1.0.8`, confirmed, uptime 45564 s). Because
the handshake used the public IP, it left to the router and returned via
hairpin NAT — so the **port-forward DNAT rule is proven**, not assumed. Residual
untested sliver: that the WAN-side firewall accepts the same packet arriving
from the internet. Check 3 passing also confirms `ip_forward` + masquerade and
that the uplink really is `eth0`.

**WGDashboard field semantics — the two AllowedIPs fields are OPPOSITE
directions and must not be set equal:**

| Dashboard field | Written into | Value |
|---|---|---|
| `Allowed IPs` | the **hub's** `[Peer]` block | that peer's own tunnel `/32`, unique, never overlapping |
| `Endpoint Allowed IPs` | the **generated client** config's `[Peer]` block | what the client routes into the tunnel |

Client-side value in use: `10.77.0.0/24, 192.168.0.116/32, 192.168.0.161/32`
(tunnel + board + the dashboard itself, so WG can be administered remotely
without SSH). **Never `0.0.0.0/0`, never `192.168.0.0/24`, and never
`192.168.0.1/32`** — the site router and the home router share that address, so
a `/32` for it would beat the home default route and send the roaming laptop's
own gateway traffic down the tunnel. Per-host `/32`s only. Note the dashboard's
peer-default did **not** apply on config creation: generated configs still came
out with `AllowedIPs = 0.0.0.0/0` and must be narrowed by hand.

**Note on peer key custody:** WGDashboard generates and stores peer private
keys, so a dashboard-issued key is not secret from the dashboard. Fine for
laptop/phone peers and for board bring-up; the production board key must be
generated on-device with only the pubkey uploaded (see Deferred).

## Step 1 — Footprint experiment ✅ PASSED (2026-08-07)

**Verdict: board-as-peer is GO.** The site-gateway fallback is not needed.
Measured with `smartalock/WireGuard-lwIP` (submodule at
`Middlewares/Third_Party/wireguard-lwip`) fully linked and *referenced* —
`--gc-sections` is on, so the netif is really created from `MX_LWIP_Init()`
and none of this is dead code the linker could drop.

| Region | Baseline | With WG | Delta | Free after |
|---|---|---|---|---|
| `.text` | 226,440 | 259,916 | **+33,476** | |
| `.rodata` | 41,748 | 43,036 | +1,288 | |
| **Flash total** | 269,156 | 303,928 | **+34,772** | **187,592 B of 480 KB (38 %)** |
| `.data`+`.bss`+stack | 69,016 | 69,208 | **+192** | **61,864 B of 128 KB** |
| `.ccmram` | 11,428 | 11,428 | **0** | 6,108 B free (unchanged) |
| `.ccmheap` | 48,000 | 48,000 | **0** | |

**CCM — the thing the whole gate was designed around — was never touched.**
The premise that CCM would be the binding limit was wrong: the port
heap-allocates its state and keeps crypto scratch on the stack, so nothing
lands in a static CCM section. Flash absorbed the cost easily.

**Runtime allocations (not visible in the section table):**

| Item | Size | From |
|---|---|---|
| `struct wireguard_device` (incl. 1 peer) | **968 B** | lwIP heap `MEM_SIZE` 20,480 via `mem_calloc` |
| UDP PCB | 1 | `MEMP_NUM_UDP_PCB` (8 available) |

Struct sizes: `wireguard_device` 968 B, `wireguard_peer` 784 B,
`wireguard_handshake` 140 B. `WIREGUARD_MAX_PEERS` is **1**, set in the
port's `wireguard-platform.h`; each extra peer adds 784 B.

**Stack — the real risk, and the one change this forced.** WireGuard's
handshake and packet crypto run on lwIP timers/input, i.e. in
**tcpip_thread**, whose CubeMX default stack is 4096 B. Measured per-frame
usage (`-fstack-usage`, `-O2`):

| Function | Frame |
|---|---|
| `poly1305_blocks` | 960 B |
| `wireguard_hmac` | 424 B |
| `chacha20poly1305_decrypt` | 360 B |
| `wireguard_process_handshake_response` | 352 B |
| `blake2s_compress` | 312 B |
| `x25519` | 216 B |

Deepest realistic chain (`wireguardif_process_data_message` →
`chacha20poly1305_decrypt` → `poly1305_blocks`) adds **~1.5 KB** on top of
lwIP's own usage. That is too little margin at 4096, so
`TCPIP_THREAD_STACKSIZE` was raised to **6144** in `LWIP/Target/lwipopts.h`.
**That +2 KB comes out of the 48 KB FreeRTOS heap in CCM** (the stack is
`pvPortMalloc`'d), not main SRAM — task stacks total ~25.6 KB of that 48 KB,
so there is room, but confirm against the `Heap=` value TRice'd at boot.

**Files added:** `App/Net/wg_link.c/h` (netif + peer bring-up, Ethernet stays
the default route), `App/Net/wg_platform.c` (the port's four required hooks).
CMake: `WIREGUARD_SOURCES`, application-only, never the bootloader.

**Two security items this step deliberately left unfinished** — both marked
loudly in the source, both must close before field use:
1. `wireguard_random_bytes()` is a SHA-256 DRBG seeded from DWT/tick/stack
   jitter. **Not a CSPRNG.** WireGuard draws ephemeral session keys from it,
   so a predictable stream breaks the tunnel outright. Enable the STM32F407
   hardware RNG (`HAL_RNG_MODULE_ENABLED` is commented out in
   `Core/Inc/stm32f4xx_hal_conf.h` — toggle in CubeMX, not by hand) and seed
   from it.
2. `wireguard_tai64n_now()` is derived from `sys_now()`, which restarts at 0
   every boot. The hub keeps the greatest timestamp seen per peer, so after a
   board reset our handshakes go backwards in time and the hub rejects them
   until its own state clears. Needs a reset-surviving monotonic source —
   SNTP once the tunnel is up, or a counter in EEPROM/ext-flash.

### Original plan (kept for reference)

Before integrating, prove the stack fits. Build the WG port + its crypto into
the application in isolation (netif created, one peer configured, handshake
exercised against the hub or a bench WG server), then:

```
arm-none-eabi-size -A build/application.elf   # NOT plain size (see CLAUDE.md)
```

- Compare `.text` (expect flash headroom), and critically **`.ccmram` +
  `.ccmheap` vs 64 KB** and main `.data`/`.bss`/`._user_heap_stack` vs 128 KB.
- Locate WG handshake/peer state and Curve25519/ChaCha scratch; decide their
  section placement. Peer/handshake state is CPU-only → CCM-eligible, but CCM
  has ~6 KB — measure. Transient scratch may go on task stacks (size the WG
  task accordingly) or main SRAM.
- **Decision:** fits → continue to step 2. Does not fit → stop, switch to the
  site-gateway fallback (design doc §3), and record the measured overflow here.

## Step 2 — Integrate the netif (device side) ✅ CODE COMPLETE (2026-08-08)

Built and linked; **not yet exercised against the hub on hardware** — that is
step 4.

- Submodule `Middlewares/Third_Party/wireguard-lwip` (smartalock), wired in as
  the application-only `WIREGUARD_SOURCES` set, never the bootloader.
- `App/Net/wg_link.c/h` creates the netif and the single hub peer. The
  Ethernet netif deliberately stays `netif_set_default()`, so only the tunnel
  subnet (covered by the WG netif's address/netmask) routes through WG and the
  encapsulated UDP still leaves via Ethernet.
- **Bring-up moved out of `MX_LWIP_Init()` into `App_DefaultTaskEntry()`**,
  after `W25Q128_Init()` and the DHCP wait. The reason is step 3's timestamp
  store: the first handshake must not fire before the flash-backed monotonic
  clock exists. `LWIP/App/lwip.c` now only carries a note saying so.
- Config lives in RAM (`s_cfg` + key buffers inside `wg_link.c`), seeded from
  the built-in default, so a caller may pass a stack temporary. CLI surface:
  `wg start|stop|status|endpoint <a.b.c.d> [port]` — `endpoint` retargets a
  live peer via `wireguardif_update_endpoint()`, which is what a bench WG
  server test needs.
- Keepalive is **21 s**, matching the value the hub issues (the plan said 25;
  the dashboard-generated configs in step 0 use 21 — kept consistent with the
  hub rather than with the plan text).
- **Device private key is still a build constant** (`s_defaultCfg` in
  `wg_link.c`, board #1, dashboard-issued). Bring-up only, loudly commented.
  Per-device on-device generation stays in Deferred.

**One integration fix worth remembering:** `wireguardif.c` carries two
leftover `printf()` calls (upstream's own `#include <stdio.h> // TODO:
Remove`). Newlib's `printf` mallocs a ~1 KB stdio buffer out of the **1.5 KB**
`._user_heap_stack` through a non-thread-safe allocator, and both calls run
from a task holding the lwIP core lock. Rather than patch the submodule, that
one file is compiled with `-Dprintf=WgPlatform_NullPrintf` (object-like on
purpose — a function-like `-Dprintf(...)` would also mangle the declaration in
`<stdio.h>`), and the sink lives in `wg_platform.c`.

## Step 3 — Autonomy / failure behaviour ✅ CODE COMPLETE (2026-08-08)

Design verified by reading the port; **bench verification with the hub pulled
is still pending** (step 4).

- **Nothing blocks.** `WgLink_Start()` returns immediately whether or not the
  hub answers; handshake and rekey run on lwIP timers in tcpip_thread. The
  only app-side periodic work is a 5 s housekeeping block in defaultTask
  (flash persist + up/down transition logging), which cannot stall RS485/CAN
  and sits inside the existing 100 ms IWDG-kick loop.
- **Retry is unbounded and free.** `should_send_initiation()` fires whenever
  `!curr_keypair.valid && peer->active`, rate-limited to one initiation per
  `REKEY_TIMEOUT` (5 s). `peer->active` is set by `wireguardif_connect()` and
  never cleared by failure, so no app-side backoff loop is needed — a hub that
  is down for a week costs one packet per 5 s and reconnects by itself.
- **Trice stays out of lwIP context.** `wg_platform.c` has no Trice calls at
  all; RNG failures are *counted* and surfaced through
  `WgPlatform_GetRngStatus()` for `wg status` / defaultTask instead.

### The two security items from step 1 — both now closed

**1. Entropy — hardware RNG.** `HAL_RNG_MODULE_ENABLED` is on and
`MX_RNG_Init()` runs from `main()`. `wireguard_random_bytes()` now seeds the
SHA-256 DRBG from 8 hardware words (plus DWT/tick/stack jitter) and mixes a
**fresh** hardware word into every 32-byte output block, so the DRBG is
whitening real noise rather than substituting for it. A seed/clock error
latches the F4 RNG, so `hw_rng_word()` clears CEIS/SEIS, re-inits the
peripheral and retries once; a hard failure increments a counter, leaves
`hw_seeded=0`, and falls back to the DRBG rather than returning zeros.
`wg status` reports both.

**2. TAI64N — reboot-surviving monotonic clock.** New `App/Net/wg_time.c`:

- one 4 KB ext-flash sector (`EXT_FLASH_WG_TIME_ADDR` = `0x0010_2000`) used as
  an **append-only ring of 16 B slots** (magic + seconds + seq + CRC32), so a
  write costs a page program, not a sector erase — 256 slots, erased and
  restarted when full or when a torn slot is found;
- every boot reads the newest valid slot, adds `WG_TIME_BOOT_BUMP_S` (1 h) and
  writes the new base back;
- while running, `WgTime_Tick()` re-persists every `WG_TIME_PERSIST_S`
  (15 min) — called from the defaultTask housekeeping block, never from lwIP
  context, because it touches SPI;
- the bump being **4× the persist interval** is the monotonicity argument: the
  worst case (power cut right before a scheduled write) still puts the next
  boot's base above every stamp the previous boot could have emitted;
- floored at `WG_TIME_BUILD_EPOCH`, which CMake defines from
  `string(TIMESTAMP … "%s")` at configure time — a virgin board therefore
  emits a plausible timestamp instead of one near 1970, and reflashing always
  moves the sequence forward even if the flash store was wiped. The CubeIDE
  managed build does not define it and falls back to 0.
- `WgTime_SetIfNewer()` is the hook for SNTP later: a real UNIX time is far
  larger than this free-running counter, so adopting it keeps the sequence
  monotonic across the switch.

At ~96 writes/day the ring erases roughly every 2.6 days — ~140 sector erases
a year against the W25Q64's 100 k endurance.

**If the tunnel refuses to handshake on first contact**, suspect hub-side
peer state left over from the step-1 experiment (which used the old
`sys_now()` clock and may have recorded a stamp our counter has not reached).
Removing and re-adding the peer in WGDashboard clears it.

### Footprint after steps 2–3

`arm-none-eabi-size -A build/application.elf`, measured 2026-08-08:

| Region | Step 1 | Now | Delta |
|---|---|---|---|
| `.text` | 259,916 | 264,604 | +4,688 |
| `.rodata` | 43,036 | 43,396 | +360 |
| **Flash total** | 303,928 | 308,000 | **182,880 B of 480 KB free (38 %)** |
| `.bss` | — | 67,384 | main SRAM ~61 KB free |
| `.ccmram` | 11,428 | 11,428 | **0 — still untouched** |
| `.ccmheap` | 48,000 | 48,000 | 0 |

Host unit tests (`ctest --test-dir tests/build`) still 7/7 green.

## Step 4 — End-to-end over the tunnel

- Laptop becomes a WG peer of the hub (retire the dead OpenVPN profiles; go
  WG-only). From a network that is **not** the board's LAN:
  - `curl http://<board-tunnel-ip>/api/fwu/status`, then a full OTA cycle
    (upload → install → confirm) over the tunnel.
  - Confirm MQTT bridge → site-A broker and HA entity visibility with the board
    remote.
- Re-run `arm-none-eabi-size -A` on the final image; record CCM/flash margins.

## Deferred / follow-up (out of first milestone)

- Per-device WG key provisioning + storage (not in the firmware image).
- Second board / multi-peer at the hub; stable tunnel-IP allocation scheme.
- The `192.168.0.0/24` renumber decision (design doc §8, currently "decide
  later") — only bites the roaming laptop, not the board, so not blocking here.
- Modbus-TCP gateway as a queued walker client (design doc §4) — separate work.

## Open questions to resolve in step 0/1

1. Hub public endpoint + is UDP 51820 actually forwarded? (`manochata.ddns.net`
   resolution.)
2. Does the WG stack + crypto fit CCM/SRAM? (step 1 — the gate.)
3. Tunnel subnet choice that avoids every site LAN.
4. Where does the device private key live in production?

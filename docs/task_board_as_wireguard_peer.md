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

## Step 0 — Verify the hub before touching firmware

Needs one SSH login to the hub VM (`192.168.0.161`). Read-only checks:

- `sudo wg show` — is an interface up, what peers/AllowedIPs exist, listen port.
- `/etc/wireguard/wg0.conf` — hub pubkey, address (tunnel subnet), listen port.
- `sysctl net.ipv4.ip_forward` — must be 1 for hub↔LAN routing.
- `iptables -t nat -S` / `nft list ruleset` — masquerade for tunnel→LAN so the
  board's packets reach the broker/HA and return.
- Router: is **UDP 51820 port-forwarded** to `192.168.0.161`, and what is the
  **public endpoint** (DDNS name that resolves, or static IP)? `manochata.ddns.net`
  did not resolve on 2026-08-05 — confirm or replace.

Output of step 0: the hub's public `Endpoint`, its tunnel subnet, and a free
tunnel IP to assign the board. **Pick a tunnel subnet that collides with no
site LAN** (not `192.168.0.0/24`).

## Step 1 — Footprint experiment (the go/no-go gate)

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

## Step 2 — Integrate the netif (device side)

- Add the WG port under `Middlewares/Third_Party/` (git submodule, matching the
  trice/backtrace pattern); wire into CMake as an application-only source set
  (never bootloader).
- Bring up a `wireguard_netif` in the lwIP init path alongside the existing
  Ethernet netif. Keep the Ethernet netif the default route for on-LAN traffic;
  route only the tunnel subnet (and the broker/HA host if off-tunnel) via WG.
- Own key material like the FWU keys are owned: **device private key must not
  be a build-time constant checked into the app in plaintext long-term.** For
  the first bring-up a provisioned key is fine; note the productionization
  (per-device key, stored where? EEPROM/ext-flash, not in the image) as
  follow-up — do **not** reuse the FWU key mechanism.
- `PersistentKeepalive = 25`. Config surface: reuse the existing config/upload
  or CLI pattern rather than hard-coding endpoint/keys in a header.

## Step 3 — Autonomy / failure behaviour

- WG handshake + rekey must run in a task context that **cannot stall RS485 or
  CAN**. Verify: pull the hub, confirm the walker/BMS/CAN cadence is unaffected
  and IWDG is still kicked.
- Reconnect/backoff when the hub is unreachable (mirror the mqtt task's
  existing backoff). No busy-wait, no blocking the tcpip_thread.
- Trice cannot be used in lwIP callback context — keep WG logging in task/ISR
  contexts only (same rule as the MQTT client).

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

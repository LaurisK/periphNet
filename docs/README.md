# PeriphNet Documentation Index

**Only current, correct documents live here.** Completed plans, fixed-issue
post-mortems and session status notes are **deleted, not archived** — what still
binds from them is restated where it belongs. This index was rebuilt 2026-09-05,
when 9 completed documents and the `archive/` tree were removed.

Status legend: **current** = describes what is shipped and running ·
**design** = the contract an implementation must meet · **direction** =
observations/proposal, nothing frozen · **plan** = approved work not yet done ·
**open issue** = a known defect, not fixed · **reference** = durable facts and
techniques.

`CLAUDE.md` in the repo root is the entry point: build/flash/test commands,
memory map, HTTP API, task table, coding standards.

---

## Where the project is going

| Doc | Status | Covers |
|-----|--------|--------|
| [design_solis_modbus_link.md](design_solis_modbus_link.md) | **direction — active work** | **The one missing link in `Solis → PeriphNet → solis_modbus → sunSale`.** Why sunSale cannot bypass `solis_modbus` and PeriphNet cannot replace it; the far end's contract (five calls, one transaction at a time, exception-2 as an adaptation signal, FC16 atomicity); the four mismatches against what the board has; and the two candidate shapes. §7 is what is undecided |
| [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md) | direction | Why autonomy is a hard requirement (the 1 Hz CAN obligation), the two-site topology, and the register-map generator idea. Its WireGuard and MQTT-vs-Modbus sections are superseded — the header says by what |
| [architecture_backlog.md](architecture_backlog.md) | direction | Everything not being worked. Undeclared context contracts, vocabulary duplication, and §6 **the pinned storage layout** — the largest piece of undesigned architecture in the project |

## What is running

| Doc | Status | Covers |
|-----|--------|--------|
| [modbus.md](modbus.md) | **current + design** | **Everything Modbus, in one document.** The model (ports, capabilities, plans, devices), the consumer surface, the engine, config JSON, record format, operator reference, test contract. §1–§10 are implemented and in production on both boards |
| [design_battery_pack.md](design_battery_pack.md) | current | **The battery pack module**, built and running. A pack is a *type* with *instances*; packs bind to a physical address, report their own condition, and let the cluster decide. `pack_pylontech` still refuses to bind — its frame parser does not exist |
| [design_can_bridge.md](design_can_bridge.md) | current | **The CAN1/CAN2 bridge**, carrying 1.28 M live frames on zaliakalnis. One file owns both cells and the single weak RX callback; store-and-forward transparency and what it costs; the identifier register and trace ring; the flash trace. `bms` mode is built but never exercised |
| [design_nv_db.md](design_nv_db.md) | current | **`nvDb` — the only authority over the external flash address space.** Users get a flat bounds-checked span from `0x00`; placement, relocation and isolation live in one module. Includes the §2.7 layout JSON that `POST /api/nvdb/layout` is defined against |
| [design_http_server_adapter.md](design_http_server_adapter.md) | current | The three decisions that survive the `http_server.c` restructure: an enum's name belongs to its module (and `-Werror=switch` enforces it), why status rendering does **not** move behind a byte sink, and why the file's size is deliberately a separate question |
| [pylontech_can_protocol.md](pylontech_can_protocol.md) | current | The Pylontech CAN frame layout `App/Can/` speaks |

## Known defects and open work

| Doc | Status | Covers |
|-----|--------|--------|
| [issue_can_bus_roles_not_configurable.md](issue_can_bus_roles_not_configurable.md) | **open issue** | Which CAN cell faces the inverter is a `#define` and must be configuration. Harmless in `bridge` mode, which is why it hid — **`bms` mode would take over the wrong side, silently.** Blocks the CAN work |
| [issue_observations_2026-09-05.md](issue_observations_2026-09-05.md) | **open issue** | Two undiagnosed field observations: sodas reports `healthy: false` with `arm_failures: 2` on the board carrying the Solis, and a bridge with nothing wired to it is indistinguishable from a quiet bus |
| [task_wg_endpoint_by_name.md](task_wg_endpoint_by_name.md) | plan | The hub endpoint is a literal IPv4 and cannot be corrected remotely once it changes — the only way in is the tunnel the address broke. Health is the handshake, never the resolution |
| [task_flash_wait_and_ota_cost.md](task_flash_wait_and_ota_cost.md) | plan | An OTA pins the CPU at 100 % because `W25Q128_WaitReady` **waits**, and DMA cannot shorten a wait. Measured, not calculated. Parked deliberately |
| [design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md) | direction | What the JK BMS actually measures, and what its SOC/SOH really are. The anchoring half has shipped; **per-cell capacity/SOH has not** |

## Reference

| Doc | Covers |
|-----|--------|
| [reference_wireguard_hub.md](reference_wireguard_hub.md) | Hub facts, the tunnel address plan, and the WGDashboard behaviours that cost time (the `PostUp` comma that deletes the interface; the two opposite `AllowedIPs` fields) |
| [reference_dma_and_cubemx_regen.md](reference_dma_and_cubemx_regen.md) | The DMA request map and its three hard constraints, plus what a CubeMX regeneration silently changes — including the WireGuard checksum regression it once reverted |
| [reference_debug_halt_during_hang.md](reference_debug_halt_during_hang.md) | Freezing the IWDG and attaching inside the 16.4 s window, what a stack overflow into a neighbouring FreeRTOS object looks like, and two standing lessons |

Diagrams: `upload_500k_sequence.{dot,png,svg}` — the HTTP upload path, generated
from the `.dot` with Graphviz.

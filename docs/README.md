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
| [design_solis_modbus_link.md](design_solis_modbus_link.md) | **BUILT, not yet on hardware** | **The missing link in `Solis → PeriphNet → solis_modbus → sunSale`, now closed in code.** Why sunSale cannot bypass `solis_modbus` and PeriphNet cannot replace it; the far end's contract (five calls, one transaction at a time, exception-2 as an adaptation signal, FC16 atomicity); the shape chosen (a Modbus TCP server on `:502`) and all six implementation steps. **§7.2 — the gateway is transparent both ways and the tunnel is its only authorization — is the section to read before touching the bind address.** §10 is the hardware acceptance list |
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
| [issue_wg_sodas_offline_2026-09-06.md](issue_wg_sodas_offline_2026-09-06.md) | **open issue** | **sodas has been off the tunnel since 2026-09-06 02:01** — 45 h with the hub hearing nothing, unattended and nocturnal. Rules out the NAT, stale-keypair, rollback and hub theories from the hub's own record; leaves site power, a wedged stack, or §5.1. **§4 is the on-site checklist, §5.1 is a real defect fixable without the board: one failed `WgLink_Restart()` disarms tunnel recovery permanently** |
| [issue_sodas_web_wedge_2026-10-03.md](issue_sodas_web_wedge_2026-10-03.md) | **open issue** | **sodas port 80 resets every new connection while `:502` keeps serving HA** — third occurrence. Mechanism established (an 8-slot netconn pool, 3 permanent, filled by `:502` connections parked behind HA's; the drain freed five 200 B heap quanta and moved `connections` by exactly 6). **Trigger not established:** one reload re-wedged in ~2 min, the next held 47+ min. §6 is the on-site plan, §7 the proposed firmware fixes (none implemented) |
| [task_wg_endpoint_by_name.md](task_wg_endpoint_by_name.md) | plan | The hub endpoint is a literal IPv4 and cannot be corrected remotely once it changes — the only way in is the tunnel the address broke. Health is the handshake, never the resolution |
| [task_flash_wait_and_ota_cost.md](task_flash_wait_and_ota_cost.md) | plan | An OTA pins the CPU at 100 % because `W25Q128_WaitReady` **waits**, and DMA cannot shorten a wait. Measured, not calculated. Parked deliberately |
| [design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md) | direction | What the JK BMS actually measures, and what its SOC/SOH really are. The anchoring half has shipped; **per-cell capacity/SOH has not** |

## Battery cluster (BUILT and host-tested 2026-09-07; not yet run on hardware, and driving nothing until a frame source exists)

| Doc | Status | Covers |
|-----|--------|--------|
| [requirements_battery_cluster.md](requirements_battery_cluster.md) | design | What a cluster of parallel packs must do. The headline requirement is combining per-pack current limits so no single pack is over-driven while the maximum is still allowed |
| [design_battery_cluster.md](design_battery_cluster.md) | **current** | The module: API, the closed-loop limit search and its binding gate, architectural constraints, test surface, HTTP shape. **§12 is the implementation** — what exists, three places the code corrects this text, and what it cost (the flash headroom answer among them) |
| [issue_soc_estimator_sodas_2026-10-06.md](issue_soc_estimator_sodas_2026-10-06.md) | **open issue** | **The pack SOC estimator read 81 % at 10 % confidence when the pack was ~30 %, and 94.7 % when it was ~85 %; the inverter is told that number** (`source: pack`). Not drift: **the coulomb integrator has never moved** (`PackSoc_UnitAdvance` truncates `dQ x 1000 / capacity` to 0 for any step under 660 mAh on a 660 Ah pack, and a 5 s poll is 150 mAh), so the estimate is an OCV lookup re-seeded every ~6 min, and the anchor overwrites with a confidence that omits polarisation/hysteresis. Also a 67.8 Ah JK re-calibration step was consumed as charge and drove the estimate to 100 %. §6 is the planned fixes (none implemented, **now superseded by [design_soc_knee_anchored.md](design_soc_knee_anchored.md)**), §7 the tests that must come first |
| [design_soc_knee_anchored.md](design_soc_knee_anchored.md) | **proposal** | **What to do about the SOC estimator, 2026-10-07: re-implement, not patch.** Confirms the failure on *discharge* from the zaliakalnis log (46 min at -43 A: counter and board integral say -12.8 pp, estimator said -47 pp; it also rose 16 pp while discharging). The operator's architecture — OCV defines SOC only at knees, JK-delta coulomb counting carries the plateau, OCV IR-corrected — is the original 2026-08 intent; the firmware built a continuous lookup. Why F2's fusion fails (correlated bias), the JK counter's 99 % fence, the top knee being unreachable at a 3.375 V CV site, 5.6 KB of main SRAM spent on unusable per-cell units, and the unvalidated OCV-table-vs-counter disagreement (~10 pp). §5 is the staged plan (shadow mode and a calibration capture before the wire changes), §6 the operator's open questions |
| [issue_batcomm_alarm_direction_sodas_2026-10-06.md](issue_batcomm_alarm_direction_sodas_2026-10-06.md) | **open issue** | **At the top of a charge a JK discharge-side fault (switch open, one alarm) held `DCL 0` for 40 min, and BatComm's pack path ALSO zeroed charge for it** — `batcomm.c:286` turns `protectionOpen` (JK bits 16 charge-MOS / 17 discharge-MOS, merged) into both directions closed plus `0x359` "system error", while the cluster (`cluster_calc.c:240`) treats the same flag per direction. Reconstructed from the CAN flash log (`configs/celllogs/sodas15_dsg_block_canlog_2026-10-06.tsv`) and the cluster's `forbiddenDsg` 38.6 min vs `forbiddenChg` 1.9 min. **Not established:** the raw JK alarm bit (nothing records it), why the JK opened discharge, and whether one cell tripped OVP — the JK cut at 54.79 V, below the 55.2 V CVL, against a `cell_ovp` of 3.650 V last read 2026-08-18. §6 is what to capture on a repeat, §7 the fixes (none implemented) |
| [review_battery_cluster_design.md](review_battery_cluster_design.md) | current | Pre-implementation embedded review, and the running status of every defect it raised — **including the one implementation found that the review did not**: `load_pm` truncated the wrong way, and the update divides by it |
| [design_can_bms_frame_source.md](design_can_bms_frame_source.md) | design | The CAN dialect the cluster's numbers go out on, and the measured JK transfer function behind it |
| [reference_dyness_can_capture_2026-09-05.md](reference_dyness_can_capture_2026-09-05.md) | reference | The zaliakalnis Dyness CAN capture: 13 identifiers decoded, the 0.01 A vendor deviation, and what is still unknown |

## Reference

| Doc | Covers |
|-----|--------|
| [reference_wireguard_hub.md](reference_wireguard_hub.md) | Hub facts, the tunnel address plan, and the WGDashboard behaviours that cost time (the `PostUp` comma that deletes the interface; the two opposite `AllowedIPs` fields) |
| [reference_dma_and_cubemx_regen.md](reference_dma_and_cubemx_regen.md) | The DMA request map and its three hard constraints, plus what a CubeMX regeneration silently changes — including the WireGuard checksum regression it once reverted |
| [reference_debug_halt_during_hang.md](reference_debug_halt_during_hang.md) | Freezing the IWDG and attaching inside the 16.4 s window, what a stack overflow into a neighbouring FreeRTOS object looks like, and two standing lessons |

Diagrams: `upload_500k_sequence.{dot,png,svg}` — the HTTP upload path, generated
from the `.dot` with Graphviz.

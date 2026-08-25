# PeriphNet Documentation Index

Status legend: **current** = describes shipped behaviour, keep updated ·
**plan** = approved implementation plan, updated when implementation deviates ·
**design** = decided but not implemented; the contract an implementation must meet ·
**direction** = observations/proposal, nothing frozen yet ·
**historical** = superseded, kept for rationale.

`CLAUDE.md` in the repo root is the entry point: build/flash/test commands,
memory map, HTTP API, task table, coding standards.

| Doc | Status | Covers |
|-----|--------|--------|
| [modbus.md](modbus.md) | current + design | **Everything Modbus, in one document.** §2 is the design being built toward — subscription API, frame-level port contract with a test port, devices/types/parameters, event-driven per-device scheduling — and none of it is implemented yet. §3 is what ships today (records, compiler, store, walker). Plus config JSON, operator reference, test contract and §7 limits |
| [pylontech_can_protocol.md](pylontech_can_protocol.md) | current | Pylontech CAN frame layout used by `App/Can/` |
| [issue_idle_iwdg_crashloop.md](issue_idle_iwdg_crashloop.md) | current | Post-mortem: EthIf stack overflow → IWDG crash loop; includes the halt-during-hang debug recipe |
| [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md) | direction | Bridge → edge controller (BMS + CAN), WAN access across NAT, RS485 bus budget, open questions |
| [design_bms_cell_health_estimation.md](design_bms_cell_health_estimation.md) | direction | **What the JK BMS actually measures, and what its SOC/SOH really are.** Firmware evidence that the JK's SOC is a persisted coulomb counter with two voltage anchors (the linear interpolation is only the cold-start seed), that its SOH reads 100 % forever on a solar ESS, and that per-cell capacity/SOH exist nowhere in it. Finds three signals nothing was reading: the per-cell balance-lead resistance array, the identity of the two cells the balancer is working between, and the charge stage / cell chemistry past 0xEF — all now in the shipped JK config. Proposes a per-cell estimator: shared-coulomb regression against OCV anchors at the knees, current-step resistance, weakest-link pack SOC, confidence-gated before it touches the CAN control path |
| [design_battery_pack.md](design_battery_pack.md) | direction + design | **The battery pack module.** Part I storyline: a pack is a *type* (blueprint) with *instances*, the same shape whatever vendor is inside — JK over Modbus first, Pylontech over CAN second. A pack does not know it is in a cluster; it reports its own state and condition and the cluster decides what to consume. Disconnection is three-fold: commanded, pack-detected, consumer-sensed. Part II API (design): `pack.h` with per-group ages and per-instance capabilities confirmed at bind, the pack-type contract with an accept/complete command split, packs bound to a **physical address** rather than any array position, the uploaded config with a narrowing command allowlist, the shared functionality task with a 250 ms tick, two nvDb users and the `NVDB_TARGET_VER` migration they need |
| [task_nv_db.md](task_nv_db.md) | design + plan | **`nvDb` — the application's only authority over the external flash address space.** Users get a flat, bounds-checked span from `0x00`; `nvDb` places, moves and isolates them and knows nothing of their content. API settled (§2), criteria (§3), internals provisional (§4), phased implementation plan (§7); not implemented |
| [task_fault_context_flash.md](task_fault_context_flash.md) | design | **A crash-log write path that depends on no interrupt.** `HAL_GetTick()` cannot advance inside a fault handler or the TIM14 callback, so the recorder's `W25Q128_WaitReady` / `HAL_SPI_Transmit` deadlines never expire and a busy chip hangs it until the IWDG resets — destroying the evidence it exists to keep. Proposes a register-level, DWT-cycle-bounded `Shared/Drivers/w25q_fault.c`; API (§3), budgets (§3.5), criteria (§5), phased plan (§6); not implemented |
| [issue_doc_code_inconsistencies.md](issue_doc_code_inconsistencies.md) | current | Doc/code audit of 2026-08-03 and what was corrected (references several docs since merged into `modbus.md`) |
| [archive/](archive/) | historical | Early project/milestone plans, the pre-encryption FWU architecture, and the 2026-03 `architecture.dot` state diagram |

> **2026-08-08:** ten Modbus documents were consolidated into
> [modbus.md](modbus.md) and deleted — the two bridge/config design specs, their
> two implementation plans, the usage reference, the common-driver direction
> doc, the refactor kickoff, the full-implementation plan, the JK BMS task, and
> both integration-test docs. Completed implementation plans were purged rather
> than merged; only their still-binding decisions survive, stated where they
> belong in `modbus.md` rather than as history.

Diagrams: `upload_500k_sequence.{dot,png,svg}` — HTTP upload path sequence,
generated from the `.dot` with Graphviz.

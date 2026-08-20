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

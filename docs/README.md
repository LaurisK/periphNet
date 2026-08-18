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
| [task_nv_db.md](task_nv_db.md) | direction | **`nvDb` — the application's only path to non-volatile storage.** Two rules: a client knows nothing it does not need, and nothing reaches flash except through `nvDb`. §1 is the whole pattern — clients and object classes (FW blobs, Modbus LUTs, parameters, time base), the bootloader/crash-handler exception contract, flash opacity, configurable layout, self-healing, deferred reclaim with a collector, wear measurement. §2 makes each checkable, §3 is the API. §4 storage is **provisional**; §6 is the migration list |
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

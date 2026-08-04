# PeriphNet Documentation Index

Status legend: **current** = describes shipped behaviour, keep updated ·
**plan** = approved implementation plan, updated when implementation deviates ·
**direction** = observations/proposal, nothing frozen yet ·
**historical** = superseded, kept for rationale.

`CLAUDE.md` in the repo root is the entry point: build/flash/test commands,
memory map, HTTP API, task table, coding standards.

| Doc | Status | Covers |
|-----|--------|--------|
| [modbus_mqtt_usage.md](modbus_mqtt_usage.md) | current | Config JSON format, HTTP endpoints, CLI commands, poll/publish/availability behaviour, HA discovery, resource usage |
| [modbus_multi_device_config_design.md](modbus_multi_device_config_design.md) | current | Design spec for the uploadable multi-device register config (records, compiler, A/B regions) |
| [impl_modbus_multi_device_config.md](impl_modbus_multi_device_config.md) | plan | Phased implementation of the above + deviations recorded during implementation |
| [impl_modbus_mqtt_integration_tests.md](impl_modbus_mqtt_integration_tests.md) | plan | Integration harness design and Trice/CLI test contracts (banner at top records the 2026-07-07 inject-contract change) |
| [task_modbus_mqtt_integration_tests.md](task_modbus_mqtt_integration_tests.md) | current | Task spec the integration-test plan answers |
| [pylontech_can_protocol.md](pylontech_can_protocol.md) | current | Pylontech CAN frame layout used by `App/Can/` |
| [issue_idle_iwdg_crashloop.md](issue_idle_iwdg_crashloop.md) | current | Post-mortem: EthIf stack overflow → IWDG crash loop; includes the halt-during-hang debug recipe |
| [design_remote_access_and_autonomy.md](design_remote_access_and_autonomy.md) | direction | Bridge → edge controller (BMS + CAN), WAN access across NAT, RS485 bus budget, open questions |
| [issue_doc_code_inconsistencies.md](issue_doc_code_inconsistencies.md) | current | Doc/code audit of 2026-08-03 and what was corrected |
| [mqtt_modbus_bridge_design.md](mqtt_modbus_bridge_design.md) | historical | Original hardcoded single-device bridge design (superseded by the config redesign) |
| [archive/](archive/) | historical | Early project/milestone plans, the pre-encryption FWU architecture, and the 2026-03 `architecture.dot` state diagram |

Diagrams: `upload_500k_sequence.{dot,png,svg}` — HTTP upload path sequence,
generated from the `.dot` with Graphviz.

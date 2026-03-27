# Task: Modbus/MQTT Integration Test Infrastructure

## Goal

Enable automated integration testing of the Modbus/MQTT bridge without requiring
physical hardware. Physical RS485 and a real MQTT broker remain supported as
additive options.

---

## Context

The host-side integration test application (`tests/integration/`) communicates
with a live board over USB/UART/UDP, sends commands, and reads decoded Trice
output. Existing tests cover general commands and BMS CAN loopback. Modbus and
MQTT subsystems have no integration test coverage today.

---

## Use Cases

### Modbus side

The Modbus subsystem must be testable through three independently controlled
capabilities:

**Port selection** — the active RS485 port can be switched at runtime between the
default production port (UART2), a test port directly accessible via USB-serial
(UART6), and a disabled mode where no physical bus is involved. Disabled mode is
the baseline for software-only testing.

**Frame monitoring** — Trice streaming of raw Modbus TX and RX frames can be
enabled or disabled on demand. Off by default to avoid stream noise; enabled
during tests to observe bus activity.

**Frame injection** — a raw Modbus frame can be submitted via command at any time.
The firmware processes it as if it had been received from the bus. This allows the
test application to simulate slave responses without a physical device.

Together, disabled port + monitor + inject allow full software testing of the
Modbus processing path: inject a response, observe via Trice that it was parsed
correctly and that data flowed onward to the MQTT bridge.

### MQTT side

The MQTT subsystem must be testable through two independently controlled
capabilities, mirroring the Modbus side:

**Message monitoring** — Trice streaming of MQTT publish and subscribe payloads,
on demand.

**Message injection** — a topic and payload can be submitted via command at any
time. The firmware processes it as if it had been received from the broker. A
broker connection is not required.

### Integration (bridge)

With both sides' tools available, the full bridge can be tested in software:
inject a Modbus response on one side, observe via monitor that the correct MQTT
publish appears on the other side — and vice versa.

Physical RS485 (UART6 via USB-serial) and a real MQTT broker (Mosquitto) can
replace or supplement injection for hardware-in-the-loop testing, using the same
monitor tooling for observation.

---

## Deliverables

1. **Firmware**: runtime commands for port selection, monitor toggle, and frame
   injection on both the Modbus and MQTT subsystems.

2. **Integration tests**: new test cases in the host test application covering the
   software-only path (port=disabled, no broker) as the minimum CI-runnable set.
   Physical-hardware test cases are optional/skipped by default, following the
   pattern of existing BMS loopback tests.

---

## Acceptance Criteria

- Injecting a valid Modbus response (port disabled, monitor on) produces the
  correct MQTT publish, observable via Trice, without a physical bus or broker.
- Injecting a malformed Modbus frame produces an error in Trice and no data is
  forwarded to MQTT.
- Injecting an MQTT message for a known writable register triggers the correct
  Modbus write, observable via Trice.
- All new integration tests pass on a live board with no additional hardware.
- No regression in existing tests.

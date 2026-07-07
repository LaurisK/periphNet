#pragma once

class TestRunner;

/// Modbus/MQTT bridge integration tests (software-only CI sequence +
/// hardware-skip UART6 tests). Tests run sequentially and share state:
/// modbus_setup configures port=disabled + monitors, modbus_teardown
/// restores everything.
void registerModbusTests(TestRunner& runner);

#pragma once

#include <string>

class TestRunner;

/// Modbus/MQTT bridge integration tests (software-only CI sequence +
/// hardware-skip tests). Tests run sequentially and share state:
/// modbus_setup configures port=disabled + monitors, modbus_provision
/// uploads the fixture config over HTTP to `deviceIp` (there is no built-in
/// default — docs/modbus.md §4.2), modbus_teardown restores everything.
void registerModbusTests(TestRunner& runner, const std::string& deviceIp);

#include "core/ModbusTests.h"
#include "core/TestRunner.h"
#include "core/Device.h"

#include <chrono>
#include <thread>

/* ----------------------------------------------------------------------------
 * Modbus/MQTT bridge — software-only integration tests
 *
 * The whole CI sequence lives in this file so registration order preserves
 * setup → tests → teardown (see docs/impl_modbus_mqtt_integration_tests.md).
 *
 * No physical RS485 bus and no MQTT broker are required:
 *   - modbus port disabled  → transactions time out instantly, inject-only
 *   - modbus inject <hex>   → frame processed as if received from the bus
 *   - mqtt inject <t> <p>   → message processed as if from the broker
 *   - monitors + "mqtt publish now" make the bridge observable via Trice
 * -------------------------------------------------------------------------- */

namespace {

TestOutcome makePass(const std::string& name, const std::string& msg = "")
{
    return {name, TestResult::Pass, msg};
}

TestOutcome makeFail(const std::string& name, const std::string& msg)
{
    return {name, TestResult::Fail, msg};
}

TestOutcome makeSkip(const std::string& name, const std::string& msg)
{
    return {name, TestResult::Skip, msg};
}

bool linesContain(const std::vector<std::string>& lines, const std::string& sub)
{
    for (const auto& l : lines) {
        if (l.find(sub) != std::string::npos) return true;
    }
    return false;
}

/* Valid 45-byte FC04 battery+load response (20 regs @ wire 3132):
 * batVoltage=51.2V, batCurrent=-5.0A, SOC=85%, SOH=99%.
 * CRC 0x6886 verified against Modbus_CRC16 (bytes: 86 68). */
const char* kValidBatteryFrame =
    "0104280200ffce00000000000000000055006300000000000000000000"
    "00000000000000000000000000008668";

/* Same structure, 2-register frame with corrupted CRC (valid would be 3a58) */
const char* kBadCrcFrame = "0104040200ffcedead";

} // namespace

void registerModbusTests(TestRunner& runner)
{
    runner.addTest("modbus_setup",
        "Configure software-only test mode: port=disabled, monitors on, start both",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* Stop anything a previous run left active (silent if stopped) */
            dev.sendCommand("modbus stop");
            dev.drain(1000);
            dev.sendCommand("mqtt stop");
            dev.drain(1000);

            if (!dev.sendAndExpect("modbus port disabled", "Modbus port: disabled", 2000)) {
                return makeFail("modbus_setup", "step 'modbus port disabled' failed");
            }
            if (!dev.sendAndExpect("modbus monitor on", "Modbus monitor: on", 1000)) {
                return makeFail("modbus_setup", "step 'modbus monitor on' failed");
            }
            if (!dev.sendAndExpect("mqtt monitor on", "MQTT monitor: on", 1000)) {
                return makeFail("modbus_setup", "step 'mqtt monitor on' failed");
            }
            if (!dev.sendAndExpect("modbus start", "Modbus: polling", 2000)) {
                return makeFail("modbus_setup", "step 'modbus start' failed");
            }
            if (!dev.sendAndExpect("mqtt start", "MQTT: bridge starting", 2000)) {
                return makeFail("modbus_setup", "step 'mqtt start' failed");
            }

            return makePass("modbus_setup", "port=disabled, monitors on, poller+bridge running");
        });

    runner.addTest("modbus_valid_inject",
        "Inject valid 45-byte battery response, verify parse (51.2V, SOC=85%)",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect(std::string("modbus inject ") + kValidBatteryFrame,
                                        "Modbus inject: 45 bytes", 2000, &lines);
            if (!ok) {
                return makeFail("modbus_valid_inject", "No 'Modbus inject: 45 bytes' ack");
            }
            if (!linesContain(lines, "Modbus RX[45]:")) {
                return makeFail("modbus_valid_inject", "Monitor did not show 'Modbus RX[45]:'");
            }

            ok = dev.sendAndExpect("modbus read", "SOC=85%", 2000, &lines);
            if (!ok) {
                return makeFail("modbus_valid_inject", "SOC=85% not in register cache");
            }
            if (!linesContain(lines, "51.2")) {
                return makeFail("modbus_valid_inject", "Battery voltage 51.2V not in cache");
            }

            return makePass("modbus_valid_inject", "Injected frame parsed: 51.2V SOC=85%");
        });

    runner.addTest("modbus_valid_inject_mqtt_publish",
        "Verify injected data reaches MQTT publish path (broker-less)",
        [](Device& dev) -> TestOutcome {
            dev.drain(300);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("mqtt publish now",
                                        "MQTT pub: periphnet/battery_soc = 85",
                                        2000, &lines);
            if (!ok) {
                return makeFail("modbus_valid_inject_mqtt_publish",
                                "No 'MQTT pub: periphnet/battery_soc = 85'");
            }
            /* battery_voltage is published before battery_soc — must be in lines */
            if (!linesContain(lines, "periphnet/battery_voltage = 51.2")) {
                return makeFail("modbus_valid_inject_mqtt_publish",
                                "battery_voltage = 51.2 not published");
            }

            return makePass("modbus_valid_inject_mqtt_publish",
                            "battery_soc=85, battery_voltage=51.2 published");
        });

    runner.addTest("modbus_bad_crc",
        "Inject frame with corrupted CRC, expect ERR_CRC and no data forwarded",
        [](Device& dev) -> TestOutcome {
            dev.drain(300);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect(std::string("modbus inject ") + kBadCrcFrame,
                                        "Modbus inject: ERR_CRC", 1500, &lines);
            if (!ok) {
                return makeFail("modbus_bad_crc", "No 'Modbus inject: ERR_CRC'");
            }
            /* Rejected frame must not be acked as accepted */
            if (linesContain(lines, "Modbus inject: 9 bytes")) {
                return makeFail("modbus_bad_crc", "Frame with bad CRC was accepted");
            }

            return makePass("modbus_bad_crc", "Bad CRC rejected, nothing forwarded");
        });

    runner.addTest("modbus_short_frame",
        "Inject 2-byte frame, expect ERR_SHORT",
        [](Device& dev) -> TestOutcome {
            dev.drain(300);

            if (!dev.sendAndExpect("modbus inject 0104", "Modbus inject: ERR_SHORT", 1000)) {
                return makeFail("modbus_short_frame", "No 'Modbus inject: ERR_SHORT'");
            }
            return makePass("modbus_short_frame", "Short frame rejected");
        });

    runner.addTest("mqtt_inject_write",
        "Inject MQTT set message, verify Modbus write queued (reg 3010 = 15)",
        [](Device& dev) -> TestOutcome {
            dev.drain(300);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("mqtt inject periphnet/overdischarge_soc/set 15",
                                        "Modbus write: reg 3010 = 15", 2000, &lines);
            if (!ok) {
                return makeFail("mqtt_inject_write", "No 'Modbus write: reg 3010 = 15'");
            }
            if (!linesContain(lines, "MQTT inject: periphnet/overdischarge_soc/set")) {
                return makeFail("mqtt_inject_write", "No MQTT inject ack for set topic");
            }

            return makePass("mqtt_inject_write", "set 15 → write reg 3010 queued");
        });

    runner.addTest("mqtt_inject_echo",
        "Inject second writable register, verify ack + write (reg 3009 = 95)",
        [](Device& dev) -> TestOutcome {
            dev.drain(300);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("mqtt inject periphnet/max_charge_soc/set 95",
                                        "Modbus write: reg 3009 = 95", 2000, &lines);
            if (!ok) {
                return makeFail("mqtt_inject_echo", "No 'Modbus write: reg 3009 = 95'");
            }
            if (!linesContain(lines, "MQTT inject: periphnet/max_charge_soc/set")) {
                return makeFail("mqtt_inject_echo", "No MQTT inject ack for set topic");
            }

            return makePass("mqtt_inject_echo", "set 95 → write reg 3009 queued");
        });

    runner.addTest("modbus_teardown",
        "Stop both subsystems, restore port=uart2 and monitors off",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            if (!dev.sendAndExpect("modbus stop", "Modbus: stopped", 5000)) {
                return makeFail("modbus_teardown", "Modbus poller did not stop");
            }
            if (!dev.sendAndExpect("mqtt stop", "MQTT: stopped", 5000)) {
                return makeFail("modbus_teardown", "MQTT bridge did not stop");
            }
            dev.drain(500);

            if (!dev.sendAndExpect("modbus monitor off", "Modbus monitor: off", 1000)) {
                return makeFail("modbus_teardown", "modbus monitor off failed");
            }
            if (!dev.sendAndExpect("mqtt monitor off", "MQTT monitor: off", 1000)) {
                return makeFail("modbus_teardown", "mqtt monitor off failed");
            }
            if (!dev.sendAndExpect("modbus port uart2", "Modbus port: uart2", 2000)) {
                return makeFail("modbus_teardown", "port restore to uart2 failed");
            }

            if (!dev.sendAndExpect("modbus status", "stopped", 2000)) {
                return makeFail("modbus_teardown", "modbus status not showing stopped");
            }
            if (!dev.sendAndExpect("mqtt status", "stopped", 2000)) {
                return makeFail("modbus_teardown", "mqtt status not showing stopped");
            }

            return makePass("modbus_teardown", "Both stopped, port + monitors restored");
        });

    /* ------------------------------------------------------------------
     * Optional hardware tests (UART6 via USB-serial RS485 adapter).
     * UART6 pins are not confirmed in the schematic yet — the firmware
     * refuses 'modbus port uart6'.  Skipped until that lands.
     * ------------------------------------------------------------------ */

    runner.addTest("modbus_hw_uart6_loopback",
        "Switch to uart6 and exchange a known frame (requires USB-serial adapter)",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("modbus_hw_uart6_loopback",
                            "Hardware test: requires UART6 USB-serial adapter (deferred)");
        });

    runner.addTest("modbus_hw_uart6_timing",
        "Verify CRC-correct response within timeout on uart6",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("modbus_hw_uart6_timing",
                            "Hardware test: requires UART6 USB-serial adapter (deferred)");
        });

    runner.addTest("modbus_hw_uart6_stop",
        "Restore port=disabled after uart6 tests",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("modbus_hw_uart6_stop",
                            "Hardware test: requires UART6 USB-serial adapter (deferred)");
        });
}

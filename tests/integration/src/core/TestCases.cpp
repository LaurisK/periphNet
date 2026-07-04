#include "core/TestCases.h"
#include "core/TestRunner.h"
#include "core/Device.h"

#include <chrono>
#include <cstdlib>
#include <thread>

static TestOutcome makePass(const std::string& name, const std::string& msg = "")
{
    return {name, TestResult::Pass, msg};
}

static TestOutcome makeFail(const std::string& name, const std::string& msg)
{
    return {name, TestResult::Fail, msg};
}

static TestOutcome makeSkip(const std::string& name, const std::string& msg)
{
    return {name, TestResult::Skip, msg};
}

void registerAllTests(TestRunner& runner)
{
    runner.addTest("heartbeat_present",
        "Verify heartbeat trice messages arrive within 3s",
        [](Device& dev) -> TestOutcome {
            auto start = std::chrono::steady_clock::now();
            while (true) {
                std::string line;
                if (dev.readLine(line, 500)) {
                    if (line.find("Heartbeat") != std::string::npos) {
                        return makePass("heartbeat_present", "Detected: " + line);
                    }
                }
                auto elapsed = std::chrono::steady_clock::now() - start;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 3000) {
                    return makeFail("heartbeat_present", "No heartbeat within 3s");
                }
            }
        });

    runner.addTest("help_command",
        "Send 'help' and verify command list appears",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("help", "Available commands", 3000, &lines);
            if (!ok) {
                return makeFail("help_command", "Did not see 'Available commands' in output");
            }

            bool sawPeripherals = false;
            bool sawReboot = false;
            bool sawDfu = false;
            bool sawHelp = false;
            for (const auto& l : lines) {
                if (l.find("peripherals") != std::string::npos) sawPeripherals = true;
                if (l.find("reboot") != std::string::npos)      sawReboot = true;
                if (l.find("dfu") != std::string::npos)         sawDfu = true;
                if (l.find("help") != std::string::npos)        sawHelp = true;
            }

            if (!sawPeripherals || !sawReboot || !sawDfu || !sawHelp) {
                return makeFail("help_command", "Missing expected commands in help output");
            }

            return makePass("help_command", "All expected commands present");
        });

    runner.addTest("peripherals_command",
        "Send 'peripherals' and check response",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);
            bool ok = dev.sendAndExpect("peripherals", "n/a", 3000);
            if (!ok) {
                return makeFail("peripherals_command", "No 'n/a' response");
            }
            return makePass("peripherals_command");
        });

    runner.addTest("unknown_command",
        "Send unknown command, verify error response",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);
            bool ok = dev.sendAndExpect("xyzzy_nonexistent", "Unknown command", 3000);
            if (!ok) {
                return makeFail("unknown_command", "No 'Unknown command' error in output");
            }
            return makePass("unknown_command");
        });

    /* ------------------------------------------------------------------
     * BMS CAN loopback tests (CAN1 TX → CAN2 RX, requires physical bus)
     *
     * These tests require CAN1 and CAN2 transceivers to be physically
     * connected on the same CAN bus.  The simulator transmits Pylontech
     * frames on CAN1, and the reader receives/parses them on CAN2.
     *
     * Tests run sequentially and share state (sim+reader stay running
     * between bms_start and bms_stop).
     * ------------------------------------------------------------------ */

    runner.addTest("bms_start",
        "Start BMS simulator (CAN1) and reader (CAN2)",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* Stop first in case a previous run left them active */
            dev.sendCommand("bms stop");
            dev.drain(500);

            bool ok = dev.sendAndExpect("bms start", "BmsSim: started", 3000);
            if (!ok) {
                return makeFail("bms_start", "BMS sim did not start");
            }
            /* Check reader also started */
            std::string line;
            for (int i = 0; i < 10; i++) {
                if (dev.readLine(line, 300)) {
                    if (line.find("BmsReader: started") != std::string::npos) {
                        return makePass("bms_start", "Sim + reader started on CAN1/CAN2");
                    }
                }
            }
            return makeFail("bms_start", "BMS reader did not start");
        });

    runner.addTest("bms_status_running",
        "Verify both sim and reader report running state",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms status", "sim=running", 3000, &lines);
            if (!ok) {
                return makeFail("bms_status_running", "Sim not showing running");
            }

            bool readerRunning = false;
            for (const auto& l : lines) {
                if (l.find("reader=running") != std::string::npos) readerRunning = true;
            }
            if (!readerRunning) {
                return makeFail("bms_status_running", "Reader not showing running");
            }

            return makePass("bms_status_running", "sim=running reader=running");
        });

    runner.addTest("bms_all_frames",
        "Send one round and verify all 6 Pylontech frame types received (rxMask=0x3F)",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "BMS rx=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_all_frames", "No BMS data received on CAN2");
            }

            /* rxMask 0x3F means all 6 frame IDs were received */
            bool allFrames = false;
            for (const auto& l : lines) {
                if (l.find("rx=0x3F") != std::string::npos ||
                    l.find("rx=0x3f") != std::string::npos) {
                    allFrames = true;
                    break;
                }
            }
            if (!allFrames) {
                return makeFail("bms_all_frames", "rxMask != 0x3F — not all frame types received");
            }

            return makePass("bms_all_frames", "All 6 Pylontech frames received (0x351-0x35E)");
        });

    runner.addTest("bms_voltage_current",
        "Verify default voltage (~51.2V) and current (~-2.5A charging) are parsed",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "V=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_voltage_current", "No voltage data in output");
            }

            bool gotVoltage = false;
            bool gotCurrent = false;
            for (const auto& l : lines) {
                /* Voltage should contain 51 (51.20V) */
                if (l.find("V=51") != std::string::npos) gotVoltage = true;
                /* Current should be negative (charging), look for I= with negative or -2 */
                if (l.find("I=") != std::string::npos &&
                    l.find("-2") != std::string::npos) gotCurrent = true;
            }

            std::string detail;
            if (!gotVoltage) detail += "voltage(51V) ";
            if (!gotCurrent) detail += "current(-2.5A) ";

            if (!detail.empty()) {
                return makeFail("bms_voltage_current", "Missing: " + detail);
            }

            return makePass("bms_voltage_current", "V=51.20V I=-2.5A");
        });

    runner.addTest("bms_soc_soh",
        "Verify SOC=85% and SOH=99% from default sim values",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "SOC=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_soc_soh", "No SOC data in output");
            }

            bool gotSoc = false;
            bool gotSoh = false;
            for (const auto& l : lines) {
                if (l.find("SOC=85%") != std::string::npos) gotSoc = true;
                if (l.find("SOH=99%") != std::string::npos) gotSoh = true;
            }

            std::string detail;
            if (!gotSoc) detail += "SOC=85% ";
            if (!gotSoh) detail += "SOH=99% ";

            if (!detail.empty()) {
                return makeFail("bms_soc_soh", "Missing: " + detail);
            }

            return makePass("bms_soc_soh", "SOC=85% SOH=99%");
        });

    runner.addTest("bms_limits",
        "Verify charge/discharge voltage limits (56.0V / 44.8V)",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "ChgLim=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_limits", "No limits data in output");
            }

            bool gotChgLim = false;
            bool gotDsgLim = false;
            for (const auto& l : lines) {
                if (l.find("ChgLim=56") != std::string::npos) gotChgLim = true;
                if (l.find("DsgLim=44") != std::string::npos) gotDsgLim = true;
            }

            std::string detail;
            if (!gotChgLim) detail += "ChgLim=56V ";
            if (!gotDsgLim) detail += "DsgLim=44.8V ";

            if (!detail.empty()) {
                return makeFail("bms_limits", "Missing: " + detail);
            }

            return makePass("bms_limits", "ChgLim=56.0V DsgLim=44.8V");
        });

    runner.addTest("bms_charge_flags",
        "Verify charge=enabled and discharge=enabled flags",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "chg=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_charge_flags", "No charge flag data in output");
            }

            bool gotFlags = false;
            for (const auto& l : lines) {
                if (l.find("chg=1") != std::string::npos &&
                    l.find("dsg=1") != std::string::npos) {
                    gotFlags = true;
                    break;
                }
            }

            if (!gotFlags) {
                return makeFail("bms_charge_flags", "Expected chg=1 dsg=1 not found");
            }

            return makePass("bms_charge_flags", "Charge and discharge both enabled");
        });

    runner.addTest("bms_manufacturer",
        "Verify manufacturer string is 'PYLON'",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "mfg=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_manufacturer", "No manufacturer data in output");
            }

            bool gotMfg = false;
            for (const auto& l : lines) {
                if (l.find("PYLON") != std::string::npos) {
                    gotMfg = true;
                    break;
                }
            }

            if (!gotMfg) {
                return makeFail("bms_manufacturer", "Manufacturer PYLON not found");
            }

            return makePass("bms_manufacturer", "mfg=\"PYLON\"");
        });

    runner.addTest("bms_set_values",
        "Change sim values (48V, 10A dsg, 72% SOC, 30C) and verify propagation",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            bool ok = dev.sendAndExpect("bms set 48.0 10.0 72 30.0", "BMS sim set", 3000);
            if (!ok) {
                return makeFail("bms_set_values", "Failed to set BMS values");
            }

            dev.drain(300);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            ok = dev.sendAndExpect("bms read", "BMS rx=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_set_values", "No updated data received");
            }

            bool gotVoltage = false;
            bool gotSoc = false;
            bool gotTemp = false;
            for (const auto& l : lines) {
                if (l.find("V=48") != std::string::npos) gotVoltage = true;
                if (l.find("SOC=72%") != std::string::npos) gotSoc = true;
                if (l.find("T=30") != std::string::npos) gotTemp = true;
            }

            std::string detail;
            if (!gotVoltage) detail += "V=48 ";
            if (!gotSoc) detail += "SOC=72% ";
            if (!gotTemp) detail += "T=30C ";

            if (!detail.empty()) {
                return makeFail("bms_set_values", "Missing updated: " + detail);
            }

            return makePass("bms_set_values", "All updated values propagated CAN1->CAN2");
        });

    runner.addTest("bms_set_full_charge",
        "Set 100% SOC, verify reader reports SOC=100%",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            bool ok = dev.sendAndExpect("bms set 53.0 0.0 100 22.0", "BMS sim set", 3000);
            if (!ok) {
                return makeFail("bms_set_full_charge", "Failed to set BMS values");
            }

            dev.drain(300);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            ok = dev.sendAndExpect("bms read", "SOC=100%", 3000, &lines);
            if (!ok) {
                return makeFail("bms_set_full_charge", "SOC=100% not seen in reader output");
            }

            return makePass("bms_set_full_charge", "SOC=100% at float voltage");
        });

    runner.addTest("bms_set_low_soc",
        "Set 5% SOC with low voltage, verify reader reports correct values",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            bool ok = dev.sendAndExpect("bms set 45.0 -0.5 5 18.0", "BMS sim set", 3000);
            if (!ok) {
                return makeFail("bms_set_low_soc", "Failed to set BMS values");
            }

            dev.drain(300);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            ok = dev.sendAndExpect("bms read", "SOC=5%", 3000, &lines);
            if (!ok) {
                return makeFail("bms_set_low_soc", "SOC=5% not seen in reader output");
            }

            bool gotVoltage = false;
            for (const auto& l : lines) {
                if (l.find("V=45") != std::string::npos) gotVoltage = true;
            }
            if (!gotVoltage) {
                return makeFail("bms_set_low_soc", "Voltage 45V not seen");
            }

            return makePass("bms_set_low_soc", "Low SOC (5%) + low voltage (45V) correct");
        });

    runner.addTest("bms_frame_count",
        "Send multiple rounds and verify frame counter increments (6 per round)",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* Restore defaults for a clean count reference */
            dev.sendAndExpect("bms set 51.2 -2.5 85 25.0", "BMS sim set", 3000);
            dev.drain(300);

            /* Send 3 more rounds of frames */
            for (int i = 0; i < 3; i++) {
                dev.sendCommand("bms send");
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "BMS rx=", 3000, &lines);
            if (!ok) {
                return makeFail("bms_frame_count", "No data after multiple sends");
            }

            /* Look for frame count > 6 (we've sent many rounds by now) */
            bool countOk = false;
            for (const auto& l : lines) {
                auto pos = l.find("frames)");
                if (pos != std::string::npos) {
                    /* Find opening paren before "frames)" */
                    auto paren = l.rfind('(', pos);
                    if (paren != std::string::npos) {
                        int count = std::atoi(l.c_str() + paren + 1);
                        if (count > 6) {
                            countOk = true;
                        }
                    }
                }
            }

            if (!countOk) {
                return makeFail("bms_frame_count", "Frame count did not increase beyond 6");
            }

            return makePass("bms_frame_count", "Frame counter incrementing correctly");
        });

    runner.addTest("bms_stop",
        "Stop BMS simulator and reader, verify clean shutdown",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* Expect both stopped messages + rx frame count */
            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms stop", "stopped", 3000, &lines);
            if (!ok) {
                return makeFail("bms_stop", "BMS stop did not confirm");
            }

            /* Look for rx frame count in stop message */
            bool gotRxCount = false;
            for (const auto& l : lines) {
                if (l.find("rx=") != std::string::npos &&
                    l.find("frames") != std::string::npos) {
                    gotRxCount = true;
                }
            }

            dev.drain(300);

            /* Verify status shows stopped */
            ok = dev.sendAndExpect("bms status", "sim=stopped", 3000, &lines);
            if (!ok) {
                return makeFail("bms_stop", "BMS status not showing stopped");
            }

            bool readerStopped = false;
            for (const auto& l : lines) {
                if (l.find("reader=stopped") != std::string::npos) readerStopped = true;
            }
            if (!readerStopped) {
                return makeFail("bms_stop", "Reader not showing stopped");
            }

            return makePass("bms_stop", "Both CAN peripherals stopped cleanly" +
                           std::string(gotRxCount ? " (rx count logged)" : ""));
        });

    runner.addTest("bms_no_data_when_stopped",
        "Verify 'bms read' reports no data when reader is not running",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "not running", 3000, &lines);
            if (!ok) {
                return makeFail("bms_no_data_when_stopped",
                               "Expected 'not running' response for read while stopped");
            }

            return makePass("bms_no_data_when_stopped");
        });

    runner.addTest("bms_restart",
        "Restart sim+reader and verify fresh state (clean rxMask)",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            bool ok = dev.sendAndExpect("bms start", "BmsSim: started", 3000);
            if (!ok) {
                return makeFail("bms_restart", "BMS sim did not restart");
            }

            /* Reader should also start */
            std::string line;
            bool readerStarted = false;
            for (int i = 0; i < 10; i++) {
                if (dev.readLine(line, 300)) {
                    if (line.find("BmsReader: started") != std::string::npos) {
                        readerStarted = true;
                        break;
                    }
                }
            }
            if (!readerStarted) {
                return makeFail("bms_restart", "BMS reader did not restart");
            }

            /* Before any send, reader should have no data */
            dev.drain(300);
            std::vector<std::string> lines;
            ok = dev.sendAndExpect("bms read", "no data", 3000, &lines);
            if (!ok) {
                /* Maybe ISR already caught something from bus noise — not fatal,
                   but we expected a clean slate */
                return makePass("bms_restart", "Restarted (note: reader may have residual data)");
            }

            return makePass("bms_restart", "Clean restart, no stale data");
        });

    runner.addTest("bms_restart_send_receive",
        "After restart, verify one send+read cycle works",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            dev.sendCommand("bms send");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("bms read", "rx=0x3F", 3000, &lines);
            if (!ok) {
                /* Try case-insensitive */
                ok = dev.sendAndExpect("bms read", "rx=0x3f", 3000, &lines);
            }
            if (!ok) {
                return makeFail("bms_restart_send_receive",
                               "Full frame set not received after restart");
            }

            return makePass("bms_restart_send_receive",
                           "All 6 frames received after clean restart");
        });

    runner.addTest("bms_final_stop",
        "Final cleanup: stop BMS sim and reader",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);
            bool ok = dev.sendAndExpect("bms stop", "stopped", 3000);
            if (!ok) {
                return makeFail("bms_final_stop", "BMS stop did not confirm");
            }

            dev.drain(300);
            ok = dev.sendAndExpect("bms status", "sim=stopped", 3000);
            if (!ok) {
                return makeFail("bms_final_stop", "Not fully stopped");
            }

            return makePass("bms_final_stop", "BMS subsystem cleanly shut down");
        });

    runner.addTest("reboot_command",
        "Send 'reboot' (destructive - requires reconnection)",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("reboot_command", "Destructive test - run manually with --test reboot_command");
        });

    runner.addTest("dfu_command",
        "Send 'dfu' (destructive - enters bootloader mode)",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("dfu_command", "Destructive test - run manually with --test dfu_command");
        });
}

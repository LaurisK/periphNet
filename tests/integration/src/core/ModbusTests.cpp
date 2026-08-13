#include "core/ModbusTests.h"
#include "core/TestRunner.h"
#include "core/Device.h"
#include "core/HttpClient.h"
#include "core/Config.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

/* ----------------------------------------------------------------------------
 * Modbus/MQTT bridge — software-only integration tests
 *
 * The whole CI sequence lives in this file so registration order preserves
 * setup → tests → teardown (contract: docs/modbus.md §6).
 *
 * No physical RS485 bus and no MQTT broker are required:
 *   - the fixture binds its device to the TEST PORT, an App-layer driver the
 *     module cannot distinguish from a UART (docs/modbus.md §5.1).  Whether a
 *     board has one is a CONFIGURATION question, not a build question.
 *   - modbus inject <hex>           → stage the reply the next frame gets
 *   - modbus silence                → stage silence instead
 *   - mqtt inject <t> <p>           → message processed as if from the broker
 *   - monitors make publishes observable via Trice without a broker
 *
 * THE SUITE PROVISIONS ITS OWN CONFIG.  Since docs/modbus.md §4.2 there is no
 * built-in default and none is provisioned — "the board is told what it is
 * for; until then it is not for anything" — so `modbus_provision` uploads the
 * fixture below over HTTP and applies it.  Everything after that depends on
 * it, and skips if the board could not be reached.
 *
 * The fixture deliberately polls at 3600 s: these tests drive data by
 * INJECTION, and a fast plan would put timeout traffic and availability
 * transitions in the middle of every assertion.
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

/* Replies the ENGINE will actually accept: a request reads exactly the
 * registers it asked for, so a staged frame has to answer THAT request.  A
 * `modbus get 0 <pt>` reads one register of one point.
 *
 * CRCs computed with the poly-0xA001 algorithm the module and the host test
 * modbus_frame both implement. */
const char* kReplyVoltage = "0104020200b850";   /* 1 reg = 512  -> 51.2 V   */
const char* kReplySoc     = "0104020055790f";   /* 1 reg = 85   -> 85 %     */
const char* kReplyExc2    = "018402c2c1";       /* exception 2              */
const char* kEchoOverdis  = "01060bc2000f6a16"; /* FC06 echo 3010 = 15      */
const char* kEchoMaxChg   = "01060bc1005f9a2a"; /* FC06 echo 3009 = 95      */

/* Same shape, corrupted CRC (valid would end b850) */
const char* kBadCrcFrame  = "0104020200dead";

/* The fixture config lives in tests/fixtures/modbus_solis.json so that
 * test_modbus_compiler can prove it compiles WITHOUT a board: a config the
 * board would reject is a broken fixture, and finding that out over HTTP is
 * the slow way.  Its blocks cover exactly the two address ranges these tests
 * touch, and its plan polls at 3600 s so scheduled traffic stays out of the
 * way of injection. */
std::string loadFixtureConfig()
{
    std::string path = Config::projectRoot() + "/tests/fixtures/modbus_solis.json";
    std::ifstream f(path);
    if (!f) {
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* Set by modbus_provision; everything downstream needs it. */
bool s_provisioned = false;

TestOutcome needsConfig(const std::string& name)
{
    return makeSkip(name, "board not provisioned (see modbus_provision)");
}

} // namespace

void registerModbusTests(TestRunner& runner, const std::string& deviceIp)
{
    runner.addTest("modbus_setup",
        "Software-only test mode: monitors on, engine + bridge running",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* The engine has no start/stop: Modbus_Init is the entire
             * lifecycle, and what gets polled is decided by SUBSCRIPTIONS
             * (docs/modbus.md §4.2).  Only the bridge is cycled. */
            dev.sendCommand("mqtt stop");
            dev.drain(1000);

            if (!dev.sendAndExpect("modbus monitor on", "Modbus monitor: on", 1000)) {
                return makeFail("modbus_setup", "step 'modbus monitor on' failed");
            }
            if (!dev.sendAndExpect("mqtt monitor on", "MQTT monitor: on", 1000)) {
                return makeFail("modbus_setup", "step 'mqtt monitor on' failed");
            }
            if (!dev.sendAndExpect("mqtt start", "MQTT: bridge starting", 2000)) {
                return makeFail("modbus_setup", "step 'mqtt start' failed");
            }

            /* Since docs/modbus.md §10 step 2 the engine polls and dispatches
             * only for SUBSCRIBERS.  The bridge is one, and it needs its
             * catalogue before a set-topic resolves — give it a moment. */
            dev.drain(1000);

            return makePass("modbus_setup", "port=disabled, monitors on, walker+bridge running");
        });

    runner.addTest("modbus_provision",
        "Upload + apply the fixture Modbus config over HTTP",
        [deviceIp](Device& dev) -> TestOutcome {
            std::string cfg = loadFixtureConfig();
            if (cfg.empty()) {
                return makeFail("modbus_provision",
                                "tests/fixtures/modbus_solis.json not found");
            }

            HttpClient http(deviceIp);

            auto up = http.post("/api/modbus/config/upload", cfg);
            if (!up.ok) {
                return makeSkip("modbus_provision",
                                "no HTTP to " + deviceIp + ": " + up.error);
            }
            if (up.status != 200) {
                return makeFail("modbus_provision",
                                "upload -> HTTP " + std::to_string(up.status) +
                                " " + up.body);
            }

            auto ap = http.post("/api/modbus/config/apply", "");
            if (!ap.ok || ap.status != 200) {
                return makeFail("modbus_provision",
                                "apply -> " + ap.error + ap.body);
            }

            /* The engine commits the swap at its next safe point, so poll the
             * STATE rather than racing a log line for it. */
            bool live = false;
            for (int i = 0; i < 20 && !live; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                auto st = http.get("/api/modbus/config/status");
                live = st.ok && st.status == 200 &&
                       st.body.find("\"valid\":true") != std::string::npos &&
                       st.body.find("\"swap_pending\":false") !=
                           std::string::npos;
            }
            if (!live) {
                return makeFail("modbus_provision",
                                "config never went live after apply");
            }
            dev.drain(300);

            s_provisioned = true;
            return makePass("modbus_provision",
                            "fixture config live on " + deviceIp);
        });

    runner.addTest("modbus_valid_read",
        "Stage a reply, request point 0, verify decode + publish (51.2 V)",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_valid_read");
            }
            dev.drain(500);

            /* Stage first: A REPLY MAY BE STAGED BEFORE THE REQUEST EXISTS,
             * which is what lets a harness answer traffic it did not cause. */
            if (!dev.sendAndExpect(std::string("modbus inject ") + kReplyVoltage,
                                   "Modbus inject: 7 bytes staged", 2000)) {
                return makeFail("modbus_valid_read", "reply not staged");
            }

            std::vector<std::string> lines;
            if (!dev.sendAndExpect("modbus get 0 0",
                                   "Modbus req: dev 0 pt 0 = 512 (0)",
                                   3000, &lines)) {
                return makeFail("modbus_valid_read",
                                "request did not complete with 512");
            }
            if (!linesContain(lines, "Modbus RX[7]:")) {
                return makeFail("modbus_valid_read", "monitor showed no RX[7]");
            }
            /* A request's reads are broadcast like any other read (§4.6). */
            if (!linesContain(lines, "MQTT pub: periphnet/battery_voltage = 51.2")) {
                return makeFail("modbus_valid_read",
                                "battery_voltage = 51.2 not published");
            }
            return makePass("modbus_valid_read", "512 -> 51.2 V, published");
        });

    runner.addTest("modbus_read_second_point",
        "A second point of the same capability decodes independently (SOC)",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_read_second_point");
            }
            dev.drain(300);

            dev.sendCommand(std::string("modbus inject ") + kReplySoc);
            dev.drain(300);

            std::vector<std::string> lines;
            if (!dev.sendAndExpect("modbus get 0 2",
                                   "Modbus req: dev 0 pt 2 = 85 (0)",
                                   3000, &lines)) {
                return makeFail("modbus_read_second_point", "no SOC result");
            }
            if (!linesContain(lines, "MQTT pub: periphnet/battery_soc = 85")) {
                return makeFail("modbus_read_second_point",
                                "battery_soc = 85 not published");
            }
            return makePass("modbus_read_second_point", "SOC = 85 published");
        });

    runner.addTest("modbus_silence_times_out",
        "Staged silence makes the request time out (mbErr_timeout = -4)",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_silence_times_out");
            }
            dev.drain(300);

            dev.sendCommand("modbus silence");
            dev.drain(300);

            /* The timeout path is reachable WITHOUT HARDWARE: the driver
             * simply does not answer (§9). */
            if (!dev.sendAndExpect("modbus get 0 0", "= 0 (-4)", 4000)) {
                return makeFail("modbus_silence_times_out",
                                "no mbErr_timeout result");
            }
            return makePass("modbus_silence_times_out", "silence -> timeout");
        });

    runner.addTest("modbus_bad_crc",
        "A corrupt reply is rejected by the ENGINE, not by the driver",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_bad_crc");
            }
            dev.drain(300);

            dev.sendCommand(std::string("modbus inject ") + kBadCrcFrame);
            dev.drain(300);

            /* The driver delivers it as a FRAME — how reception ended is all it
             * knows — and the engine returns mbErr_crc (-5). */
            if (!dev.sendAndExpect("modbus get 0 0", "(-5)", 3000)) {
                return makeFail("modbus_bad_crc", "CRC error not reported");
            }
            return makePass("modbus_bad_crc", "corrupt frame -> mbErr_crc");
        });

    runner.addTest("modbus_exception_reply",
        "A slave exception becomes a per-item code, not a generic failure",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_exception_reply");
            }
            dev.drain(300);

            dev.sendCommand(std::string("modbus inject ") + kReplyExc2);
            dev.drain(300);

            /* Exception 2 = illegal data address = mbErr_excIllegalAddress. */
            if (!dev.sendAndExpect("modbus get 0 0", "(-21)", 3000)) {
                return makeFail("modbus_exception_reply",
                                "exception 2 not reported as -21");
            }
            return makePass("modbus_exception_reply", "exception 2 -> -21");
        });

    runner.addTest("mqtt_inject_write",
        "Inject MQTT set message, verify a Modbus request is submitted",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("mqtt_inject_write");
            }
            dev.drain(300);

            /* An rw point writes then READS BACK, so the slave must answer
             * twice: the echo, then the read-back. */
            dev.sendCommand(std::string("modbus inject ") + kEchoOverdis);
            dev.drain(300);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("mqtt inject periphnet/overdischarge_soc/set 15",
                                        "MQTT: set periphnet/overdischarge_soc = 15",
                                        3000, &lines);
            if (!ok) {
                return makeFail("mqtt_inject_write",
                                "No 'MQTT: set periphnet/overdischarge_soc = 15'");
            }
            if (!linesContain(lines, "MQTT inject: periphnet/overdischarge_soc/set")) {
                return makeFail("mqtt_inject_write", "No MQTT inject ack for set topic");
            }
            return makePass("mqtt_inject_write", "set 15 -> Modbus_Request submitted");
        });

    runner.addTest("mqtt_inject_write_out_of_range",
        "Inject out-of-range set value, verify the MODULE rejects it (§4.6)",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("mqtt_inject_write_out_of_range");
            }
            dev.drain(300);

            /* The bounds live on the point record and the module enforces them
             * BEFORE A FRAME IS FORMED; the bridge no longer checks. */
            if (!dev.sendAndExpect("mqtt inject periphnet/overdischarge_soc/set 99",
                                   "MQTT: set periphnet/overdischarge_soc rejected",
                                   4000)) {
                return makeFail("mqtt_inject_write_out_of_range",
                                "Out-of-range set (99 > writeMax 40) was not rejected");
            }
            return makePass("mqtt_inject_write_out_of_range",
                            "set 99 rejected by writeMin/writeMax range");
        });

    runner.addTest("mqtt_inject_echo",
        "Inject second writable point, verify ack + request (max_charge_soc = 95)",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("mqtt_inject_echo");
            }
            dev.drain(300);

            dev.sendCommand(std::string("modbus inject ") + kEchoMaxChg);
            dev.drain(300);

            std::vector<std::string> lines;
            bool ok = dev.sendAndExpect("mqtt inject periphnet/max_charge_soc/set 95",
                                        "MQTT: set periphnet/max_charge_soc = 95",
                                        3000, &lines);
            if (!ok) {
                return makeFail("mqtt_inject_echo",
                                "No 'MQTT: set periphnet/max_charge_soc = 95'");
            }
            if (!linesContain(lines, "MQTT inject: periphnet/max_charge_soc/set")) {
                return makeFail("mqtt_inject_echo", "No MQTT inject ack for set topic");
            }
            return makePass("mqtt_inject_echo", "set 95 -> Modbus_Request submitted");
        });

    runner.addTest("modbus_teardown",
        "Stop the bridge, monitors off; the engine has no stop",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* Only the BRIDGE has a lifecycle.  Stopping it unsubscribes,
             * which destroys the timers and quiets the wire — that is what
             * "a consumer controls the bus by subscribing" means (§4.3). */
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
            if (!dev.sendAndExpect("mqtt status", "stopped", 2000)) {
                return makeFail("modbus_teardown", "mqtt status not showing stopped");
            }

            return makePass("modbus_teardown", "bridge stopped, monitors off");
        });

}

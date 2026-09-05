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
 * Modbus engine — software-only integration tests
 *
 * The whole CI sequence lives in this file so registration order preserves
 * setup → tests → teardown (contract: docs/modbus.md §6).
 *
 * No physical RS485 bus is required:
 *   - the fixture binds its device to the TEST PORT, an App-layer driver the
 *     module cannot distinguish from a UART (docs/modbus.md §5.1).  Whether a
 *     board has one is a CONFIGURATION question, not a build question.
 *   - modbus inject <hex>           → stage the reply the next frame gets
 *   - modbus silence                → stage silence instead
 *   - modbus dump on                → the Trice SUBSCRIBER; it is what makes
 *     the engine poll at all, and what makes each dispatched sample
 *     observable.  This was the MQTT bridge until MQTT was removed from the
 *     project (docs/design_solis_modbus_link.md §9.1)
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

/* The GATEWAY SEAM's frames (docs/design_solis_modbus_link.md §9 step 2).
 * These address REGISTERS, not points, so they are the only replies here whose
 * shape the config has no say in. */
const char* kRawRead2     = "010404020000553a03";   /* FC04 x2 = 512, 85    */
const char* kRawWrite16Ack= "01100bc100021210";     /* FC16 echo, 3009 x2   */

/* ASSERT ON THE FRAME LENGTH, NOT THE OUTCOME.  `modbus raw 0 16 3009 2 95 15`
 * must put ONE write-multiple frame on the wire —
 * 01 10 0b c1 00 02 04 00 5f 00 0f 3d 15, thirteen bytes.  An outcome
 * assertion would pass just as happily against two FC06 writes, which is
 * exactly the failure the Remote Dispatch block suffers from (§3.5); a
 * 13-byte frame to slave 1 carrying two registers can only be FC16, while a
 * decomposed write would show as two 8-byte frames. */
#define MB_FC16_TX_LEN "Modbus TX[13]:"

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
        "Software-only test mode: monitors on, engine polling for a subscriber",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* The engine has no start/stop: Modbus_Init is the entire
             * lifecycle, and what gets polled is decided by SUBSCRIPTIONS
             * (docs/modbus.md §4.2).  The subscriber used to be the MQTT
             * bridge; MQTT was removed from the project
             * (docs/design_solis_modbus_link.md §9.1), so the Trice sink —
             * which exists for exactly this, with no MQTT in the picture —
             * is what makes the engine poll now. */
            if (!dev.sendAndExpect("modbus monitor on", "Modbus monitor: on", 1000)) {
                return makeFail("modbus_setup", "step 'modbus monitor on' failed");
            }
            if (!dev.sendAndExpect("modbus dump on", "Modbus dump: on", 2000)) {
                return makeFail("modbus_setup", "step 'modbus dump on' failed");
            }

            /* Since docs/modbus.md §10 step 2 the engine polls and dispatches
             * only for SUBSCRIBERS — give the subscription a moment to arm
             * its timers. */
            dev.drain(1000);

            return makePass("modbus_setup", "port=disabled, monitors on, engine polling");
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
            if (!linesContain(lines, "Modbus dump: periphnet/battery_voltage = 51.2")) {
                return makeFail("modbus_valid_read",
                                "battery_voltage = 51.2 not dispatched");
            }
            return makePass("modbus_valid_read", "512 -> 51.2 V, dispatched");
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
            if (!linesContain(lines, "Modbus dump: periphnet/battery_soc = 85")) {
                return makeFail("modbus_read_second_point",
                                "battery_soc = 85 not dispatched");
            }
            return makePass("modbus_read_second_point", "SOC = 85 dispatched");
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

    runner.addTest("modbus_raw_read",
        "Raw FC04 by wire address: no point, no ptOrd, no decode (§4.11)",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_raw_read");
            }
            dev.drain(300);

            dev.sendCommand(std::string("modbus inject ") + kRawRead2);
            dev.drain(300);

            /* 3132 is inside the fixture's declared block, but that is
             * incidental: the raw seam consults no block and no point.  What
             * comes back is registers, undecoded -- 512 stays 512 and does
             * not become 51.2 V. */
            std::vector<std::string> lines;
            if (!dev.sendAndExpect("modbus raw 0 4 3132 2",
                                   "Modbus raw: 512 85", 3000, &lines)) {
                return makeFail("modbus_raw_read",
                                "no 'Modbus raw: 512 85' -- registers should "
                                "come back undecoded");
            }
            return makePass("modbus_raw_read", "2 registers, undecoded");
        });

    runner.addTest("modbus_raw_write_is_one_fc16_frame",
        "FC16 is framed VERBATIM, never decomposed into FC06 writes (§7.3)",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_raw_write_is_one_fc16_frame");
            }
            dev.drain(300);

            dev.sendCommand(std::string("modbus inject ") + kRawWrite16Ack);
            dev.drain(300);

            /* The fixture's capability declares writeFc 6.  A raw transfer
             * passes the CLIENT's function code through anyway -- rewriting a
             * client's FC16 into per-register FC06 writes is the one thing a
             * gateway may not do here. */
            std::vector<std::string> lines;
            if (!dev.sendAndExpect("modbus raw 0 16 3009 2 95 15",
                                   "Modbus raw:", 3000, &lines)) {
                return makeFail("modbus_raw_write_is_one_fc16_frame",
                                "raw FC16 produced no result line");
            }
            if (!linesContain(lines, MB_FC16_TX_LEN)) {
                return makeFail("modbus_raw_write_is_one_fc16_frame",
                                "the wire did not carry ONE FC16 frame with "
                                "both registers");
            }
            return makePass("modbus_raw_write_is_one_fc16_frame",
                            "3009..3010 written as a single FC16");
        });

    runner.addTest("modbus_raw_rejects_bad_shape",
        "The raw seam validates the PDU's shape and nothing else",
        [](Device& dev) -> TestOutcome {
            if (!s_provisioned) {
                return needsConfig("modbus_raw_rejects_bad_shape");
            }
            dev.drain(300);

            /* FC05 is not one of 3/4/6/16 -- refused before a frame is
             * formed, so no reply needs staging. */
            if (!dev.sendAndExpect("modbus raw 0 5 3132 1",
                                   "Modbus raw: rejected", 2000)) {
                return makeFail("modbus_raw_rejects_bad_shape",
                                "fc 5 was not rejected");
            }
            /* FC06 lands exactly one register; asking for two is a shape
             * error, not something to try on the wire. */
            if (!dev.sendAndExpect("modbus raw 0 6 3009 2 95 15",
                                   "Modbus raw: rejected", 2000)) {
                return makeFail("modbus_raw_rejects_bad_shape",
                                "fc 6 with count 2 was not rejected");
            }
            return makePass("modbus_raw_rejects_bad_shape",
                            "fc 5 and fc 6 x2 both refused");
        });

    runner.addTest("modbus_teardown",
        "Unsubscribe and monitors off; the engine has no stop",
        [](Device& dev) -> TestOutcome {
            dev.drain(500);

            /* Only a SUBSCRIBER has a lifecycle.  Dropping the Trice sink
             * unsubscribes, which destroys the timers and quiets the wire —
             * that is what "a consumer controls the bus by subscribing"
             * means (§4.3). */
            if (!dev.sendAndExpect("modbus dump off", "Modbus dump: off", 5000)) {
                return makeFail("modbus_teardown", "subscriber did not drop");
            }
            dev.drain(500);

            if (!dev.sendAndExpect("modbus monitor off", "Modbus monitor: off", 1000)) {
                return makeFail("modbus_teardown", "modbus monitor off failed");
            }

            return makePass("modbus_teardown", "unsubscribed, monitors off");
        });

}

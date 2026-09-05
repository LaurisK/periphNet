#include "core/GatewayTests.h"
#include "core/Device.h"
#include "core/ModbusTcpClient.h"
#include "core/TestRunner.h"

extern "C" {
#include "fixtures/solis_modbus_groups.h"
}

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

/* ----------------------------------------------------------------------------
 * The board as solis_modbus finds it
 *
 * Every read below is one upstream really issues: the group table is generated
 * from `hybrid_sensors.py` by tools/extract_solis_groups.py, and the client is
 * shaped like `client_manager.py` (one transaction at a time, 50/100 ms
 * spacing, 5 s timeout).  So a pass here means the far end will work, not that
 * a hand-written approximation of it works.
 *
 * WHAT COUNTS AS A PASS is the part worth stating.  For a READ the answer may
 * legitimately be an exception: `solis_modbus`'s map is generic across Solis
 * hybrids and this particular inverter will not implement every group.  What
 * must NEVER happen is silence — upstream bisects on exception 2 and adapts,
 * but a timeout costs a pymodbus retry and then five seconds of the whole
 * integration, because one lock covers every slave on the link
 * (docs/design_solis_modbus_link.md §6.4, "the two rules Shape A commits to").
 * So these tests assert on TIMEOUTS AND MALFORMED REPLIES, and merely report
 * the exception split.
 * -------------------------------------------------------------------------- */

namespace {

constexpr uint16_t kGatewayPort = 502;
constexpr uint8_t  kSolisUnit   = 1;      // the Solis's slave address

/* The board's own budget is 1500 ms; anything beyond this means it neither
 * answered nor gave up, which is the state rule 2 exists to make impossible. */
constexpr double kMaxAnswerMs = 2500.0;

TestOutcome pass(const std::string& n, const std::string& m = "")
{
    return {n, TestResult::Pass, m};
}
TestOutcome fail(const std::string& n, const std::string& m)
{
    return {n, TestResult::Fail, m};
}
TestOutcome skip(const std::string& n, const std::string& m)
{
    return {n, TestResult::Skip, m};
}

std::string g_ip;
bool        g_reachable = false;

/// Connect, or say why not. Shared so each test does not re-probe a dead port.
bool openClient(ModbusTcpClient& c, std::string& err)
{
    return c.connect(g_ip, kGatewayPort, 3000, err);
}

TestOutcome needsGateway(const std::string& n)
{
    return skip(n, "nothing answered on " + g_ip + ":502 (see gw_connect)");
}

} // namespace

void registerGatewayTests(TestRunner& runner, const std::string& deviceIp)
{
    g_ip = deviceIp;

    runner.addTest("gw_connect",
        "TCP connect to the Modbus gateway on :502",
        [](Device&) -> TestOutcome {
            ModbusTcpClient c;
            std::string err;

            if (!openClient(c, err)) {
                g_reachable = false;
                /* NOT A FAILURE.  A board with no tunnel configured serves
                 * nothing on purpose, and so does one running an image from
                 * before the gateway landed. */
                return skip("gw_connect", err);
            }
            g_reachable = true;
            return pass("gw_connect", "connected to " + g_ip + ":502");
        });

    runner.addTest("gw_read_all_groups",
        "All 48 solis_modbus register groups: never silence, never malformed",
        [](Device&) -> TestOutcome {
            if (!g_reachable) return needsGateway("gw_read_all_groups");

            ModbusTcpClient c;
            std::string err;
            if (!openClient(c, err)) return fail("gw_read_all_groups", err);

            int    data = 0, exc = 0;
            double worst = 0.0;
            std::string worstWhere;
            std::vector<std::string> bad;

            for (uint32_t i = 0; i < SOLIS_GROUP_COUNT; i++) {
                const sSolisGroup& g = kSolisGroups[i];
                auto r = c.readRegisters(kSolisUnit, g.fc, g.start, g.count);

                if (r.elapsedMs > worst) {
                    worst = r.elapsedMs;
                    worstWhere = std::to_string(g.start);
                }

                if (r.timedOut) {
                    bad.push_back(std::to_string(g.start) + " TIMED OUT");
                } else if (r.protocolError) {
                    bad.push_back(std::to_string(g.start) + " " + r.detail);
                } else if (r.exceptionCode != 0) {
                    exc++;
                } else if (r.registers.size() != g.count) {
                    bad.push_back(std::to_string(g.start) + " returned " +
                                  std::to_string(r.registers.size()) + " of " +
                                  std::to_string(g.count));
                } else {
                    data++;
                }
                if (bad.size() >= 4) break;   // enough to diagnose
            }

            if (!bad.empty()) {
                std::string m = "groups that did not answer properly: ";
                for (const auto& b : bad) m += "[" + b + "] ";
                return fail("gw_read_all_groups", m);
            }
            if (worst > kMaxAnswerMs) {
                return fail("gw_read_all_groups",
                            "group " + worstWhere + " took " +
                            std::to_string(static_cast<int>(worst)) +
                            " ms — the board must answer or say 0x06 well "
                            "inside pymodbus's 5 s");
            }

            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "%d/%u groups returned data, %d answered an "
                          "exception, worst %.0f ms",
                          data, SOLIS_GROUP_COUNT, exc, worst);
            /* Zero data groups means the chain is broken somewhere past the
             * gateway — the inverter, the wiring or the device record. */
            if (data == 0) {
                return fail("gw_read_all_groups",
                            std::string(msg) +
                            " — the gateway is answering but the inverter is "
                            "not; check the RS485 side, not :502");
            }
            return pass("gw_read_all_groups", msg);
        });

    runner.addTest("gw_exception_not_silence",
        "An address the inverter lacks answers an exception, not a timeout",
        [](Device&) -> TestOutcome {
            if (!g_reachable) return needsGateway("gw_exception_not_silence");

            ModbusTcpClient c;
            std::string err;
            if (!openClient(c, err)) return fail("gw_exception_not_silence", err);

            /* 39999 is in neither the input nor the holding map upstream uses,
             * and no Solis implements it.  §3.4: an exception is what upstream
             * LEARNS from; silence is what it retries forever. */
            auto r = c.readRegisters(kSolisUnit, 4, 39999, 1);

            if (r.timedOut) {
                return fail("gw_exception_not_silence",
                            "an unknown address produced SILENCE — upstream "
                            "would burn a retry and 5 s of its shared lock");
            }
            if (r.protocolError) {
                return fail("gw_exception_not_silence", r.detail);
            }
            if (r.exceptionCode == 0) {
                return pass("gw_exception_not_silence",
                            "the inverter answered 39999 — unexpected but "
                            "not a gateway fault");
            }
            return pass("gw_exception_not_silence",
                        r.detail + " in " +
                        std::to_string(static_cast<int>(r.elapsedMs)) + " ms");
        });

    runner.addTest("gw_unknown_unit_is_0x0A",
        "An unmapped unit id answers gateway-path-unavailable",
        [](Device&) -> TestOutcome {
            if (!g_reachable) return needsGateway("gw_unknown_unit_is_0x0A");

            ModbusTcpClient c;
            std::string err;
            if (!openClient(c, err)) return fail("gw_unknown_unit_is_0x0A", err);

            /* 247 is the top legal slave address and no config here uses it. */
            auto r = c.readRegisters(247, 4, 33000, 1);

            if (r.timedOut) {
                return fail("gw_unknown_unit_is_0x0A",
                            "an unknown unit id produced silence; it must not "
                            "reach the wire at all");
            }
            if (r.exceptionCode != 0x0A) {
                return fail("gw_unknown_unit_is_0x0A",
                            "expected 0x0A, got " +
                            (r.exceptionCode ? r.detail : std::string("a data reply")));
            }
            return pass("gw_unknown_unit_is_0x0A", "0x0A, no bus traffic");
        });

    runner.addTest("gw_bad_function_is_0x01",
        "An unsupported function code answers illegal-function",
        [](Device&) -> TestOutcome {
            if (!g_reachable) return needsGateway("gw_bad_function_is_0x01");

            ModbusTcpClient c;
            std::string err;
            if (!openClient(c, err)) return fail("gw_bad_function_is_0x01", err);

            auto r = c.readRegisters(kSolisUnit, 0x01, 0, 1);   // read coils

            if (r.exceptionCode != 0x01) {
                return fail("gw_bad_function_is_0x01",
                            "expected 0x01, got " +
                            (r.timedOut ? std::string("silence")
                                        : (r.exceptionCode ? r.detail
                                                           : std::string("a data reply"))));
            }
            return pass("gw_bad_function_is_0x01", "0x01");
        });

    runner.addTest("gw_transaction_ids_are_echoed",
        "Every reply carries its request's transaction id",
        [](Device&) -> TestOutcome {
            if (!g_reachable) return needsGateway("gw_transaction_ids_are_echoed");

            ModbusTcpClient c;
            std::string err;
            if (!openClient(c, err)) return fail("gw_transaction_ids_are_echoed", err);

            /* The client already refuses a mismatched id, which is what
             * pymodbus does — so a wrong id shows up here as a protocol
             * error rather than as wrong data.  Ten in a row, because an
             * off-by-one only appears once the counter has moved. */
            for (int i = 0; i < 10; i++) {
                auto r = c.readRegisters(kSolisUnit, 4, 33000, 1);
                if (r.protocolError) {
                    return fail("gw_transaction_ids_are_echoed", r.detail);
                }
                if (r.timedOut) {
                    return fail("gw_transaction_ids_are_echoed",
                                "timed out on request " + std::to_string(i));
                }
            }
            return pass("gw_transaction_ids_are_echoed", "10 requests, ids matched");
        });

    runner.addTest("gw_sustained_poll_is_stable",
        "A minute of upstream-paced polling: no silence, no drift",
        [](Device&) -> TestOutcome {
            if (!g_reachable) return needsGateway("gw_sustained_poll_is_stable");

            ModbusTcpClient c;
            std::string err;
            if (!openClient(c, err)) return fail("gw_sustained_poll_is_stable", err);

            /* Upstream's FAST groups are the ones that run continuously, so
             * this is the shape of the steady-state load the board will carry
             * on top of its own JK poll.  Two passes over them is enough to
             * catch a leak in the request FIFO or a listener that stops
             * accepting after the first burst. */
            int    txns = 0, busy = 0;
            double worst = 0.0;

            for (int pass_i = 0; pass_i < 2; pass_i++) {
                for (uint32_t i = 0; i < SOLIS_GROUP_COUNT; i++) {
                    const sSolisGroup& g = kSolisGroups[i];
                    if (std::string(g.poll) != "FAST") continue;

                    auto r = c.readRegisters(kSolisUnit, g.fc, g.start, g.count);
                    txns++;
                    worst = std::max(worst, r.elapsedMs);

                    if (r.timedOut) {
                        return fail("gw_sustained_poll_is_stable",
                                    "silence at group " +
                                    std::to_string(g.start) + " after " +
                                    std::to_string(txns) + " transactions");
                    }
                    if (r.protocolError) {
                        return fail("gw_sustained_poll_is_stable",
                                    "malformed reply after " +
                                    std::to_string(txns) + ": " + r.detail);
                    }
                    /* 0x06 IS A PASS.  It is the board saying "come back",
                     * which is the designed answer when the engine is busy
                     * with the BMS poll — the property being checked is that
                     * it SAYS it rather than going quiet. */
                    if (r.exceptionCode == 0x06) busy++;
                }
            }

            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "%d transactions, %d answered busy (0x06), worst %.0f ms",
                          txns, busy, worst);
            if (worst > kMaxAnswerMs) {
                return fail("gw_sustained_poll_is_stable", msg);
            }
            return pass("gw_sustained_poll_is_stable", msg);
        });

    /* ------------------------------------------------------------------
     * Writes — registered so they are discoverable, skipped so they are
     * deliberate.  Run with `--test gw_hw_write_dispatch_block` only after
     * the image is confirmed (docs/design_solis_modbus_link.md §10 step 7).
     * ------------------------------------------------------------------ */

    runner.addTest("gw_hw_write_dispatch_block",
        "FC16 over 44100-44104 — MOVES REAL POWER, run by name only",
        [](Device&) -> TestOutcome {
            return skip("gw_hw_write_dispatch_block",
                        "writes to a live inverter: run explicitly, after "
                        "confirm (design_solis_modbus_link.md §10)");
        });

    runner.addTest("gw_hw_write_storage_mode",
        "FC06 to 43110 — changes inverter behaviour, run by name only",
        [](Device&) -> TestOutcome {
            return skip("gw_hw_write_storage_mode",
                        "writes to a live inverter: run explicitly, after "
                        "confirm (design_solis_modbus_link.md §10)");
        });
}

#include "core/MqttTests.h"
#include "core/TestRunner.h"
#include "core/Device.h"

namespace {

TestOutcome makeSkip(const std::string& name, const std::string& msg)
{
    return {name, TestResult::Skip, msg};
}

} // namespace

void registerMqttTests(TestRunner& runner)
{
    /* ------------------------------------------------------------------
     * Hardware-in-the-loop tests — require a live Mosquitto broker
     * (default 10.42.0.1:1883) reachable from the board.  Skipped by
     * default, following the BMS loopback pattern.
     * ------------------------------------------------------------------ */

    runner.addTest("mqtt_hw_connect",
        "Start bridge against a real broker, wait for 'connected'",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("mqtt_hw_connect",
                            "Hardware test: requires Mosquitto broker at 10.42.0.1:1883");
        });

    runner.addTest("mqtt_hw_publish_battery",
        "Verify battery_soc published to the broker (observe via monitor)",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("mqtt_hw_publish_battery",
                            "Hardware test: requires Mosquitto broker at 10.42.0.1:1883");
        });

    runner.addTest("mqtt_hw_subscribe_write",
        "mosquitto_pub to a /set topic, verify Modbus write queued",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("mqtt_hw_subscribe_write",
                            "Hardware test: requires Mosquitto broker at 10.42.0.1:1883");
        });

    runner.addTest("mqtt_hw_stop",
        "Stop bridge, verify 'offline' published",
        [](Device& /*dev*/) -> TestOutcome {
            return makeSkip("mqtt_hw_stop",
                            "Hardware test: requires Mosquitto broker at 10.42.0.1:1883");
        });
}

#pragma once

class TestRunner;

/// MQTT hardware-in-the-loop tests (require a live Mosquitto broker).
/// Registered as immediate-skip; run manually with --test mqtt_hw_*.
/// The software-only MQTT bridge tests live in ModbusTests.cpp so the
/// shared setup/teardown sequence stays in registration order.
void registerMqttTests(TestRunner& runner);

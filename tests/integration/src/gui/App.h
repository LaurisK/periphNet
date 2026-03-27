#pragma once

#include "core/Config.h"
#include "core/Device.h"
#include "core/Log.h"
#include "core/TestRunner.h"
#include "core/TestCases.h"

#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

/// Central GUI application state shared across all tabs.
class App {
public:
    App();
    ~App();

    void render();

    // Configuration
    Config config;

    // Device
    std::unique_ptr<Device> device;
    bool connected = false;
    std::string statusMessage = "Disconnected";

    void connect();
    void disconnect();

    // Trice log buffer (fed by background reader thread)
    LogBuffer logBuffer;
    std::thread logReaderThread;
    std::atomic<bool> logReaderRunning{false};
    void startLogReader();
    void stopLogReader();

    // Test runner
    TestRunner testRunner;
    std::vector<TestOutcome> testResults;
    std::atomic<bool> testsRunning{false};
    std::thread testThread;
    void runTests();

    // Console command history
    std::vector<std::string> consoleHistory;
};

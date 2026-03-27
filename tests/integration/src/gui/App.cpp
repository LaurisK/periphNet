#include "gui/App.h"

App::App()
{
    registerAllTests(testRunner);
}

App::~App()
{
    disconnect();
    if (testThread.joinable()) {
        testThread.join();
    }
}

void App::connect()
{
    disconnect();

    config.resolveToolPaths();
    device = createDevice(config);

    if (device->connect()) {
        connected = true;
        statusMessage = "Connected";
        startLogReader();
    } else {
        statusMessage = "Connection failed";
        device.reset();
    }
}

void App::disconnect()
{
    stopLogReader();

    if (device) {
        device->disconnect();
        device.reset();
    }

    connected = false;
    statusMessage = "Disconnected";
}

void App::startLogReader()
{
    if (logReaderRunning) return;
    logReaderRunning = true;

    logReaderThread = std::thread([this]() {
        while (logReaderRunning && device) {
            std::string line;
            if (device->readLine(line, 200)) {
                logBuffer.append(line);
            }
        }
    });
}

void App::stopLogReader()
{
    logReaderRunning = false;
    if (logReaderThread.joinable()) {
        logReaderThread.join();
    }
}

void App::runTests()
{
    if (testsRunning || !connected) return;

    testsRunning = true;
    testResults.clear();

    // Temporarily stop the log reader so the test runner can read trice lines
    stopLogReader();

    testThread = std::thread([this]() {
        testResults = testRunner.runAll(*device, config.testFilter);
        testsRunning = false;

        // Restart log reader after tests complete
        if (connected) {
            startLogReader();
        }
    });
}

void App::render()
{
    // Rendering is done by the tab classes in main.cpp
}

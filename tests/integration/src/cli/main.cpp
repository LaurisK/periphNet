#include "core/Config.h"
#include "core/Device.h"
#include "core/TestRunner.h"
#include "core/TestCases.h"

#include <cstdio>
#include <iostream>

int main(int argc, char* argv[])
{
    Config config = Config::fromArgs(argc, argv);

    TestRunner runner;
    registerAllTests(runner);

    if (config.listTests) {
        std::printf("Available tests:\n");
        for (const auto& tc : runner.tests()) {
            std::printf("  %-25s %s\n", tc.name.c_str(), tc.description.c_str());
        }
        return 0;
    }

    auto device = createDevice(config);

    std::printf("Connecting (%s mode)...\n",
                config.mode == TransportMode::Usb    ? "USB" :
                config.mode == TransportMode::Udp    ? "UDP" :
                                                       "Serial");

    if (!device->connect()) {
        std::fprintf(stderr, "Failed to connect to device\n");
        return 1;
    }

    std::printf("Connected. Running tests...\n\n");

    auto results = runner.runAll(*device, config.testFilter,
        [](const TestOutcome& o) {
            const char* tag =
                (o.result == TestResult::Pass) ? "\033[32mPASS\033[0m" :
                (o.result == TestResult::Fail) ? "\033[31mFAIL\033[0m" :
                                                 "\033[33mSKIP\033[0m";
            std::printf("  [%s] %-25s %6.0fms  %s\n",
                        tag, o.testName.c_str(), o.durationMs, o.message.c_str());
        });

    device->disconnect();

    // Summary
    int pass = 0, fail = 0, skip = 0;
    for (const auto& r : results) {
        switch (r.result) {
            case TestResult::Pass: pass++; break;
            case TestResult::Fail: fail++; break;
            case TestResult::Skip: skip++; break;
        }
    }

    std::printf("\n%d passed, %d failed, %d skipped\n", pass, fail, skip);
    return fail > 0 ? 1 : 0;
}

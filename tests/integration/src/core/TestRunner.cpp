#include "core/TestRunner.h"
#include "core/Device.h"

#include <algorithm>
#include <chrono>

void TestRunner::addTest(const std::string& name, const std::string& desc, TestFunc func)
{
    tests_.push_back({name, desc, std::move(func)});
}

std::vector<TestOutcome> TestRunner::runAll(Device& device,
                                            const std::vector<std::string>& filter,
                                            ProgressCallback cb)
{
    std::vector<TestOutcome> results;

    for (const auto& tc : tests_) {
        // Apply filter if specified
        if (!filter.empty()) {
            auto it = std::find(filter.begin(), filter.end(), tc.name);
            if (it == filter.end()) continue;
        }

        auto start = std::chrono::steady_clock::now();

        TestOutcome outcome;
        try {
            outcome = tc.func(device);
        } catch (const std::exception& e) {
            outcome = {tc.name, TestResult::Fail,
                       std::string("Exception: ") + e.what()};
        }

        auto end = std::chrono::steady_clock::now();
        outcome.durationMs = std::chrono::duration<double, std::milli>(end - start).count();
        outcome.testName = tc.name;

        results.push_back(outcome);
        if (cb) cb(outcome);
    }

    return results;
}

#pragma once

#include <string>
#include <vector>
#include <functional>

class Device;

enum class TestResult { Pass, Fail, Skip };

struct TestOutcome {
    std::string testName;
    TestResult result;
    std::string message;
    double durationMs = 0.0;
};

using TestFunc = std::function<TestOutcome(Device& device)>;

struct TestCase {
    std::string name;
    std::string description;
    TestFunc func;
};

class TestRunner {
public:
    void addTest(const std::string& name, const std::string& desc, TestFunc func);

    /// Run all tests (or filtered subset). Callback invoked after each test.
    using ProgressCallback = std::function<void(const TestOutcome&)>;
    std::vector<TestOutcome> runAll(Device& device,
                                    const std::vector<std::string>& filter = {},
                                    ProgressCallback cb = nullptr);

    const std::vector<TestCase>& tests() const { return tests_; }

private:
    std::vector<TestCase> tests_;
};

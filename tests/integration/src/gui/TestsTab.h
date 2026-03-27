#pragma once

class App;

/// Tests tab: run tests, view pass/fail results.
class TestsTab {
public:
    explicit TestsTab(App& app) : app_(app) {}
    void render();

private:
    App& app_;
};

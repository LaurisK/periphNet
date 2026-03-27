#pragma once

#include <cstddef>

class App;

/// Trice log streaming tab: auto-scrolling log viewer.
class TriceLogTab {
public:
    explicit TriceLogTab(App& app) : app_(app) {}
    void render();

private:
    App& app_;
    size_t lastIndex_ = 0;
    bool autoScroll_ = true;
    char filterBuf_[256] = "";
};

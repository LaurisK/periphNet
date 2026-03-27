#pragma once

class App;

/// Console tab: manual command entry with history.
class ConsoleTab {
public:
    explicit ConsoleTab(App& app) : app_(app) {}
    void render();

private:
    App& app_;
    char inputBuf_[256] = "";
    bool focusInput_ = false;
};

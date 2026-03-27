#pragma once

class App;

/// Settings tab: transport mode, port paths, connect/disconnect.
class SettingsTab {
public:
    explicit SettingsTab(App& app) : app_(app) {}
    void render();

private:
    App& app_;
    char usbPort_[256] = "/dev/ttyACM0";
    char udpCmdPort_[256] = "/dev/ttyACM0";
    char serialTricePort_[256] = "/dev/ttyUSB0";
    char serialCmdPort_[256] = "/dev/ttyUSB1";
    int serialTriceBaud_ = 460800;
    int serialCmdBaud_ = 115200;
    int udpPort_ = 17001;
    int modeIdx_ = 0; // 0=USB, 1=UDP, 2=Serial
};

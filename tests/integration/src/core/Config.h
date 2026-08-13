#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <memory>

class Device;

enum class TransportMode { Usb, Udp, Serial };

struct Config {
    TransportMode mode = TransportMode::Usb;

    // USB mode: single port for trice + commands
    std::string usbPort = "/dev/ttyACM0";

    // UDP mode: trice via UDP, commands via serial
    uint16_t udpPort = 17001;
    std::string udpCommandPort = "/dev/ttyACM0";

    // Serial mode: separate ports for trice and commands
    std::string serialTricePort = "/dev/ttyUSB0";
    int serialTriceBaud = 460800;
    std::string serialCommandPort = "/dev/ttyUSB1";
    int serialCommandBaud = 115200;

    // HTTP control plane (config upload; there is no built-in default config)
    std::string deviceIp = "10.42.0.203";

    // Tool paths (auto-detected, overridable)
    std::string triceBinary;
    std::string tilJson;
    std::string liJson;

    // Test filtering
    std::vector<std::string> testFilter;
    bool listTests = false;

    /// Parse from command-line arguments.
    static Config fromArgs(int argc, char* argv[]);

    /// Resolve project root directory.
    static std::string projectRoot();

    /// Fill in triceBinary/tilJson/liJson from project root if not set.
    void resolveToolPaths();
};

/// Factory: creates Device with correct transport stack based on config.
std::unique_ptr<Device> createDevice(const Config& config);

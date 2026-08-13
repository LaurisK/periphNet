#include "core/Config.h"
#include "core/Device.h"
#include "core/transport/UsbTriceSource.h"
#include "core/transport/UdpTriceSource.h"
#include "core/transport/SerialTriceSource.h"
#include "core/transport/SerialCommandTransport.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/stat.h>

static void printUsage(const char* prog)
{
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Transport:\n"
        << "  --mode usb|udp|serial   Transport mode (default: usb)\n"
        << "  --port PATH             USB serial port (default: /dev/ttyACM0)\n"
        << "  --trice-port PATH       Trice serial port (serial mode)\n"
        << "  --trice-baud RATE       Trice serial baud (default: 460800)\n"
        << "  --cmd-port PATH         Command serial port (serial/udp mode)\n"
        << "  --cmd-baud RATE         Command serial baud (default: 115200)\n"
        << "  --udp-port PORT         UDP listen port (default: 17001)\n"
        << "  --ip ADDR               Board IPv4 for HTTP config upload\n"
        << "                          (default: 10.42.0.203)\n"
        << "\n"
        << "Paths:\n"
        << "  --trice-bin PATH        Path to trice binary\n"
        << "  --project-root PATH     Project root (for til.json, li.json, trice)\n"
        << "\n"
        << "Tests:\n"
        << "  --list                  List available tests and exit\n"
        << "  --test NAME             Run only named test (repeatable)\n"
        << "  --help                  Show this help\n";
}

Config Config::fromArgs(int argc, char* argv[])
{
    Config cfg;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        auto nextArg = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--ip") {
            cfg.deviceIp = nextArg();
        } else if (arg == "--mode") {
            std::string m = nextArg();
            if (m == "usb")         cfg.mode = TransportMode::Usb;
            else if (m == "udp")    cfg.mode = TransportMode::Udp;
            else if (m == "serial") cfg.mode = TransportMode::Serial;
            else {
                std::cerr << "Unknown mode: " << m << "\n";
                std::exit(1);
            }
        } else if (arg == "--port") {
            cfg.usbPort = nextArg();
        } else if (arg == "--trice-port") {
            cfg.serialTricePort = nextArg();
        } else if (arg == "--trice-baud") {
            cfg.serialTriceBaud = std::stoi(nextArg());
        } else if (arg == "--cmd-port") {
            cfg.serialCommandPort = nextArg();
            cfg.udpCommandPort = cfg.serialCommandPort;
        } else if (arg == "--cmd-baud") {
            cfg.serialCommandBaud = std::stoi(nextArg());
        } else if (arg == "--udp-port") {
            cfg.udpPort = static_cast<uint16_t>(std::stoi(nextArg()));
        } else if (arg == "--trice-bin") {
            cfg.triceBinary = nextArg();
        } else if (arg == "--project-root") {
            // Override project root for tool path resolution
            std::string root = nextArg();
            cfg.triceBinary = root + "/tools/trice";
            cfg.tilJson = root + "/til.json";
            cfg.liJson = root + "/li.json";
        } else if (arg == "--list") {
            cfg.listTests = true;
        } else if (arg == "--test") {
            cfg.testFilter.push_back(nextArg());
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    cfg.resolveToolPaths();
    return cfg;
}

std::string Config::projectRoot()
{
    // 1. Environment variable
    const char* env = std::getenv("PERIPHNET_ROOT");
    if (env && env[0]) {
        return env;
    }

    // 2. Compile-time macro
#ifdef PERIPHNET_PROJECT_ROOT
    return PERIPHNET_PROJECT_ROOT;
#else
    return ".";
#endif
}

void Config::resolveToolPaths()
{
    std::string root = projectRoot();

    if (triceBinary.empty()) {
        triceBinary = root + "/tools/trice";
    }
    if (tilJson.empty()) {
        tilJson = root + "/til.json";
    }
    if (liJson.empty()) {
        liJson = root + "/li.json";
    }
}

std::unique_ptr<Device> createDevice(const Config& config)
{
    std::unique_ptr<ITriceSource> triceSource;
    std::unique_ptr<ICommandTransport> cmdTransport;

    switch (config.mode) {
    case TransportMode::Usb: {
        auto usb = std::make_unique<UsbTriceSource>(
            config.usbPort,
            config.triceBinary,
            config.tilJson,
            config.liJson);

        // Command transport borrows the USB serial FD (set after connect)
        auto cmd = std::make_unique<SerialCommandTransport>();
        // The Device will wire these together after start()
        triceSource = std::move(usb);
        cmdTransport = std::move(cmd);
        break;
    }
    case TransportMode::Udp: {
        triceSource = std::make_unique<UdpTriceSource>(
            config.udpPort,
            config.triceBinary,
            config.tilJson,
            config.liJson);

        cmdTransport = std::make_unique<SerialCommandTransport>(
            config.udpCommandPort, 115200);
        break;
    }
    case TransportMode::Serial: {
        triceSource = std::make_unique<SerialTriceSource>(
            config.serialTricePort,
            config.serialTriceBaud,
            config.triceBinary,
            config.tilJson,
            config.liJson);

        cmdTransport = std::make_unique<SerialCommandTransport>(
            config.serialCommandPort, config.serialCommandBaud);
        break;
    }
    }

    return std::make_unique<Device>(std::move(triceSource), std::move(cmdTransport));
}

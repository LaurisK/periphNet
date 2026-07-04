#pragma once

#include "core/transport/ICommandTransport.h"
#include "core/SerialPort.h"

#include <string>
#include <memory>

/// Sends newline-terminated ASCII commands over a serial port.
/// Can own its own port (dedicated UART) or borrow an FD (USB shared mode).
class SerialCommandTransport : public ICommandTransport {
public:
    /// Default constructor for borrowed-FD mode (USB). Call borrowFd() before use.
    SerialCommandTransport();

    /// Constructor for dedicated serial port mode.
    SerialCommandTransport(std::string path, int baud);

    ~SerialCommandTransport() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool sendCommand(const std::string& cmd) override;
    void borrowFd(int fd) override;

private:
    std::unique_ptr<SerialPort> ownedPort_;
    int fd_ = -1;
};

#pragma once

#include <string>

/// Abstract interface for sending ASCII commands to the device.
class ICommandTransport {
public:
    virtual ~ICommandTransport() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    /// Send a command string (newline is appended automatically).
    virtual bool sendCommand(const std::string& cmd) = 0;

    /// Borrow an FD from a shared trice source (USB mode).
    virtual void borrowFd(int fd) { (void)fd; }
};

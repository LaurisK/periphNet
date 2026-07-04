#pragma once

#include <string>

/// Abstract interface for reading decoded trice output lines.
class ITriceSource {
public:
    virtual ~ITriceSource() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    /// Read one decoded trice line. Blocks up to timeoutMs.
    /// Returns false on timeout or EOF.
    virtual bool readLine(std::string& out, int timeoutMs) = 0;

    /// For USB mode: returns the serial FD so command transport can borrow it.
    /// Returns -1 by default (non-shared transports).
    virtual int sharedFd() const { return -1; }
};

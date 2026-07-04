#pragma once

#include "core/transport/ITriceSource.h"
#include "core/transport/ICommandTransport.h"

#include <memory>
#include <string>
#include <vector>

/// High-level device communication: send commands, wait for trice responses.
class Device {
public:
    Device(std::unique_ptr<ITriceSource> triceSource,
           std::unique_ptr<ICommandTransport> commandTransport);
    ~Device();

    bool connect();
    void disconnect();
    bool isConnected() const;

    /// Send a command (newline appended automatically).
    bool sendCommand(const std::string& cmd);

    /// Send command, then wait for a trice line containing expectSubstring within timeoutMs.
    /// Optionally collects all lines received during the wait.
    bool sendAndExpect(const std::string& cmd,
                       const std::string& expectSubstring,
                       int timeoutMs = 3000,
                       std::vector<std::string>* allLines = nullptr);

    /// Read the next trice line (blocks up to timeoutMs).
    bool readLine(std::string& out, int timeoutMs = 1000);

    /// Drain all available trice lines for up to timeoutMs.
    std::vector<std::string> drain(int timeoutMs = 500);

    ITriceSource& triceSource() { return *triceSource_; }

private:
    std::unique_ptr<ITriceSource> triceSource_;
    std::unique_ptr<ICommandTransport> commandTransport_;
    bool connected_ = false;
};

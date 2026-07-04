#include "core/Device.h"

#include <chrono>
#include <utility>

Device::Device(std::unique_ptr<ITriceSource> triceSource,
               std::unique_ptr<ICommandTransport> commandTransport)
    : triceSource_(std::move(triceSource))
    , commandTransport_(std::move(commandTransport))
{
}

Device::~Device()
{
    disconnect();
}

bool Device::connect()
{
    if (connected_) return true;

    if (!triceSource_->start()) {
        return false;
    }

    // In USB shared-FD mode, wire the command transport to the trice source's serial FD
    int sharedFd = triceSource_->sharedFd();
    if (sharedFd >= 0) {
        commandTransport_->borrowFd(sharedFd);
    }

    if (!commandTransport_->open()) {
        triceSource_->stop();
        return false;
    }

    connected_ = true;
    return true;
}

void Device::disconnect()
{
    if (!connected_) return;

    commandTransport_->close();
    triceSource_->stop();
    connected_ = false;
}

bool Device::isConnected() const
{
    return connected_;
}

bool Device::sendCommand(const std::string& cmd)
{
    if (!connected_) return false;
    return commandTransport_->sendCommand(cmd);
}

bool Device::sendAndExpect(const std::string& cmd,
                           const std::string& expectSubstring,
                           int timeoutMs,
                           std::vector<std::string>* allLines)
{
    if (!sendCommand(cmd)) {
        return false;
    }

    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        int elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        if (elapsedMs >= timeoutMs) {
            return false;
        }

        int remaining = timeoutMs - elapsedMs;
        int readTimeout = (remaining < 500) ? remaining : 500;

        std::string line;
        if (readLine(line, readTimeout)) {
            if (allLines) {
                allLines->push_back(line);
            }
            if (line.find(expectSubstring) != std::string::npos) {
                return true;
            }
        }
    }
}

bool Device::readLine(std::string& out, int timeoutMs)
{
    if (!connected_) return false;
    return triceSource_->readLine(out, timeoutMs);
}

std::vector<std::string> Device::drain(int timeoutMs)
{
    std::vector<std::string> lines;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        int elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        if (elapsedMs >= timeoutMs) break;

        std::string line;
        int remaining = timeoutMs - elapsedMs;
        int readTimeout = (remaining < 100) ? remaining : 100;

        if (readLine(line, readTimeout)) {
            lines.push_back(std::move(line));
        }
    }

    return lines;
}

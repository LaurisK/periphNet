#pragma once

#include "core/transport/ITriceSource.h"
#include "core/TriceProcess.h"

#include <string>
#include <cstdint>

/// UDP trice source: trice tool handles the UDP socket directly.
/// Simplest source — no serial port, no FIFO, no reader thread.
class UdpTriceSource : public ITriceSource {
public:
    UdpTriceSource(uint16_t port,
                   std::string triceBinary,
                   std::string tilPath,
                   std::string liPath);
    ~UdpTriceSource() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;
    bool readLine(std::string& out, int timeoutMs) override;

private:
    TriceProcess trice_;
};

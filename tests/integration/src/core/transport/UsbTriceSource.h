#pragma once

#include "core/transport/ITriceSource.h"
#include "core/SerialPort.h"
#include "core/TriceProcess.h"

#include <thread>
#include <atomic>
#include <string>

/// USB CDC trice source: reads raw serial data, pipes through FIFO to trice subprocess.
/// Shares the serial FD with SerialCommandTransport for command writing.
class UsbTriceSource : public ITriceSource {
public:
    UsbTriceSource(std::string serialPath,
                   std::string triceBinary,
                   std::string tilPath,
                   std::string liPath);
    ~UsbTriceSource() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;
    bool readLine(std::string& out, int timeoutMs) override;
    int sharedFd() const override;

private:
    SerialPort serial_;
    TriceProcess trice_;
    std::thread readerThread_;
    std::atomic<bool> running_{false};

    void readerLoop();
};

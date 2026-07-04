#pragma once

#include "core/transport/ITriceSource.h"
#include "core/SerialPort.h"
#include "core/TriceProcess.h"

#include <thread>
#include <atomic>
#include <string>

/// Serial trice source: reads raw UART data, pipes through FIFO to trice subprocess.
/// Used for dedicated trice UART (e.g., USART3 at 460800 baud).
class SerialTriceSource : public ITriceSource {
public:
    SerialTriceSource(std::string serialPath,
                      int baud,
                      std::string triceBinary,
                      std::string tilPath,
                      std::string liPath);
    ~SerialTriceSource() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;
    bool readLine(std::string& out, int timeoutMs) override;

private:
    SerialPort serial_;
    TriceProcess trice_;
    std::thread readerThread_;
    std::atomic<bool> running_{false};

    void readerLoop();
};

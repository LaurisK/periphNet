#include "core/transport/UsbTriceSource.h"

#include <utility>

UsbTriceSource::UsbTriceSource(std::string serialPath,
                               std::string triceBinary,
                               std::string tilPath,
                               std::string liPath)
    : serial_(std::move(serialPath))
    , trice_(std::move(triceBinary), std::move(tilPath), std::move(liPath), "FILE", "")
{
}

UsbTriceSource::~UsbTriceSource()
{
    stop();
}

bool UsbTriceSource::start()
{
    if (running_) return true;

    if (!serial_.open()) {
        return false;
    }

    if (!trice_.start()) {
        serial_.close();
        return false;
    }

    running_ = true;
    readerThread_ = std::thread(&UsbTriceSource::readerLoop, this);

    return true;
}

void UsbTriceSource::stop()
{
    running_ = false;

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    trice_.stop();
    serial_.close();
}

bool UsbTriceSource::isRunning() const
{
    return running_;
}

bool UsbTriceSource::readLine(std::string& out, int timeoutMs)
{
    return trice_.readLine(out, timeoutMs);
}

int UsbTriceSource::sharedFd() const
{
    return serial_.fd();
}

void UsbTriceSource::readerLoop()
{
    uint8_t buf[256];

    while (running_) {
        ssize_t n = serial_.read(buf, sizeof(buf), 100);
        if (n > 0) {
            trice_.writeFifo(buf, static_cast<size_t>(n));
        }
    }
}

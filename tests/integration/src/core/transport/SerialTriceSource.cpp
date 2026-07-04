#include "core/transport/SerialTriceSource.h"

#include <utility>

SerialTriceSource::SerialTriceSource(std::string serialPath,
                                     int baud,
                                     std::string triceBinary,
                                     std::string tilPath,
                                     std::string liPath)
    : serial_(std::move(serialPath), baud)
    , trice_(std::move(triceBinary), std::move(tilPath), std::move(liPath), "FILE", "")
{
}

SerialTriceSource::~SerialTriceSource()
{
    stop();
}

bool SerialTriceSource::start()
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
    readerThread_ = std::thread(&SerialTriceSource::readerLoop, this);

    return true;
}

void SerialTriceSource::stop()
{
    running_ = false;

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    trice_.stop();
    serial_.close();
}

bool SerialTriceSource::isRunning() const
{
    return running_;
}

bool SerialTriceSource::readLine(std::string& out, int timeoutMs)
{
    return trice_.readLine(out, timeoutMs);
}

void SerialTriceSource::readerLoop()
{
    uint8_t buf[256];

    while (running_) {
        ssize_t n = serial_.read(buf, sizeof(buf), 100);
        if (n > 0) {
            trice_.writeFifo(buf, static_cast<size_t>(n));
        }
    }
}

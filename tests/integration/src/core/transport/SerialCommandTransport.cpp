#include "core/transport/SerialCommandTransport.h"

#include <unistd.h>
#include <utility>

SerialCommandTransport::SerialCommandTransport() = default;

SerialCommandTransport::SerialCommandTransport(std::string path, int baud)
    : ownedPort_(std::make_unique<SerialPort>(std::move(path), baud))
{
}

SerialCommandTransport::~SerialCommandTransport()
{
    close();
}

bool SerialCommandTransport::open()
{
    if (fd_ >= 0) return true;

    if (ownedPort_) {
        if (!ownedPort_->open()) return false;
        fd_ = ownedPort_->fd();
    }
    // Borrowed FD mode: fd_ set via borrowFd()
    return fd_ >= 0;
}

void SerialCommandTransport::close()
{
    if (ownedPort_) {
        ownedPort_->close();
    }
    fd_ = -1;
}

bool SerialCommandTransport::isOpen() const
{
    return fd_ >= 0;
}

bool SerialCommandTransport::sendCommand(const std::string& cmd)
{
    if (fd_ < 0) return false;

    std::string data = cmd + "\n";
    ssize_t written = ::write(fd_, data.c_str(), data.size());
    return written == static_cast<ssize_t>(data.size());
}

void SerialCommandTransport::borrowFd(int fd)
{
    fd_ = fd;
}

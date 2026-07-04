#include "core/SerialPort.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <utility>

SerialPort::SerialPort(std::string path, int baud)
    : path_(std::move(path)), baud_(baud)
{
}

SerialPort::~SerialPort()
{
    close();
}

SerialPort::SerialPort(SerialPort&& other) noexcept
    : path_(std::move(other.path_)), baud_(other.baud_), fd_(other.fd_)
{
    other.fd_ = -1;
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept
{
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        baud_ = other.baud_;
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

static speed_t baudToSpeed(int baud)
{
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

bool SerialPort::configure()
{
    struct termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        return false;
    }

    // Raw mode: no echo, no canonical processing, no signals
    cfmakeraw(&tty);

    // 8N1
    tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;

    // No hardware flow control
    tty.c_cflag &= ~CRTSCTS;

    // Non-blocking reads (handled via poll)
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    speed_t speed = baudToSpeed(baud_);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        return false;
    }

    // Flush any stale data
    tcflush(fd_, TCIOFLUSH);

    return true;
}

bool SerialPort::open()
{
    if (fd_ >= 0) {
        return true;
    }

    fd_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        return false;
    }

    // Clear O_NONBLOCK after open (we use poll for timeout control)
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);

    if (!configure()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

void SerialPort::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool SerialPort::isOpen() const
{
    return fd_ >= 0;
}

ssize_t SerialPort::read(uint8_t* buf, size_t len, int timeoutMs)
{
    if (fd_ < 0) return -1;

    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeoutMs);
    if (ret < 0) return -1;
    if (ret == 0) return 0;

    return ::read(fd_, buf, len);
}

ssize_t SerialPort::write(const uint8_t* buf, size_t len)
{
    if (fd_ < 0) return -1;
    return ::write(fd_, buf, len);
}

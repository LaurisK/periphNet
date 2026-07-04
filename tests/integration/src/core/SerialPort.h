#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <sys/types.h>

/// RAII POSIX serial port wrapper (termios).
class SerialPort {
public:
    explicit SerialPort(std::string path, int baud = 115200);
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;

    bool open();
    void close();
    bool isOpen() const;
    int fd() const { return fd_; }

    /// Read up to len bytes, blocking up to timeoutMs. Returns bytes read, 0 on timeout, -1 on error.
    ssize_t read(uint8_t* buf, size_t len, int timeoutMs);

    /// Write len bytes. Returns bytes written or -1 on error.
    ssize_t write(const uint8_t* buf, size_t len);

private:
    std::string path_;
    int baud_;
    int fd_ = -1;

    bool configure();
};

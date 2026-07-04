#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <sys/types.h>

/// Manages a named FIFO + trice subprocess for decoding TCOBS trice data.
class TriceProcess {
public:
    /// @param tricePort  "FILE" for FIFO-based, "UDP4" for UDP
    /// @param triceArgs  FIFO path for FILE mode, ":17001" for UDP4
    TriceProcess(std::string triceBinary,
                 std::string tilPath,
                 std::string liPath,
                 std::string tricePort,
                 std::string triceArgs);
    ~TriceProcess();

    TriceProcess(const TriceProcess&) = delete;
    TriceProcess& operator=(const TriceProcess&) = delete;

    /// Start the trice subprocess. For FILE mode, creates the named FIFO first.
    bool start();

    /// Stop subprocess + clean up FIFO.
    void stop();

    bool isRunning() const;

    /// Read one decoded line from trice stdout. Blocks up to timeoutMs.
    bool readLine(std::string& out, int timeoutMs);

    /// Write raw bytes to the FIFO (FILE mode only). Called from serial reader thread.
    bool writeFifo(const uint8_t* data, size_t len);

    /// Get the FIFO path (for FILE mode).
    const std::string& fifoPath() const { return fifoPath_; }

private:
    std::string triceBinary_;
    std::string tilPath_;
    std::string liPath_;
    std::string tricePort_;
    std::string triceArgs_;
    std::string fifoPath_;

    pid_t childPid_ = -1;
    int stdoutReadFd_ = -1;
    int fifoWriteFd_ = -1;
    FILE* stdoutFile_ = nullptr;

    bool createFifo();
    void removeFifo();
};

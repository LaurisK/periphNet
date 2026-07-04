#include "core/TriceProcess.h"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <utility>

TriceProcess::TriceProcess(std::string triceBinary,
                           std::string tilPath,
                           std::string liPath,
                           std::string tricePort,
                           std::string triceArgs)
    : triceBinary_(std::move(triceBinary))
    , tilPath_(std::move(tilPath))
    , liPath_(std::move(liPath))
    , tricePort_(std::move(tricePort))
    , triceArgs_(std::move(triceArgs))
{
}

TriceProcess::~TriceProcess()
{
    stop();
}

bool TriceProcess::createFifo()
{
    fifoPath_ = "/tmp/periphnet_trice_" + std::to_string(getpid()) + ".fifo";
    unlink(fifoPath_.c_str());
    if (mkfifo(fifoPath_.c_str(), 0600) != 0) {
        return false;
    }
    return true;
}

void TriceProcess::removeFifo()
{
    if (!fifoPath_.empty()) {
        unlink(fifoPath_.c_str());
        fifoPath_.clear();
    }
}

bool TriceProcess::start()
{
    if (childPid_ > 0) {
        return true;
    }

    // For FILE mode, create the FIFO and set triceArgs to its path
    bool isFileMode = (tricePort_ == "FILE");
    if (isFileMode) {
        if (!createFifo()) {
            return false;
        }
        triceArgs_ = fifoPath_;
    }

    // Create pipe for capturing trice stdout
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        if (isFileMode) removeFifo();
        return false;
    }

    childPid_ = fork();
    if (childPid_ < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        if (isFileMode) removeFifo();
        return false;
    }

    if (childPid_ == 0) {
        // Child: redirect stdout+stderr to pipe, exec trice
        ::close(pipeFds[0]);
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[1]);

        execlp(triceBinary_.c_str(), "trice",
               "log",
               "-p", tricePort_.c_str(),
               "-args", triceArgs_.c_str(),
               "-i", tilPath_.c_str(),
               "-li", liPath_.c_str(),
               "-color", "off",
               nullptr);
        _exit(127);
    }

    // Parent
    ::close(pipeFds[1]);
    stdoutReadFd_ = pipeFds[0];
    stdoutFile_ = fdopen(stdoutReadFd_, "r");

    // For FILE mode, open the FIFO for writing (trice must open read end first)
    if (isFileMode) {
        // Small delay to let trice start and open the FIFO read end
        usleep(100000); // 100ms

        fifoWriteFd_ = ::open(fifoPath_.c_str(), O_WRONLY);
        if (fifoWriteFd_ < 0) {
            stop();
            return false;
        }
    }

    return true;
}

void TriceProcess::stop()
{
    if (fifoWriteFd_ >= 0) {
        ::close(fifoWriteFd_);
        fifoWriteFd_ = -1;
    }

    if (childPid_ > 0) {
        kill(childPid_, SIGTERM);

        // Wait up to 500ms for graceful exit
        for (int i = 0; i < 50; i++) {
            int status;
            pid_t ret = waitpid(childPid_, &status, WNOHANG);
            if (ret == childPid_ || ret < 0) break;
            usleep(10000);
        }

        // Force kill if still alive
        if (waitpid(childPid_, nullptr, WNOHANG) == 0) {
            kill(childPid_, SIGKILL);
            waitpid(childPid_, nullptr, 0);
        }

        childPid_ = -1;
    }

    if (stdoutFile_) {
        fclose(stdoutFile_);
        stdoutFile_ = nullptr;
        stdoutReadFd_ = -1; // closed by fclose
    } else if (stdoutReadFd_ >= 0) {
        ::close(stdoutReadFd_);
        stdoutReadFd_ = -1;
    }

    removeFifo();
}

bool TriceProcess::isRunning() const
{
    if (childPid_ <= 0) return false;
    int status;
    pid_t ret = waitpid(childPid_, &status, WNOHANG);
    return (ret == 0);
}

bool TriceProcess::readLine(std::string& out, int timeoutMs)
{
    if (stdoutReadFd_ < 0) return false;

    struct pollfd pfd{};
    pfd.fd = stdoutReadFd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeoutMs);
    if (ret <= 0) return false;

    char buf[4096];
    if (!stdoutFile_ || !fgets(buf, sizeof(buf), stdoutFile_)) {
        return false;
    }

    out = buf;
    // Strip trailing newline
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }

    // Strip trice FILE: prefix if present
    const std::string prefix = "  FILE: ";
    if (out.substr(0, prefix.size()) == prefix) {
        out = out.substr(prefix.size());
    }

    return true;
}

bool TriceProcess::writeFifo(const uint8_t* data, size_t len)
{
    if (fifoWriteFd_ < 0 || len == 0) return false;

    ssize_t written = ::write(fifoWriteFd_, data, len);
    return written > 0;
}

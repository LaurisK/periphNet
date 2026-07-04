#pragma once

#include <string>
#include <deque>
#include <vector>
#include <mutex>
#include <cstddef>

/// Thread-safe circular buffer of log lines.
class LogBuffer {
public:
    explicit LogBuffer(size_t maxLines = 10000);

    void append(const std::string& line);
    size_t size() const;

    /// Returns lines from fromIndex onward (for incremental GUI updates).
    std::vector<std::string> linesSince(size_t fromIndex) const;

    /// Returns all lines.
    std::vector<std::string> allLines() const;

    void clear();

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    size_t maxLines_;
    size_t totalAppended_ = 0;
};

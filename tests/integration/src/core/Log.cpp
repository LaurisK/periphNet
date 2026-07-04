#include "core/Log.h"

LogBuffer::LogBuffer(size_t maxLines)
    : maxLines_(maxLines)
{
}

void LogBuffer::append(const std::string& line)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(line);
    totalAppended_++;
    while (lines_.size() > maxLines_) {
        lines_.pop_front();
    }
}

size_t LogBuffer::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.size();
}

std::vector<std::string> LogBuffer::linesSince(size_t fromIndex) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;

    // fromIndex is an absolute index into totalAppended_
    size_t droppedCount = totalAppended_ - lines_.size();
    if (fromIndex < droppedCount) {
        fromIndex = droppedCount;
    }

    size_t startOffset = fromIndex - droppedCount;
    for (size_t i = startOffset; i < lines_.size(); i++) {
        result.push_back(lines_[i]);
    }

    return result;
}

std::vector<std::string> LogBuffer::allLines() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {lines_.begin(), lines_.end()};
}

void LogBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    totalAppended_ = 0;
}

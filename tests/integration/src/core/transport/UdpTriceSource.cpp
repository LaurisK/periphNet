#include "core/transport/UdpTriceSource.h"

#include <utility>

UdpTriceSource::UdpTriceSource(uint16_t port,
                               std::string triceBinary,
                               std::string tilPath,
                               std::string liPath)
    : trice_(std::move(triceBinary), std::move(tilPath), std::move(liPath),
             "UDP4", ":" + std::to_string(port))
{
}

UdpTriceSource::~UdpTriceSource()
{
    stop();
}

bool UdpTriceSource::start()
{
    return trice_.start();
}

void UdpTriceSource::stop()
{
    trice_.stop();
}

bool UdpTriceSource::isRunning() const
{
    return trice_.isRunning();
}

bool UdpTriceSource::readLine(std::string& out, int timeoutMs)
{
    return trice_.readLine(out, timeoutMs);
}

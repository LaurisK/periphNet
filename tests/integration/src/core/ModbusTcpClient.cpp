#include "core/ModbusTcpClient.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

uint16_t get16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

double sinceMs(std::chrono::steady_clock::time_point t0)
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now() - t0).count();
}

} // namespace

ModbusTcpClient::~ModbusTcpClient()
{
    close();
}

bool ModbusTcpClient::connect(const std::string& host, uint16_t port,
                              int timeoutMs, std::string& err)
{
    close();

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        err = std::string("socket: ") + std::strerror(errno);
        return false;
    }

    /* Nagle would coalesce a 12-byte request with nothing and delay it; the
     * far end this emulates disables it for the same reason. */
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        err = "bad address: " + host;
        close();
        return false;
    }

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        err = "connect " + host + ":" + std::to_string(port) + ": " +
              std::strerror(errno);
        close();
        return false;
    }
    return true;
}

void ModbusTcpClient::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool ModbusTcpClient::sendAll(const uint8_t* p, size_t n, std::string& err)
{
    while (n > 0) {
        ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
        if (w <= 0) {
            err = std::string("send: ") + std::strerror(errno);
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

bool ModbusTcpClient::recvExactly(uint8_t* p, size_t n, int timeoutMs,
                                  std::string& err)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);

    while (n > 0) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            err = "timeout";
            return false;
        }
        int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count());

        struct pollfd pfd { fd_, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, left);
        if (pr == 0) {
            err = "timeout";
            return false;
        }
        if (pr < 0) {
            err = std::string("poll: ") + std::strerror(errno);
            return false;
        }

        ssize_t r = ::recv(fd_, p, n, 0);
        if (r == 0) {
            err = "peer closed";
            return false;
        }
        if (r < 0) {
            err = std::string("recv: ") + std::strerror(errno);
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

ModbusTcpClient::Result ModbusTcpClient::transact(const std::vector<uint8_t>& pdu,
                                                  uint8_t unit, int spacingMs,
                                                  uint16_t expectRegs)
{
    Result res;

    if (fd_ < 0) {
        res.detail = "not connected";
        res.protocolError = true;
        return res;
    }

    /* The spacing upstream inserts before every transaction.  It is part of
     * the contract being emulated, not politeness: a client that hammers the
     * board is a different load than the one it was designed for. */
    if (spacingMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(spacingMs));
    }

    const uint16_t txn = nextTxn_++;
    res.txnId = txn;

    std::vector<uint8_t> frame;
    put16(frame, txn);
    put16(frame, 0);                                   // protocol id
    put16(frame, static_cast<uint16_t>(pdu.size() + 1)); // unit + PDU
    frame.push_back(unit);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    lastReq_ = frame;

    auto t0 = std::chrono::steady_clock::now();

    std::string err;
    if (!sendAll(frame.data(), frame.size(), err)) {
        res.detail = err;
        res.protocolError = true;
        res.elapsedMs = sinceMs(t0);
        return res;
    }

    uint8_t hdr[7];
    if (!recvExactly(hdr, sizeof(hdr), kTimeoutMs, err)) {
        res.elapsedMs = sinceMs(t0);
        res.timedOut  = (err == "timeout");
        res.protocolError = !res.timedOut;
        res.detail = err;
        return res;
    }

    if (get16(&hdr[0]) != txn) {
        res.elapsedMs = sinceMs(t0);
        res.protocolError = true;
        res.detail = "transaction id " + std::to_string(get16(&hdr[0])) +
                     " does not match request " + std::to_string(txn);
        return res;
    }
    if (get16(&hdr[2]) != 0) {
        res.elapsedMs = sinceMs(t0);
        res.protocolError = true;
        res.detail = "protocol id " + std::to_string(get16(&hdr[2])) + " != 0";
        return res;
    }

    const uint16_t length = get16(&hdr[4]);
    if (length < 2 || length > 254) {
        res.elapsedMs = sinceMs(t0);
        res.protocolError = true;
        res.detail = "MBAP length " + std::to_string(length) + " out of range";
        return res;
    }
    if (hdr[6] != unit) {
        res.elapsedMs = sinceMs(t0);
        res.protocolError = true;
        res.detail = "unit id " + std::to_string(hdr[6]) + " != " +
                     std::to_string(unit);
        return res;
    }

    std::vector<uint8_t> body(static_cast<size_t>(length) - 1);
    if (!recvExactly(body.data(), body.size(), kTimeoutMs, err)) {
        res.elapsedMs = sinceMs(t0);
        res.timedOut  = (err == "timeout");
        res.protocolError = !res.timedOut;
        res.detail = "body: " + err;
        return res;
    }
    res.elapsedMs = sinceMs(t0);

    const uint8_t fc = body[0];

    /* An exception PDU is a valid response.  pymodbus spends no retry on one
     * and the 5 s timeout never starts — which is exactly why the gateway
     * answers 0x06 instead of going quiet. */
    if (fc & 0x80) {
        if (body.size() < 2) {
            res.protocolError = true;
            res.detail = "exception response with no code";
            return res;
        }
        res.exceptionCode = body[1];
        res.detail = "exception 0x" +
                     std::string(1, "0123456789ABCDEF"[(body[1] >> 4) & 0xF]) +
                     std::string(1, "0123456789ABCDEF"[body[1] & 0xF]);
        return res;
    }

    if (expectRegs > 0) {
        if (body.size() < 2 || body[1] != expectRegs * 2 ||
            body.size() != static_cast<size_t>(2 + expectRegs * 2)) {
            res.protocolError = true;
            res.detail = "expected " + std::to_string(expectRegs) +
                         " registers, byte count " +
                         std::to_string(body.size() >= 2 ? body[1] : 0) +
                         " in a " + std::to_string(body.size()) + "-byte body";
            return res;
        }
        res.registers.reserve(expectRegs);
        for (uint16_t i = 0; i < expectRegs; i++) {
            res.registers.push_back(get16(&body[2 + i * 2]));
        }
    }

    res.ok = true;
    return res;
}

ModbusTcpClient::Result ModbusTcpClient::readRegisters(uint8_t unit, uint8_t fc,
                                                       uint16_t addr,
                                                       uint16_t count)
{
    std::vector<uint8_t> pdu{ fc };
    put16(pdu, addr);
    put16(pdu, count);
    return transact(pdu, unit, kReadSpacing, count);
}

ModbusTcpClient::Result ModbusTcpClient::writeSingle(uint8_t unit, uint16_t addr,
                                                     uint16_t value)
{
    std::vector<uint8_t> pdu{ 0x06 };
    put16(pdu, addr);
    put16(pdu, value);
    return transact(pdu, unit, kWriteSpacing, 0);
}

ModbusTcpClient::Result ModbusTcpClient::writeMultiple(
    uint8_t unit, uint16_t addr, const std::vector<uint16_t>& values)
{
    std::vector<uint8_t> pdu{ 0x10 };
    put16(pdu, addr);
    put16(pdu, static_cast<uint16_t>(values.size()));
    pdu.push_back(static_cast<uint8_t>(values.size() * 2));
    for (uint16_t v : values) {
        put16(pdu, v);
    }
    return transact(pdu, unit, kWriteSpacing, 0);
}

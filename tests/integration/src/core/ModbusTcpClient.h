#pragma once

/**
 * A Modbus TCP client that behaves the way `solis_modbus` behaves.
 *
 * The point is not to be a good Modbus client — it is to be THE SAME client
 * the board will actually face, because the gateway's whole design is shaped
 * by that far end's habits (docs/design_solis_modbus_link.md §3.3):
 *
 *   - ONE TRANSACTION AT A TIME.  `client_manager.py` holds one asyncio lock
 *     per host:port across every slave and every write, so the board never
 *     sees a second outstanding request and a single slow answer
 *     head-of-line-blocks everything.
 *   - Inter-frame spacing: 50 ms before a read, 100 ms before a write.
 *   - A 5 s pymodbus timeout with ONE retry.  Reaching that timeout is the
 *     failure the gateway's 1500 ms budget and its 0x06 answer exist to
 *     prevent, so this client reports elapsed time per transaction and the
 *     tests assert on it.
 *
 * AN EXCEPTION IS A RESULT, NOT AN ERROR, and the split matters: upstream
 * bisects a failing block on exception 2 or 3 and merely skips the group on
 * anything else, while a TIMEOUT costs a retry and then five seconds of the
 * whole integration. So `Result` distinguishes them.
 */

#include <cstdint>
#include <string>
#include <vector>

class ModbusTcpClient {
public:
    struct Result {
        bool     ok            = false;  ///< a data response arrived
        bool     timedOut      = false;  ///< nothing arrived in time
        bool     protocolError = false;  ///< a reply that is not well-formed
        uint8_t  exceptionCode = 0;      ///< non-zero: the far end said no
        double   elapsedMs     = 0.0;
        uint16_t txnId         = 0;
        std::string detail;              ///< why, when something went wrong
        std::vector<uint16_t> registers; ///< FC03/FC04 payload
    };

    ModbusTcpClient() = default;
    ~ModbusTcpClient();

    ModbusTcpClient(const ModbusTcpClient&) = delete;
    ModbusTcpClient& operator=(const ModbusTcpClient&) = delete;

    /// @return true when the TCP session is up. `err` says why not.
    bool connect(const std::string& host, uint16_t port, int timeoutMs,
                 std::string& err);
    void close();
    bool connected() const { return fd_ >= 0; }

    Result readRegisters(uint8_t unit, uint8_t fc, uint16_t addr,
                         uint16_t count);
    Result writeSingle(uint8_t unit, uint16_t addr, uint16_t value);
    Result writeMultiple(uint8_t unit, uint16_t addr,
                         const std::vector<uint16_t>& values);

    /// Bytes of the last request, for a test that needs to assert on framing.
    const std::vector<uint8_t>& lastRequest() const { return lastReq_; }

    /// pymodbus's own numbers, so a test that waits is waiting like HA does.
    static constexpr int  kTimeoutMs    = 5000;
    static constexpr int  kReadSpacing  = 50;
    static constexpr int  kWriteSpacing = 100;

private:
    Result transact(const std::vector<uint8_t>& pdu, uint8_t unit,
                    int spacingMs, uint16_t expectRegs);
    bool   sendAll(const uint8_t* p, size_t n, std::string& err);
    bool   recvExactly(uint8_t* p, size_t n, int timeoutMs, std::string& err);

    int      fd_    = -1;
    uint16_t nextTxn_ = 1;
    std::vector<uint8_t> lastReq_;
};

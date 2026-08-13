#pragma once

#include <cstdint>
#include <string>

/// Minimal blocking HTTP/1.1 client over a raw TCP socket.
///
/// It exists for ONE job: provisioning a board with a Modbus config before the
/// Modbus tests run.  Since docs/modbus.md §4.2 there is no built-in default
/// config — "the board is told what it is for; until then it is not for
/// anything" — so a test fixture has to upload one, and upload is an HTTP
/// operation.  The CLI channel cannot carry it: `modbus` has no command that
/// takes a config, deliberately.
///
/// Not a general HTTP library: no chunked encoding, no redirects, no keep-alive.
/// The board's server closes the connection per request, which is exactly what
/// this reads until.
class HttpClient {
public:
    struct Response {
        bool        ok = false;      ///< transport succeeded (not the status)
        int         status = 0;      ///< HTTP status code, 0 if unparsed
        std::string body;
        std::string error;           ///< transport-level failure, if any
    };

    HttpClient(std::string host, uint16_t port = 80, int timeoutMs = 10000);

    Response get(const std::string& path);
    Response post(const std::string& path, const std::string& body,
                  const std::string& contentType = "application/json");
    Response del(const std::string& path);

    const std::string& host() const { return host_; }

private:
    Response request(const std::string& method, const std::string& path,
                     const std::string& body, const std::string& contentType);

    std::string host_;
    uint16_t    port_;
    int         timeoutMs_;
};

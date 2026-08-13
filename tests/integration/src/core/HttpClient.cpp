#include "core/HttpClient.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

HttpClient::HttpClient(std::string host, uint16_t port, int timeoutMs)
    : host_(std::move(host)), port_(port), timeoutMs_(timeoutMs)
{
}

HttpClient::Response HttpClient::get(const std::string& path)
{
    return request("GET", path, "", "");
}

HttpClient::Response HttpClient::post(const std::string& path,
                                      const std::string& body,
                                      const std::string& contentType)
{
    return request("POST", path, body, contentType);
}

HttpClient::Response HttpClient::del(const std::string& path)
{
    return request("DELETE", path, "", "");
}

HttpClient::Response HttpClient::request(const std::string& method,
                                         const std::string& path,
                                         const std::string& body,
                                         const std::string& contentType)
{
    Response resp;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        resp.error = std::string("socket: ") + std::strerror(errno);
        return resp;
    }

    struct timeval tv;
    tv.tv_sec  = timeoutMs_ / 1000;
    tv.tv_usec = (timeoutMs_ % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        resp.error = "bad IPv4 address: " + host_;
        ::close(fd);
        return resp;
    }

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        resp.error = "connect " + host_ + ": " + std::strerror(errno);
        ::close(fd);
        return resp;
    }

    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: " + host_ + "\r\n"
                      "Connection: close\r\n";
    if (!body.empty()) {
        req += "Content-Type: " + contentType + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) {
            resp.error = std::string("send: ") + std::strerror(errno);
            ::close(fd);
            return resp;
        }
        sent += static_cast<size_t>(n);
    }

    /* The server closes after each response, so "read to EOF" IS the framing. */
    std::string raw;
    char        buf[2048];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            raw.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;                       /* clean close */
        }
        if (errno == EINTR) {
            continue;
        }
        if (raw.empty()) {
            resp.error = std::string("recv: ") + std::strerror(errno);
            ::close(fd);
            return resp;
        }
        break;                           /* timeout with data: take what we got */
    }
    ::close(fd);

    if (raw.rfind("HTTP/1.", 0) != 0) {
        resp.error = "not an HTTP response";
        return resp;
    }
    size_t sp = raw.find(' ');
    if (sp != std::string::npos) {
        resp.status = std::atoi(raw.c_str() + sp + 1);
    }
    size_t hdrEnd = raw.find("\r\n\r\n");
    resp.body = (hdrEnd == std::string::npos) ? "" : raw.substr(hdrEnd + 4);
    resp.ok   = true;
    return resp;
}

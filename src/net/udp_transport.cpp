// UDP transport: IP:port addressing for the same PC (tldbot), a LAN, or forwarded ports.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <cstdio>

#include "net/protocol.h"
#include "net/session.h"
#include "net/transport.h"

namespace net {
namespace {

PeerId pack(const sockaddr_in& a) {
    return (static_cast<PeerId>(ntohl(a.sin_addr.s_addr)) << 16) | ntohs(a.sin_port);
}

sockaddr_in unpack(PeerId id) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(static_cast<uint32_t>(id >> 16));
    a.sin_port = htons(static_cast<uint16_t>(id & 0xFFFF));
    return a;
}

bool make_addr(const std::string& ip, uint16_t port, sockaddr_in& out) {
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
}

class UdpTransport final : public Transport {
public:
    ~UdpTransport() override { close(); }

    bool open(const Config& cfg, const LogFn& log) override {
        log_ = log;
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup failed");
        wsa_started_ = true;
        sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) return fail("socket() failed");
        u_long non_blocking = 1;
        ioctlsocket(sock_, FIONBIO, &non_blocking);
        BOOL report_reset = FALSE;  // don't let ICMP "port unreachable" poison recvfrom
        DWORD unused = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), nullptr, 0, &unused, nullptr, nullptr);

        sockaddr_in local{};
        const uint16_t bind_port = cfg.role == Role::Host ? cfg.port : 0;
        if (!make_addr(cfg.bind_ip, bind_port, local)) return fail("bad bind address '" + cfg.bind_ip + "'");
        if (bind(sock_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
            return fail("bind " + cfg.bind_ip + ":" + std::to_string(bind_port) + " failed (port in use?)");
        if (cfg.role == Role::Client) {
            sockaddr_in host{};
            if (!make_addr(cfg.host_ip, cfg.port, host)) return fail("bad host address '" + cfg.host_ip + "'");
            host_ = pack(host);
            status_ = "joining " + describe(host_);
        } else {
            status_ = "hosting on " + cfg.bind_ip + ":" + std::to_string(cfg.port);
        }
        return true;
    }

    bool host_peer(PeerId& out) override {
        if (!host_) return false;
        out = host_;
        return true;
    }

    void send(PeerId to, const uint8_t* data, size_t size) override {
        if (sock_ == INVALID_SOCKET) return;
        const sockaddr_in a = unpack(to);
        sendto(sock_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
               reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    }

    bool receive(PeerId& from, std::vector<uint8_t>& data) override {
        if (sock_ == INVALID_SOCKET) return false;
        uint8_t buf[kMaxPacket];
        for (;;) {
            sockaddr_in a{};
            int len = sizeof(a);
            const int n = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&a), &len);
            if (n >= 0) {
                from = pack(a);
                data.assign(buf, buf + n);
                return true;
            }
            const int err = WSAGetLastError();
            if (err == WSAEMSGSIZE || err == WSAECONNRESET) continue;  // oversized / ICMP noise
            return false;  // WSAEWOULDBLOCK (nothing pending) or a real error
        }
    }

    void wait(int timeout_ms) override {
        if (sock_ == INVALID_SOCKET) {
            Sleep(static_cast<DWORD>(timeout_ms));
            return;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(sock_, &readable);
        timeval tv{0, timeout_ms * 1000};
        select(0, &readable, nullptr, nullptr, &tv);
    }

    void close() override {
        if (sock_ != INVALID_SOCKET) closesocket(sock_);
        sock_ = INVALID_SOCKET;
        if (wsa_started_) WSACleanup();
        wsa_started_ = false;
    }

    std::string describe(PeerId peer) const override {
        const sockaddr_in a = unpack(peer);
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
    }

    std::string status() const override { return status_; }

private:
    bool fail(const std::string& why) {
        status_ = why;
        if (log_) log_(why);
        return false;
    }

    LogFn log_;
    SOCKET sock_ = INVALID_SOCKET;
    bool wsa_started_ = false;
    PeerId host_ = 0;
    std::string status_;
};

}  // namespace

std::unique_ptr<Transport> make_udp_transport() { return std::make_unique<UdpTransport>(); }

}  // namespace net

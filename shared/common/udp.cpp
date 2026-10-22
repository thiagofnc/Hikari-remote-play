#include "hikari/udp.h"

#include <mstcpip.h>
#include <qos2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "hikari/log.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace hikari {

std::string NetAddress::to_string() const
{
    char host[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;
    if (storage.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
            in_addr v4;
            std::memcpy(&v4, &a->sin6_addr.s6_addr[12], 4);
            inet_ntop(AF_INET, &v4, host, sizeof(host));
            port = ntohs(a->sin6_port);
            return std::string(host) + ":" + std::to_string(port);
        }
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
        port = ntohs(a->sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }
    const auto* a = reinterpret_cast<const sockaddr_in*>(&storage);
    inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
    port = ntohs(a->sin_port);
    return std::string(host) + ":" + std::to_string(port);
}

bool NetAddress::operator==(const NetAddress& o) const
{
    return len == o.len && std::memcmp(&storage, &o.storage, static_cast<size_t>(len)) == 0;
}

bool NetAddress::resolve(const std::string& input, uint16_t default_port, NetAddress& out)
{
    std::string host = input;
    std::string port = std::to_string(default_port);
    if (!host.empty() && host.front() == '[') {
        const auto close = host.find(']');
        if (close == std::string::npos) return false;
        if (close + 2 < host.size() && host[close + 1] == ':') port = host.substr(close + 2);
        host = host.substr(1, close - 1);
    } else if (std::count(host.begin(), host.end(), ':') == 1) {
        const auto c = host.find(':');
        port = host.substr(c + 1);
        host = host.substr(0, c);
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    // Prefer IPv4 (Tailscale 100.x addresses), map into our dual-stack socket.
    const addrinfo* pick = res;
    for (auto* p = res; p; p = p->ai_next)
        if (p->ai_family == AF_INET) {
            pick = p;
            break;
        }
    out = {};
    if (pick->ai_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(pick->ai_addr);
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&out.storage);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = v4->sin_port;
        v6->sin6_addr.s6_addr[10] = 0xFF;
        v6->sin6_addr.s6_addr[11] = 0xFF;
        std::memcpy(&v6->sin6_addr.s6_addr[12], &v4->sin_addr, 4);
        out.len = sizeof(sockaddr_in6);
    } else {
        std::memcpy(&out.storage, pick->ai_addr, pick->ai_addrlen);
        out.len = static_cast<int>(pick->ai_addrlen);
    }
    freeaddrinfo(res);
    return true;
}

NetAddress NetAddress::broadcast(uint16_t port)
{
    NetAddress a;
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&a.storage);
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    v6->sin6_addr.s6_addr[10] = 0xFF;
    v6->sin6_addr.s6_addr[11] = 0xFF;
    std::memset(&v6->sin6_addr.s6_addr[12], 0xFF, 4);
    a.len = sizeof(sockaddr_in6);
    return a;
}

bool UdpSocket::startup()
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    });
    return ok;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint16_t bind_port, bool allow_broadcast)
{
    startup();
    close();
    sock_ = WSASocketW(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (sock_ == INVALID_SOCKET) {
        HK_LOGE("socket() failed: %d", WSAGetLastError());
        return false;
    }
    DWORD off = 0;
    setsockopt(sock_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    BOOL no_reset = FALSE;
    DWORD ret = 0;
    WSAIoctl(sock_, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), nullptr, 0, &ret, nullptr, nullptr);
    int buf = 8 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    if (allow_broadcast) {
        BOOL on = TRUE;
        setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
    }
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(bind_port);
    addr.sin6_addr = in6addr_any;
    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        HK_LOGE("bind(%u) failed: %d", bind_port, WSAGetLastError());
        close();
        return false;
    }
    return true;
}

void UdpSocket::close()
{
    if (qos_) {
        QOSCloseHandle(static_cast<HANDLE>(qos_));
        qos_ = nullptr;
        qos_flow_ = 0;
    }
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

bool UdpSocket::send_to(const NetAddress& to, const void* data, size_t len)
{
    const int r = sendto(sock_, static_cast<const char*>(data), static_cast<int>(len), 0,
                         reinterpret_cast<const sockaddr*>(&to.storage), to.len);
    if (r < 0) {
        ++send_errors_;
        return false;
    }
    return true;
}

int UdpSocket::recv_from(void* buf, size_t cap, NetAddress& from, int timeout_ms)
{
    if (timeout_ms >= 0) {
        WSAPOLLFD p{};
        p.fd = sock_;
        p.events = POLLRDNORM;
        const int n = WSAPoll(&p, 1, timeout_ms);
        if (n <= 0) return -1;
    }
    from.len = sizeof(from.storage);
    const int r = recvfrom(sock_, static_cast<char*>(buf), static_cast<int>(cap), 0,
                           reinterpret_cast<sockaddr*>(&from.storage), &from.len);
    if (r < 0) {
        from.len = 0;
        return -1;
    }
    return r;
}

void UdpSocket::set_traffic_class(const NetAddress& to, TrafficClass tc)
{
    if (!qos_) {
        QOS_VERSION v{1, 0};
        HANDLE h = nullptr;
        if (!QOSCreateHandle(&v, &h)) return;
        qos_ = h;
    }
    if (qos_flow_) {
        QOSRemoveSocketFromFlow(static_cast<HANDLE>(qos_), sock_, qos_flow_, 0);
        qos_flow_ = 0;
    }
    const QOS_TRAFFIC_TYPE type = tc == TrafficClass::Control ? QOSTrafficTypeControl
                                  : tc == TrafficClass::Audio ? QOSTrafficTypeVoice
                                                              : QOSTrafficTypeAudioVideo;
    QOS_FLOWID flow = 0;
    if (!QOSAddSocketToFlow(static_cast<HANDLE>(qos_), sock_,
                            const_cast<sockaddr*>(reinterpret_cast<const sockaddr*>(&to.storage)), type,
                            QOS_NON_ADAPTIVE_FLOW, &flow)) {
        HK_LOGD("qWAVE flow setup failed (%lu), continuing without DSCP", GetLastError());
        return;
    }
    qos_flow_ = flow;
}

uint16_t UdpSocket::local_port() const
{
    sockaddr_in6 a{};
    int len = sizeof(a);
    if (getsockname(sock_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin6_port);
}

}  // namespace hikari

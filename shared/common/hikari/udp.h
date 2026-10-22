#pragma once
// Thin Winsock UDP wrapper tuned for real-time media.
//  * dual-stack IPv6 socket (works with Tailscale IPv4 and IPv6 addresses)
//  * ICMP "port unreachable" no longer aborts recv (SIO_UDP_CONNRESET off)
//  * large kernel buffers so a keyframe burst is not dropped locally
//  * optional DSCP tagging via qWAVE (EF for input, AF41 for media) so WMM
//    Wi-Fi and QoS-aware routers prioritise controller packets

#include <cstddef>
#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace hikari {

struct NetAddress {
    sockaddr_storage storage{};
    int len = 0;

    bool valid() const { return len > 0; }
    std::string to_string() const;
    bool operator==(const NetAddress& o) const;
    bool operator!=(const NetAddress& o) const { return !(*this == o); }

    // Resolves "host", "host:port", "[v6]:port". Blocking (DNS).
    static bool resolve(const std::string& host, uint16_t default_port, NetAddress& out);
    static NetAddress broadcast(uint16_t port);
};

enum class TrafficClass { Control, Video, Audio };

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    static bool startup();  // WSAStartup once

    bool open(uint16_t bind_port, bool allow_broadcast = false);
    void close();
    bool is_open() const { return sock_ != INVALID_SOCKET; }

    // Returns false on hard error; a full send buffer is reported as false
    // and counted (we never block the caller on the network).
    bool send_to(const NetAddress& to, const void* data, size_t len);
    // Waits up to timeout_ms (0 = poll, -1 = forever). Returns bytes or -1 on
    // timeout/error.
    int recv_from(void* buf, size_t cap, NetAddress& from, int timeout_ms);

    // Tags traffic towards `to` with a DSCP class (best effort, needs qWAVE).
    void set_traffic_class(const NetAddress& to, TrafficClass tc);
    uint16_t local_port() const;
    uint64_t send_errors() const { return send_errors_; }
    SOCKET native() const { return sock_; }

private:
    SOCKET sock_ = INVALID_SOCKET;
    void* qos_ = nullptr;
    unsigned long qos_flow_ = 0;
    uint64_t send_errors_ = 0;
};

}  // namespace hikari

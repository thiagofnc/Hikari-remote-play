#include "host_probe.h"

#include <cstdio>
#include <cstring>

#include "hikari/clock.h"
#include "hikari/crypto.h"
#include "hikari/net_protocol.h"
#include "hikari/system.h"
#include "hikari/udp.h"

namespace hikari {

using proto::PacketType;

ProbeOutcome probe_host(const std::vector<std::string>& candidates, const std::string& pairing_code,
                        const std::atomic<bool>* cancel, int timeout_ms)
{
    ProbeOutcome out;
    struct Target {
        std::string name;
        NetAddress addr;
        uint8_t host_nonce[proto::kNonceSize]{};
        bool challenged = false;
    };
    std::vector<Target> targets;
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        Target t;
        t.name = c;
        if (NetAddress::resolve(c, proto::kDefaultPort, t.addr)) targets.push_back(t);
    }
    if (targets.empty()) {
        out.result = ProbeResult::CannotResolve;
        return out;
    }

    UdpSocket sock;
    if (!sock.open(0)) return out;
    const crypto::Digest psk = crypto::psk_from_code(pairing_code);
    uint8_t nonce[proto::kNonceSize];
    crypto::random_bytes(nonce, sizeof(nonce));

    const uint64_t deadline = now_us() + uint64_t(timeout_ms) * 1000;
    uint64_t next_send = 0;
    bool rejected_version = false;
    while (now_us() < deadline) {
        if (cancel && cancel->load()) {
            out.result = ProbeResult::Cancelled;
            return out;
        }
        if (now_us() >= next_send) {
            next_send = now_us() + 300'000;
            for (auto& t : targets) {
                uint8_t buf[256];
                ByteWriter w(buf, sizeof(buf));
                if (!t.challenged) {
                    proto::Hello h;
                    std::memcpy(h.client_nonce, nonce, sizeof(nonce));
                    std::snprintf(h.client_name, sizeof(h.client_name), "%s", computer_name().c_str());
                    h.flags = proto::kHelloProbe;
                    proto::write_header(w, PacketType::Hello, 0);
                    proto::write(w, h);
                } else {
                    proto::Auth a;
                    std::memcpy(a.client_nonce, nonce, sizeof(nonce));
                    std::memcpy(a.host_nonce, t.host_nonce, sizeof(t.host_nonce));
                    const auto mac = crypto::auth_mac(psk, nonce, t.host_nonce);
                    std::memcpy(a.mac, mac.data(), mac.size());
                    proto::write_header(w, PacketType::Auth, 0);
                    proto::write(w, a);
                }
                sock.send_to(t.addr, buf, w.size());
            }
        }
        uint8_t buf[2048];
        NetAddress from;
        const int n = sock.recv_from(buf, sizeof(buf), from, 50);
        if (n <= 0) continue;
        ByteReader r(buf, static_cast<size_t>(n));
        proto::Header h;
        if (!proto::read_header(r, h)) continue;
        Target* t = nullptr;
        for (auto& c : targets)
            if (c.addr == from) t = &c;
        if (!t) continue;

        if (h.type == PacketType::Challenge) {
            proto::Challenge c;
            if (!proto::read(r, c) || t->challenged) continue;
            std::memcpy(t->host_nonce, c.host_nonce, sizeof(c.host_nonce));
            t->challenged = true;
            next_send = 0;  // answer right away
        } else if (h.type == PacketType::Reject) {
            proto::Reject rj;
            if (!proto::read(r, rj)) continue;
            if (rj.reason == proto::RejectReason::BadVersion) {
                rejected_version = true;
                continue;  // another candidate might be a matching host
            }
            out.result = ProbeResult::WrongCode;
            out.address = t->name;
            return out;
        } else if (h.type == PacketType::Welcome && t->challenged) {
            proto::Welcome wel;
            if (!proto::read(r, wel)) continue;
            const auto want = crypto::welcome_mac(psk, t->host_nonce, nonce);
            if (!crypto::constant_time_equal(want.data(), wel.mac, proto::kMacSize)) continue;
            out.result = ProbeResult::Ok;
            out.host_name = wel.host_name;
            out.address = t->name;
            return out;
        }
    }
    out.result = rejected_version ? ProbeResult::VersionMismatch : ProbeResult::NoAnswer;
    return out;
}

}  // namespace hikari

#include "host_net.h"

#include <algorithm>
#include <cstring>

#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/secure_packet.h"
#include "hikari/system.h"
#include "hikari/tailscale.h"
#include "hk_link.h"

namespace hikari {

using proto::PacketType;

namespace {
constexpr uint64_t kSessionTimeoutUs = 6'000'000;
constexpr uint64_t kHandshakeTimeoutUs = 5'000'000;
}  // namespace

HostNet::HostNet() : input_owd_(3'000'000), capture_to_sent_(2'000'000) {}

HostNet::~HostNet() { stop(); }

void HostNet::set_psk(const crypto::Digest& psk)
{
    std::lock_guard<std::mutex> lk(mu_);
    psk_ = psk;
}

void HostNet::set_tailscale_address(const std::string& ip)
{
    std::lock_guard<std::mutex> lk(mu_);
    tailscale_ip_ = is_tailscale_ipv4(ip) ? ip : "";
}

bool HostNet::start(uint16_t port, const crypto::Digest& psk, const std::string& host_name, const uint8_t host_id[8],
                    HostNetEvents events)
{
    stop();
    psk_ = psk;
    host_name_ = host_name;
    std::memcpy(host_id_, host_id, 8);
    ev_ = std::move(events);
    if (!sock_.open(port, true)) return false;
    running_ = true;
    rx_thread_ = std::thread(&HostNet::rx_loop, this);
    tx_thread_ = std::thread(&HostNet::tx_loop, this);
    HK_LOGI("Listening on UDP %u", port);
    return true;
}

void HostNet::stop()
{
    if (!running_.exchange(false)) return;
    if (connected_) {
        send_control(PacketType::Bye, [](ByteWriter&) {});
    }
    if (rx_thread_.joinable()) rx_thread_.join();
    if (tx_thread_.joinable()) tx_thread_.join();
    sock_.close();
    std::lock_guard<std::mutex> lk(mu_);
    session_.reset();
    connected_ = false;
}

bool HostNet::session_snapshot(uint32_t& id, NetAddress& addr)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!session_) return false;
    id = session_->id;
    addr = session_->addr;
    return true;
}

void HostNet::send_control(PacketType type, const std::function<void(ByteWriter&)>& body)
{
    uint8_t buf[proto::kMaxDatagram];
    NetAddress addr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!session_) return;
        ByteWriter w(buf, sizeof(buf));
        proto::write_header(w, type, session_->id);
        body(w);
        if (!seal(w, session_->key)) return;
        addr = session_->addr;
        sock_.send_to(addr, buf, w.size());
    }
}

void HostNet::set_stream_config(const proto::StreamConfig& cfg)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        config_ = cfg;
    }
    send_control(PacketType::StreamConfig, [&](ByteWriter& w) { proto::write(w, cfg); });
}

void HostNet::send_status(const proto::HostStatus& s)
{
    send_control(PacketType::HostStatus, [&](ByteWriter& w) { proto::write(w, s); });
}

void HostNet::send_stats(const proto::HostStats& s)
{
    send_control(PacketType::HostStats, [&](ByteWriter& w) { proto::write(w, s); });
}

void HostNet::set_pacing(double spread_k, uint32_t bitrate_kbps, uint16_t fps)
{
    // Rate at which one average frame takes 1/k of a frame interval. Floor at
    // 50 Mbps so small frames never wait; ceiling well under typical LAN rates.
    const double bps = std::clamp(double(bitrate_kbps) * 1000.0 * spread_k * (fps / 60.0), 50e6, 900e6);
    pace_bps_.store(bps);
}

void HostNet::disconnect_client() { end_session("host request"); }

void HostNet::end_session(const char* why)
{
    bool was = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (session_) {
            HK_LOGI("Client %s disconnected (%s)", session_->name.c_str(), why);
            was = true;
        }
    }
    if (was) send_control(PacketType::Bye, [](ByteWriter&) {});
    {
        std::lock_guard<std::mutex> lk(mu_);
        session_.reset();
    }
    connected_ = false;
    if (was && ev_.disconnected) ev_.disconnected();
}

void HostNet::send_video(OutgoingFrame&& f)
{
    if (!connected_) return;
    auto p = std::make_unique<OutgoingFrame>(std::move(f));
    if (!tx_queue_.push(std::move(p))) {
        // Sender saturated: everything queued is now late. Drop it and ask
        // the encoder for a fresh keyframe instead of building a backlog.
        tx_queue_.clear([&](std::unique_ptr<OutgoingFrame>&&) { ++frames_dropped_; });
        if (ev_.sender_overflow) ev_.sender_overflow();
    }
}

void HostNet::send_audio(const proto::AudioPacket& meta, const uint8_t* data, size_t len)
{
    uint32_t id;
    NetAddress addr;
    if (!session_snapshot(id, addr)) return;
    uint8_t buf[proto::kMaxDatagram];
    ByteWriter w(buf, sizeof(buf));
    proto::write_header(w, PacketType::Audio, id);
    proto::write(w, meta);
    w.bytes(data, len);
    if (w.ok()) sock_.send_to(addr, buf, w.size());
}

HostNetStats HostNet::stats()
{
    const uint64_t now = now_us();
    HostNetStats s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (session_) {
            s.client_connected = true;
            s.client_name = session_->name;
            s.client_address = session_->addr.to_string();
        }
    }
    s.input_owd = input_owd_.summary(now);
    s.capture_to_sent = capture_to_sent_.summary(now);
    s.input_rate = input_rate_.sample(now);
    s.video_mbps = video_bytes_.sample(now) * 8 / 1e6;
    s.frames_sent = frames_sent_.load();
    s.frames_dropped_sender = frames_dropped_.load();
    s.send_errors = sock_.send_errors();
    return s;
}

void HostNet::rx_loop()
{
    set_thread_role(ThreadRole::Input, "hikari-net-rx");
    uint8_t buf[2048];
    while (running_) {
        NetAddress from;
        const int n = sock_.recv_from(buf, sizeof(buf), from, 50);
        const uint64_t now = now_us();
        if (n > 0) handle(buf, static_cast<size_t>(n), from, now);

        bool timed_out = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (session_ && now - session_->last_rx_us > kSessionTimeoutUs) timed_out = true;
            for (auto it = pending_.begin(); it != pending_.end();)
                it = now - it->second.at_us > kHandshakeTimeoutUs ? pending_.erase(it) : std::next(it);
        }
        if (timed_out) end_session("timeout");
    }
}

void HostNet::handle(const uint8_t* buf, size_t len, const NetAddress& from, uint64_t now)
{
    ByteReader r(buf, len);
    proto::Header h;
    if (!proto::read_header(r, h)) return;

    switch (h.type) {
    case PacketType::Discover: {
        if (now - last_discover_reply_us_ < 50'000) return;  // cheap flood guard
        last_discover_reply_us_ = now;
        proto::DiscoverReply rep;
        std::snprintf(rep.host_name, sizeof(rep.host_name), "%s", host_name_.c_str());
        rep.port = sock_.local_port();
        std::memcpy(rep.host_id, host_id_, 8);
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::snprintf(rep.tailscale_ipv4, sizeof(rep.tailscale_ipv4), "%s", tailscale_ip_.c_str());
        }
        uint8_t out[128];
        ByteWriter w(out, sizeof(out));
        proto::write_header(w, PacketType::DiscoverReply, 0);
        proto::write(w, rep);
        sock_.send_to(from, out, w.size());
        return;
    }
    case PacketType::Hello: handle_hello(r, from, now); return;
    case PacketType::Auth: handle_auth(r, from, now); return;
    default: break;
    }
    if (is_media(h.type)) return;

    // Authenticated session traffic.
    size_t body = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!session_ || h.session != session_->id) return;
        body = open_sealed(buf, len, session_->key);
        if (!body) return;
        session_->last_rx_us = now;
        if (from != session_->addr) {
            HK_LOGI("Client moved %s -> %s", session_->addr.to_string().c_str(), from.to_string().c_str());
            session_->addr = from;
        }
        if (h.type == PacketType::Input) {
            ByteReader ir(buf, body);
            proto::Header ih;
            proto::read_header(ir, ih);
            proto::Input in;
            if (!proto::read(ir, in)) return;
            // Newest state wins: anything not newer than what we forwarded is stale.
            if (session_->have_input && !hk_seq_newer(in.seq, session_->last_input_seq)) return;
            session_->have_input = true;
            session_->last_input_seq = in.seq;
        }
    }

    ByteReader b(buf, body);
    proto::read_header(b, h);
    switch (h.type) {
    case PacketType::Input: {
        proto::Input in;
        proto::read(b, in);
        if ((in.flags & proto::kInputHostClock) && in.client_time_us <= now && now - in.client_time_us < 2'000'000)
            input_owd_.record(now - in.client_time_us, now);
        input_rate_.add();
        if (ev_.input) ev_.input(in, now);
        break;
    }
    case PacketType::Ping: {
        proto::Ping p;
        if (!proto::read(b, p)) break;
        proto::Pong pong{p.seq, p.sender_us, now_us()};
        send_control(PacketType::Pong, [&](ByteWriter& w) { proto::write(w, pong); });
        break;
    }
    case PacketType::ReceiverReport: {
        proto::ReceiverReport rr;
        if (proto::read(b, rr) && ev_.receiver_report) ev_.receiver_report(rr);
        break;
    }
    case PacketType::KeyframeRequest: {
        proto::KeyframeRequest k;
        if (proto::read(b, k) && ev_.keyframe_request) ev_.keyframe_request(k.reason);
        break;
    }
    case PacketType::Play: {
        proto::Play p;
        if (proto::read(b, p) && ev_.play) ev_.play(p);
        break;
    }
    case PacketType::Stop:
        if (ev_.stop) ev_.stop();
        break;
    case PacketType::Power: {
        proto::Power p;
        if (proto::read(b, p) && ev_.power) ev_.power(p);
        break;
    }
    case PacketType::SetPreset: {
        proto::SetPreset p;
        if (proto::read(b, p) && ev_.preset) ev_.preset(p);
        break;
    }
    case PacketType::Bye:
        end_session("client left");
        break;
    default:
        break;
    }
}

void HostNet::handle_hello(ByteReader& r, const NetAddress& from, uint64_t now)
{
    proto::Hello hello;
    if (!proto::read(r, hello)) return;
    uint8_t out[256];
    if (hello.version != proto::kVersion) {
        ByteWriter w(out, sizeof(out));
        proto::write_header(w, PacketType::Reject, 0);
        proto::write(w, proto::Reject{proto::RejectReason::BadVersion});
        sock_.send_to(from, out, w.size());
        return;
    }
    proto::Challenge ch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const std::string key = from.to_string();
        auto it = pending_.find(key);
        // Retransmitted Hello: answer with the same nonce so the exchange converges.
        if (it != pending_.end() &&
            std::memcmp(it->second.hello.client_nonce, hello.client_nonce, proto::kNonceSize) == 0) {
            std::memcpy(ch.host_nonce, it->second.host_nonce, proto::kNonceSize);
        } else {
            if (pending_.size() > 32) pending_.clear();
            PendingHandshake p;
            p.hello = hello;
            crypto::random_bytes(p.host_nonce, proto::kNonceSize);
            p.at_us = now;
            std::memcpy(ch.host_nonce, p.host_nonce, proto::kNonceSize);
            pending_[key] = p;
        }
    }
    ByteWriter w(out, sizeof(out));
    proto::write_header(w, PacketType::Challenge, 0);
    proto::write(w, ch);
    sock_.send_to(from, out, w.size());
}

void HostNet::handle_auth(ByteReader& r, const NetAddress& from, uint64_t now)
{
    proto::Auth a;
    if (!proto::read(r, a)) return;
    PendingHandshake p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_.find(from.to_string());
        if (it == pending_.end()) return;
        p = it->second;
        if (std::memcmp(p.hello.client_nonce, a.client_nonce, proto::kNonceSize) != 0 ||
            std::memcmp(p.host_nonce, a.host_nonce, proto::kNonceSize) != 0)
            return;
        pending_.erase(it);
    }
    crypto::Digest psk;
    {
        std::lock_guard<std::mutex> lk(mu_);
        psk = psk_;
    }
    const crypto::Digest want = crypto::auth_mac(psk, a.client_nonce, a.host_nonce);
    uint8_t out[512];
    if (!crypto::constant_time_equal(want.data(), a.mac, proto::kMacSize)) {
        HK_LOGW("Rejected client %s: wrong pairing code", from.to_string().c_str());
        ByteWriter w(out, sizeof(out));
        proto::write_header(w, PacketType::Reject, 0);
        proto::write(w, proto::Reject{proto::RejectReason::BadAuth});
        sock_.send_to(from, out, w.size());
        return;
    }

    proto::Welcome wel;
    const crypto::Digest wm = crypto::welcome_mac(psk, a.host_nonce, a.client_nonce);
    std::memcpy(wel.mac, wm.data(), proto::kMacSize);
    std::snprintf(wel.host_name, sizeof(wel.host_name), "%s", host_name_.c_str());

    if (p.hello.flags & proto::kHelloProbe) {
        // Pairing check from a client's Settings: prove we know the code, but
        // leave whoever is playing undisturbed.
        {
            std::lock_guard<std::mutex> lk(mu_);
            wel.config = config_;
        }
        ByteWriter w(out, sizeof(out));
        proto::write_header(w, PacketType::Welcome, 0);
        proto::write(w, wel);
        sock_.send_to(from, out, w.size());
        HK_LOGI("Pairing verified for \"%s\" (%s)", p.hello.client_name, from.to_string().c_str());
        return;
    }

    auto s = std::make_unique<Session>();
    do {
        s->id = crypto::random_u32();
    } while (s->id == 0);
    s->addr = from;
    const crypto::Digest key = crypto::session_key(psk, a.client_nonce, a.host_nonce);
    s->key.set_key(key.data(), key.size());
    s->name = p.hello.client_name;
    s->last_rx_us = now;
    wel.session_id = s->id;

    std::unique_ptr<Session> previous;
    {
        std::lock_guard<std::mutex> lk(mu_);
        wel.config = config_;
        previous = std::move(session_);
        session_ = std::move(s);
    }
    if (previous) {
        // Tell the displaced client why, so it stops instead of fighting back
        // for the host in a reconnect loop. Sealed with its own session key.
        ByteWriter bw(out, sizeof(out));
        proto::write_header(bw, PacketType::Bye, previous->id);
        proto::write(bw, proto::Bye{proto::ByeReason::Replaced});
        if (seal(bw, previous->key)) sock_.send_to(previous->addr, out, bw.size());
    }
    ByteWriter w(out, sizeof(out));
    proto::write_header(w, PacketType::Welcome, 0);
    proto::write(w, wel);
    sock_.send_to(from, out, w.size());
    sock_.set_traffic_class(from, TrafficClass::Video);

    connected_ = true;
    HK_LOGI("Client \"%s\" connected from %s%s", p.hello.client_name, from.to_string().c_str(),
            previous ? (" (took over from \"" + previous->name + "\")").c_str() : "");
    if (ev_.connected) ev_.connected(p.hello.client_name, p.hello.preset, p.hello.max_bitrate_kbps);
}

void HostNet::tx_loop()
{
    set_thread_role(ThreadRole::Network, "hikari-video-tx");
    PreciseTimer timer;
    while (running_) {
        if (WaitForSingleObject(tx_queue_.event(), 100) != WAIT_OBJECT_0) continue;
        while (auto item = tx_queue_.pop()) {
            std::unique_ptr<OutgoingFrame> f = std::move(*item);
            uint32_t id;
            NetAddress addr;
            if (!session_snapshot(id, addr)) continue;

            proto::VideoFragment meta;
            meta.frame_id = f->frame_id;
            meta.flags = static_cast<uint8_t>((f->keyframe ? proto::kVideoKeyframe : 0) |
                                              (f->recovery ? proto::kVideoRecovery : 0));
            meta.fec_group = f->fec_group;
            meta.capture_us = f->capture_us;
            meta.encode_us = f->encode_us;
            const uint64_t t0 = now_us();
            meta.host_us = static_cast<uint16_t>(std::min<uint64_t>(t0 - f->capture_us, 0xFFFF));

            // Pace in small bursts so a large keyframe does not overflow
            // shallow router/Wi-Fi queues, without delaying normal frames.
            const double bps = pace_bps_.load();
            size_t sent_bytes = 0;
            int burst = 0;
            packetizer_.packetize(id, meta, f->data.data(), f->data.size(), [&](const uint8_t* d, size_t n) {
                sock_.send_to(addr, d, n);
                sent_bytes += n;
                if (++burst % 4 == 0) {
                    const uint64_t due = t0 + static_cast<uint64_t>(double(sent_bytes) * 8e6 / bps);
                    const uint64_t now = now_us();
                    if (due > now + 300)
                        timer.sleep_until_us(due);
                    else
                        while (now_us() < due) YieldProcessor();
                }
            });
            const uint64_t done = now_us();
            capture_to_sent_.record(done - f->capture_us, done);
            video_bytes_.add(sent_bytes);
            ++frames_sent_;
        }
    }
}

}  // namespace hikari

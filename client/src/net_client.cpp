#include "net_client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/secure_packet.h"
#include "hikari/system.h"

namespace hikari {

using proto::PacketType;

namespace {
constexpr uint64_t kHelloInterval = 300'000;
constexpr uint64_t kHandshakeTimeout = 6'000'000;
constexpr uint64_t kLinkTimeout = 3'000'000;
constexpr uint64_t kReportInterval = 100'000;
constexpr uint64_t kKeyframeRetry = 400'000;

std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}
}  // namespace

const char* to_string(ConnState s)
{
    switch (s) {
    case ConnState::Idle: return "Not connected";
    case ConnState::Connecting: return "Connecting";
    case ConnState::Connected: return "Connected";
    case ConnState::Reconnecting: return "Reconnecting";
    case ConnState::Rejected: return "Pairing code rejected";
    case ConnState::Unreachable: return "Host unreachable";
    }
    return "?";
}

ClientNet::ClientNet() = default;

ClientNet::~ClientNet() { stop(); }

bool ClientNet::start(ClientNetEvents ev)
{
    ev_ = std::move(ev);
    if (!sock_.open(0, true)) return false;
    running_ = true;
    rx_ = std::thread(&ClientNet::rx_loop, this);
    return true;
}

void ClientNet::stop()
{
    if (!running_.exchange(false)) return;
    disconnect();
    sock_.close();
    if (rx_.joinable()) rx_.join();
}

void ClientNet::set_state(ConnState s, const std::string& detail)
{
    if (state_.exchange(s) == s) return;
    HK_LOGI("Connection: %s%s%s", to_string(s), detail.empty() ? "" : " - ", detail.c_str());
    if (ev_.state) ev_.state(s, detail);
}

void ClientNet::connect(const std::string& host, const std::string& code, proto::Preset preset, uint32_t max_kbps)
{
    std::lock_guard<std::mutex> lk(mu_);
    host_ = host;
    psk_ = crypto::psk_from_code(code);
    preset_ = preset;
    max_kbps_ = max_kbps;
    want_connect_ = true;
    target_changed_ = true;
    session_ = 0;
    have_challenge_ = false;
    next_resolve_ = 0;
}

void ClientNet::disconnect()
{
    bool had_session;
    {
        std::lock_guard<std::mutex> lk(mu_);
        had_session = session_ != 0;
    }
    if (had_session) send_sealed(PacketType::Bye, [](ByteWriter&) {});
    {
        std::lock_guard<std::mutex> lk(mu_);
        want_connect_ = false;
        session_ = 0;
    }
    set_state(ConnState::Idle);
}

void ClientNet::send_sealed(PacketType t, const std::function<void(ByteWriter&)>& body)
{
    uint8_t buf[proto::kMaxDatagram];
    std::lock_guard<std::mutex> lk(mu_);
    if (!session_) return;
    ByteWriter w(buf, sizeof(buf));
    proto::write_header(w, t, session_);
    body(w);
    if (seal(w, key_)) sock_.send_to(addr_, buf, w.size());
}

void ClientNet::send_input(proto::Input in)
{
    if (clock_.valid()) {
        in.client_time_us = clock_.to_remote(in.client_time_us);
        in.flags |= proto::kInputHostClock;
    }
    send_sealed(PacketType::Input, [&](ByteWriter& w) { proto::write(w, in); });
}

void ClientNet::play()
{
    send_sealed(PacketType::Play, [](ByteWriter& w) { proto::write(w, proto::Play{1}); });
}

void ClientNet::stop_stream()
{
    send_sealed(PacketType::Stop, [](ByteWriter&) {});
}

void ClientNet::set_preset(proto::Preset p, uint32_t max_kbps)
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        preset_ = p;
        max_kbps_ = max_kbps;
    }
    proto::SetPreset sp{p, max_kbps};
    send_sealed(PacketType::SetPreset, [&](ByteWriter& w) { proto::write(w, sp); });
}

void ClientNet::power(proto::PowerAction a)
{
    proto::Power p{a};
    send_sealed(PacketType::Power, [&](ByteWriter& w) { proto::write(w, p); });
}

void ClientNet::request_keyframe(uint8_t reason)
{
    proto::KeyframeRequest k{assembler_.highest_frame(), reason};
    send_sealed(PacketType::KeyframeRequest, [&](ByteWriter& w) { proto::write(w, k); });
    ++keyframe_requests_;
}

void ClientNet::recycle(std::unique_ptr<AssembledFrame> f)
{
    std::lock_guard<std::mutex> lk(recycle_mu_);
    if (recycled_.size() < 32) recycled_.push_back(std::move(f));
}

void ClientNet::set_decode_feedback(uint16_t queue, uint16_t decode_p95_us, double present_fps, uint16_t audio_ms)
{
    fb_queue_ = queue;
    fb_decode_p95_ = decode_p95_us;
    fb_present_fps_x100_ = static_cast<uint16_t>(std::min(present_fps * 100.0, 65535.0));
    fb_audio_ms_ = audio_ms;
}

ClientNetStats ClientNet::stats()
{
    ClientNetStats s;
    s.state = state_.load();
    s.rtt_ms = clock_.last_rtt() / 1000.0;
    s.jitter_ms = jitter_us_ / 1000.0;
    s.loss_pct = loss_pct_.load();
    s.mbps = mbps_.load();
    s.queue_delay_ms = last_queue_delay_ / 1000.0;
    const auto& a = assembler_.stats();
    s.frames_complete = a.frames_complete;
    s.frames_dropped = a.frames_dropped;
    s.frames_fec_recovered = a.frames_fec_recovered;
    s.keyframe_requests = keyframe_requests_.load();
    s.clock_synced = clock_.valid();
    return s;
}

std::vector<DiscoveredHost> ClientNet::discovered()
{
    const uint64_t now = now_us();
    std::lock_guard<std::mutex> lk(disc_mu_);
    std::vector<DiscoveredHost> out;
    for (auto& [id, h] : discovered_)
        if (now - h.last_seen_us < 6'000'000) out.push_back(h);
    return out;
}

void ClientNet::handshake_step(uint64_t now)
{
    std::unique_lock<std::mutex> lk(mu_);
    if (!want_connect_ || session_) return;

    if (target_changed_ || !addr_.valid()) {
        if (now < next_resolve_) return;
        const std::string host = host_;
        lk.unlock();
        NetAddress a;
        const bool ok = NetAddress::resolve(host, proto::kDefaultPort, a);  // DNS: may block briefly
        lk.lock();
        if (!ok) {
            next_resolve_ = now + 2'000'000;
            lk.unlock();
            set_state(ConnState::Unreachable, "cannot resolve " + host);
            return;
        }
        addr_ = a;
        target_changed_ = false;
        handshake_started_ = now;
        have_challenge_ = false;
        crypto::random_bytes(client_nonce_, sizeof(client_nonce_));
    }
    if (now - last_hello_ < kHelloInterval) return;
    last_hello_ = now;

    uint8_t buf[256];
    ByteWriter w(buf, sizeof(buf));
    if (!have_challenge_) {
        proto::Hello h;
        std::memcpy(h.client_nonce, client_nonce_, sizeof(client_nonce_));
        std::snprintf(h.client_name, sizeof(h.client_name), "%s", computer_name().c_str());
        h.preset = preset_;
        h.max_bitrate_kbps = max_kbps_;
#ifdef HIKARI_WITH_OPUS
        h.audio_codecs = 3;
#else
        h.audio_codecs = 1;
#endif
        proto::write_header(w, PacketType::Hello, 0);
        proto::write(w, h);
    } else {
        proto::Auth a;
        std::memcpy(a.client_nonce, client_nonce_, sizeof(client_nonce_));
        std::memcpy(a.host_nonce, host_nonce_, sizeof(host_nonce_));
        const auto mac = crypto::auth_mac(psk_, client_nonce_, host_nonce_);
        std::memcpy(a.mac, mac.data(), mac.size());
        proto::write_header(w, PacketType::Auth, 0);
        proto::write(w, a);
    }
    sock_.send_to(addr_, buf, w.size());
    const bool timed_out = now - handshake_started_ > kHandshakeTimeout;
    if (timed_out) {
        // Keep trying forever (host may be rebooting); restart the exchange.
        handshake_started_ = now;
        have_challenge_ = false;
        crypto::random_bytes(client_nonce_, sizeof(client_nonce_));
        next_resolve_ = 0;
        target_changed_ = true;  // re-resolve: DHCP / Tailscale address may change
    }
    lk.unlock();
    if (timed_out) set_state(ConnState::Unreachable, "no answer from host");
    else if (state_ != ConnState::Reconnecting && state_ != ConnState::Unreachable) set_state(ConnState::Connecting);
}

void ClientNet::handle(const uint8_t* buf, size_t len, const NetAddress& from, uint64_t now)
{
    ByteReader r(buf, len);
    proto::Header h;
    if (!proto::read_header(r, h)) return;

    switch (h.type) {
    case PacketType::DiscoverReply: {
        proto::DiscoverReply d;
        if (!proto::read(r, d)) return;
        NetAddress a = from;
        DiscoveredHost dh;
        dh.name = d.host_name;
        dh.host_id = hex(d.host_id, sizeof(d.host_id));
        const std::string s = a.to_string();
        dh.address = s.substr(0, s.rfind(':')) + (d.port == proto::kDefaultPort ? "" : ":" + std::to_string(d.port));
        dh.last_seen_us = now;
        std::lock_guard<std::mutex> lk(disc_mu_);
        discovered_[dh.host_id] = dh;
        return;
    }
    case PacketType::Challenge: {
        proto::Challenge c;
        if (!proto::read(r, c)) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (session_ || !want_connect_ || from != addr_) return;
        std::memcpy(host_nonce_, c.host_nonce, sizeof(host_nonce_));
        have_challenge_ = true;
        last_hello_ = 0;  // answer immediately
        return;
    }
    case PacketType::Reject: {
        proto::Reject rj;
        if (!proto::read(r, rj)) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            want_connect_ = false;
        }
        set_state(ConnState::Rejected, rj.reason == proto::RejectReason::BadVersion ? "different Hikari version"
                                                                                    : "wrong pairing code");
        return;
    }
    case PacketType::Welcome: {
        proto::Welcome wel;
        if (!proto::read(r, wel)) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (session_ || !have_challenge_ || from != addr_) return;
            const auto want = crypto::welcome_mac(psk_, host_nonce_, client_nonce_);
            if (!crypto::constant_time_equal(want.data(), wel.mac, proto::kMacSize)) return;  // impostor
            session_ = wel.session_id;
            const auto k = crypto::session_key(psk_, client_nonce_, host_nonce_);
            key_.set_key(k.data(), k.size());
            last_rx_ = now;
        }
        assembler_.reset();
        have_transit_ = false;
        next_ping_ = 0;
        sock_.set_traffic_class(from, TrafficClass::Control);
        set_state(ConnState::Connected);
        if (ev_.stream_config) ev_.stream_config(wel.config);
        return;
    }
    default:
        break;
    }

    // Session traffic.
    size_t body = len;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!session_ || h.session != session_) return;
        if (!is_media(h.type)) {
            body = open_sealed(buf, len, key_);
            if (!body) return;
        }
        last_rx_ = now;
    }
    ByteReader b(buf, body);
    proto::read_header(b, h);
    switch (h.type) {
    case PacketType::Video:
    case PacketType::VideoParity: {
        proto::VideoFragment f;
        if (!proto::read_video_header(b, f)) return;
        if (f.flags & proto::kVideoKeyframe) awaiting_keyframe_ = false;
        assembler_.push(f, h.type == PacketType::VideoParity, now);
        while (auto fr = assembler_.pop(now)) on_frame_ready(std::move(fr), now);
        break;
    }
    case PacketType::Audio: {
        proto::AudioPacket a;
        if (proto::read(b, a) && ev_.audio) ev_.audio(a);
        break;
    }
    case PacketType::Pong: {
        proto::Pong p;
        if (proto::read(b, p)) clock_.add_sample(p.echo_us, p.remote_us, now);
        break;
    }
    case PacketType::HostStatus: {
        proto::HostStatus s;
        if (proto::read(b, s) && ev_.host_status) ev_.host_status(s);
        break;
    }
    case PacketType::HostStats: {
        proto::HostStats s;
        if (proto::read(b, s) && ev_.host_stats) ev_.host_stats(s);
        break;
    }
    case PacketType::StreamConfig: {
        proto::StreamConfig c;
        if (proto::read(b, c) && ev_.stream_config) ev_.stream_config(c);
        break;
    }
    case PacketType::Bye: {
        {
            std::lock_guard<std::mutex> lk(mu_);
            session_ = 0;
            have_challenge_ = false;
            target_changed_ = true;
        }
        set_state(ConnState::Reconnecting, "host closed the session");
        break;
    }
    default:
        break;
    }
}

void ClientNet::on_frame_ready(std::unique_ptr<AssembledFrame> f, uint64_t now)
{
    // Network one-way delay estimate for this frame, independent of frame
    // size: first packet arrival vs. when the host sent it (capture + host_us).
    const int64_t transit = static_cast<int64_t>(f->first_packet_us) - static_cast<int64_t>(f->capture_us + f->host_us);
    if (have_transit_) {
        const double d = std::fabs(double(transit - prev_transit_));
        jitter_us_ += (d - jitter_us_) / 16.0;
    }
    prev_transit_ = transit;
    have_transit_ = true;
    if (now - min_window_start_ > 5'000'000) {  // two 5 s windows -> 5..10 s minimum
        min_transit_[1] = min_transit_[0];
        min_transit_[0] = INT64_MAX;
        min_window_start_ = now;
    }
    min_transit_[0] = std::min(min_transit_[0], transit);
    last_queue_delay_ = transit - std::min(min_transit_[0], min_transit_[1]);

    if (!frames_.push(std::move(f))) {
        // The decoder is not keeping up: everything queued is stale. Flush
        // and restart from a keyframe rather than build latency.
        frames_.clear([&](std::unique_ptr<AssembledFrame>&& x) { assembler_.recycle(std::move(x)); });
        awaiting_keyframe_ = true;
        last_keyframe_req_ = now;
        request_keyframe(2);
    }
}

void ClientNet::periodic(uint64_t now)
{
    {
        std::lock_guard<std::mutex> lk(recycle_mu_);
        for (auto& f : recycled_) assembler_.recycle(std::move(f));
        recycled_.clear();
    }
    assembler_.set_reorder_window(reorder_window_us_.load());
    while (auto fr = assembler_.pop(now)) on_frame_ready(std::move(fr), now);

    bool connected;
    {
        std::lock_guard<std::mutex> lk(mu_);
        connected = session_ != 0;
        if (connected && now - last_rx_ > kLinkTimeout) {
            session_ = 0;
            have_challenge_ = false;
            target_changed_ = true;
            connected = false;
            last_rx_ = now;
        }
    }
    if (!connected) {
        if (state_ == ConnState::Connected) set_state(ConnState::Reconnecting, "connection lost");
        handshake_step(now);
    } else {
        // Loss -> ask for a keyframe; repeat if it does not arrive promptly.
        if (assembler_.take_loss_event() && now - last_keyframe_req_ > 120'000) {
            awaiting_keyframe_ = true;
            last_keyframe_req_ = now;
            request_keyframe(1);
        } else if (awaiting_keyframe_ && now - last_keyframe_req_ > kKeyframeRetry) {
            last_keyframe_req_ = now;
            request_keyframe(1);
        }
        if (now >= next_ping_) {
            proto::Ping p{++ping_seq_, now};
            send_sealed(PacketType::Ping, [&](ByteWriter& w) { proto::write(w, p); });
            // Fast pings until the clock has converged, then 4 Hz.
            next_ping_ = now + (clock_.valid() && ping_seq_ > 20 ? 250'000 : 50'000);
        }
        if (now >= next_report_) {
            next_report_ = now + kReportInterval;
            const AssemblerStats& a = assembler_.stats();
            proto::ReceiverReport rr;
            rr.highest_frame = assembler_.highest_frame();
            rr.interval_us = static_cast<uint32_t>(last_report_us_ ? now - last_report_us_ : kReportInterval);
            const uint64_t expected = a.packets_expected - last_stats_.packets_expected;
            const uint64_t got = a.data_packets_received - last_stats_.data_packets_received;
            rr.packets_received = static_cast<uint32_t>(a.packets_received - last_stats_.packets_received);
            rr.packets_lost = static_cast<uint32_t>(expected > got ? expected - got : 0);
            rr.bytes_received = static_cast<uint32_t>(a.bytes_received - last_stats_.bytes_received);
            rr.frames_complete = static_cast<uint32_t>(a.frames_complete - last_stats_.frames_complete);
            rr.frames_dropped = static_cast<uint32_t>(a.frames_dropped - last_stats_.frames_dropped);
            rr.frames_fec_recovered = static_cast<uint32_t>(a.frames_fec_recovered - last_stats_.frames_fec_recovered);
            rr.rtt_us = static_cast<uint32_t>(clock_.last_rtt());
            rr.jitter_us = static_cast<uint32_t>(jitter_us_);
            rr.queue_delay_us = static_cast<int32_t>(last_queue_delay_);
            rr.decode_queue = fb_queue_;
            rr.decode_p95_us = fb_decode_p95_;
            rr.present_fps_x100 = fb_present_fps_x100_;
            rr.audio_buffer_ms = fb_audio_ms_;
            send_sealed(PacketType::ReceiverReport, [&](ByteWriter& w) { proto::write(w, rr); });

            const double total = double(rr.packets_received) + rr.packets_lost;
            loss_pct_ = total > 0 ? 100.0 * rr.packets_lost / total : 0.0;
            mbps_ = rr.interval_us ? rr.bytes_received * 8.0 / rr.interval_us : 0.0;
            last_stats_ = a;
            last_report_us_ = now;
        }
    }

    if (discovery_ && now >= next_discover_) {
        next_discover_ = now + 1'500'000;
        uint8_t buf[16];
        ByteWriter w(buf, sizeof(buf));
        proto::write_header(w, PacketType::Discover, 0);
        sock_.send_to(NetAddress::broadcast(proto::kDefaultPort), buf, w.size());
    }
}

void ClientNet::rx_loop()
{
    set_thread_role(ThreadRole::Network, "hikari-net-rx");
    uint8_t buf[2048];
    uint64_t last_periodic = 0;
    while (running_) {
        NetAddress from;
        // Short timeout: frame deadlines and keyframe retries are checked here.
        const int n = sock_.recv_from(buf, sizeof(buf), from, 1);
        const uint64_t now = now_us();
        if (n > 0) handle(buf, static_cast<size_t>(n), from, now);
        if (n <= 0 || now - last_periodic >= 1000) {
            last_periodic = now;
            periodic(now);
        }
    }
}

}  // namespace hikari

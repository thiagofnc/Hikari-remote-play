#pragma once
// Client network endpoint.
//
// One RX thread does everything time-critical on the receive side: pairing
// handshake, frame reassembly (FEC, deadlines), keyframe requests, receiver
// reports, clock sync and LAN discovery. Complete frames are handed to the
// decode thread through a small bounded queue; input is sent directly from the
// input thread (send_input) without touching the RX thread.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hikari/crypto.h"
#include "hikari/histogram.h"
#include "hikari/net_protocol.h"
#include "hikari/sync.h"
#include "hikari/udp.h"
#include "hikari/video_transport.h"

namespace hikari {

enum class ConnState { Idle, Connecting, Connected, Reconnecting, Rejected, Unreachable };
const char* to_string(ConnState s);

struct DiscoveredHost {
    std::string name;
    std::string address;  // "ip:port"
    std::string host_id;  // hex
    uint64_t last_seen_us = 0;
};

struct ClientNetStats {
    ConnState state = ConnState::Idle;
    double rtt_ms = 0;
    double jitter_ms = 0;
    double loss_pct = 0;
    double mbps = 0;
    double queue_delay_ms = 0;
    uint64_t frames_complete = 0;
    uint64_t frames_dropped = 0;
    uint64_t frames_fec_recovered = 0;
    uint64_t keyframe_requests = 0;
    bool clock_synced = false;
};

struct ClientNetEvents {
    std::function<void(ConnState, const std::string& detail)> state;
    std::function<void(const proto::StreamConfig&)> stream_config;
    std::function<void(const proto::HostStatus&)> host_status;
    std::function<void(const proto::HostStats&)> host_stats;
    std::function<void(const proto::AudioPacket&)> audio;  // RX thread; copy what you need
};

class ClientNet {
public:
    ClientNet();
    ~ClientNet();

    bool start(ClientNetEvents ev);
    void stop();

    // Connects (and keeps reconnecting) to `host` ("name", "ip", "ip:port").
    void connect(const std::string& host, const std::string& pairing_code, proto::Preset preset,
                 uint32_t max_kbps);
    void disconnect();
    ConnState state() const { return state_.load(); }

    // Thread-safe senders.
    void send_input(proto::Input in);
    void play();
    void stop_stream();
    void set_preset(proto::Preset p, uint32_t max_kbps);
    void power(proto::PowerAction a);
    void request_keyframe(uint8_t reason);

    // Decode thread side.
    using FrameQueue = BoundedQueue<std::unique_ptr<AssembledFrame>, 8>;
    FrameQueue& frames() { return frames_; }
    void recycle(std::unique_ptr<AssembledFrame> f);
    void set_decode_feedback(uint16_t queue, uint16_t decode_p95_us, double present_fps, uint16_t audio_ms);
    void set_reorder_window(uint64_t us) { reorder_window_us_ = us; }

    const ClockSync& clock() const { return clock_; }
    ClientNetStats stats();

    void set_discovery(bool on) { discovery_ = on; }
    std::vector<DiscoveredHost> discovered();

private:
    void rx_loop();
    void handle(const uint8_t* buf, size_t len, const NetAddress& from, uint64_t now);
    void handshake_step(uint64_t now);
    void send_sealed(proto::PacketType t, const std::function<void(ByteWriter&)>& body);
    void periodic(uint64_t now);
    void on_frame_ready(std::unique_ptr<AssembledFrame> f, uint64_t now);
    void set_state(ConnState s, const std::string& detail = "");

    UdpSocket sock_;
    std::thread rx_;
    std::atomic<bool> running_{false};
    ClientNetEvents ev_;
    std::atomic<ConnState> state_{ConnState::Idle};

    // Connection target + session (guarded by mu_).
    std::mutex mu_;
    std::string host_;
    NetAddress addr_;
    crypto::Digest psk_{};
    proto::Preset preset_ = proto::Preset::Balanced;
    uint32_t max_kbps_ = 0;
    bool want_connect_ = false;
    bool target_changed_ = false;
    uint32_t session_ = 0;
    crypto::Hmac key_;
    uint8_t client_nonce_[proto::kNonceSize]{};
    uint8_t host_nonce_[proto::kNonceSize]{};
    bool have_challenge_ = false;
    uint64_t handshake_started_ = 0;
    uint64_t last_hello_ = 0;
    uint64_t last_rx_ = 0;
    uint64_t next_resolve_ = 0;

    FrameAssembler assembler_;
    FrameQueue frames_;
    std::mutex recycle_mu_;
    std::vector<std::unique_ptr<AssembledFrame>> recycled_;
    std::atomic<uint64_t> reorder_window_us_{3000};

    ClockSync clock_;
    uint32_t ping_seq_ = 0;
    uint64_t next_ping_ = 0;
    uint64_t next_report_ = 0;
    uint64_t last_keyframe_req_ = 0;
    bool awaiting_keyframe_ = false;
    std::atomic<uint64_t> keyframe_requests_{0};

    // Report deltas.
    AssemblerStats last_stats_{};
    uint64_t last_report_us_ = 0;
    double jitter_us_ = 0;
    int64_t prev_transit_ = 0;
    bool have_transit_ = false;
    int64_t min_transit_[2] = {INT64_MAX, INT64_MAX};
    uint64_t min_window_start_ = 0;
    int64_t last_queue_delay_ = 0;
    std::atomic<double> loss_pct_{0};
    std::atomic<double> mbps_{0};

    std::atomic<uint16_t> fb_queue_{0}, fb_decode_p95_{0}, fb_present_fps_x100_{0}, fb_audio_ms_{0};

    std::atomic<bool> discovery_{false};
    uint64_t next_discover_ = 0;
    std::mutex disc_mu_;
    std::map<std::string, DiscoveredHost> discovered_;
};

}  // namespace hikari

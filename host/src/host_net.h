#pragma once
// Host network endpoint: pairing handshake, one active client session,
// input/control reception, paced video and audio transmission.
//
// Threads:
//   net-rx     (Input role)   receives everything; input is forwarded to the
//                             bridge from this thread with no queue hop.
//   video-tx   (Network role) packetizes + paces encoded frames.
// Audio is sent directly from the audio capture thread (tiny packets).

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hikari/crypto.h"
#include "hikari/histogram.h"
#include "hikari/net_protocol.h"
#include "hikari/sync.h"
#include "hikari/udp.h"
#include "hikari/video_transport.h"

namespace hikari {

struct HostNetEvents {
    std::function<void(const std::string& client_name, proto::Preset, uint32_t max_kbps)> connected;
    std::function<void()> disconnected;
    std::function<void(const proto::Input&, uint64_t rx_us)> input;  // runs on net-rx: keep it fast
    std::function<void(uint8_t reason)> keyframe_request;
    std::function<void(const proto::ReceiverReport&)> receiver_report;
    std::function<void(const proto::Play&)> play;
    std::function<void()> stop;
    std::function<void(const proto::Power&)> power;
    std::function<void(const proto::SetPreset&)> preset;
    std::function<void()> sender_overflow;  // video sender fell behind: force a keyframe
};

struct OutgoingFrame {
    std::vector<uint8_t> data;
    uint32_t frame_id = 0;
    bool keyframe = false;
    bool recovery = false;
    uint64_t capture_us = 0;
    uint16_t encode_us = 0;
    uint8_t fec_group = 0;
};

struct HostNetStats {
    bool client_connected = false;
    std::string client_name;
    std::string client_address;
    LatencySummary input_owd;       // client sample -> host receive
    LatencySummary capture_to_sent; // capture -> last packet of the frame sent
    double input_rate = 0;
    double video_mbps = 0;
    uint64_t frames_sent = 0;
    uint64_t frames_dropped_sender = 0;
    uint64_t send_errors = 0;
};

class HostNet {
public:
    HostNet();
    ~HostNet();

    bool start(uint16_t port, const crypto::Digest& psk, const std::string& host_name, const uint8_t host_id[8],
               HostNetEvents events);
    void stop();

    void set_stream_config(const proto::StreamConfig& cfg);
    void send_video(OutgoingFrame&& f);
    void send_audio(const proto::AudioPacket& meta, const uint8_t* data, size_t len);
    void send_status(const proto::HostStatus& s);
    void send_stats(const proto::HostStats& s);
    // Spread each frame over 1/k of the frame interval at the current bitrate.
    void set_pacing(double spread_k, uint32_t bitrate_kbps, uint16_t fps);
    void disconnect_client();

    bool client_connected() const { return connected_.load(); }
    HostNetStats stats();

private:
    struct Session {
        uint32_t id = 0;
        NetAddress addr;
        crypto::Hmac key;
        std::string name;
        uint64_t last_rx_us = 0;
        uint32_t last_input_seq = 0;
        bool have_input = false;
    };
    struct PendingHandshake {
        proto::Hello hello;
        uint8_t host_nonce[proto::kNonceSize];
        uint64_t at_us;
    };

    void rx_loop();
    void tx_loop();
    void handle(const uint8_t* buf, size_t len, const NetAddress& from, uint64_t now);
    void handle_hello(ByteReader& r, const NetAddress& from, uint64_t now);
    void handle_auth(ByteReader& r, const NetAddress& from, uint64_t now);
    void send_control(proto::PacketType type, const std::function<void(ByteWriter&)>& body);
    void end_session(const char* why);
    bool session_snapshot(uint32_t& id, NetAddress& addr);

    UdpSocket sock_;
    std::thread rx_thread_, tx_thread_;
    std::atomic<bool> running_{false};
    HostNetEvents ev_;
    crypto::Digest psk_{};
    std::string host_name_;
    uint8_t host_id_[8]{};

    mutable std::mutex mu_;  // guards session_, pending_, config_
    std::unique_ptr<Session> session_;
    std::map<std::string, PendingHandshake> pending_;
    proto::StreamConfig config_;
    std::atomic<bool> connected_{false};

    BoundedQueue<std::unique_ptr<OutgoingFrame>, 3> tx_queue_;
    VideoPacketizer packetizer_;
    std::atomic<double> pace_bps_{100e6};

    RollingHistogram input_owd_;
    RollingHistogram capture_to_sent_;
    RateCounter input_rate_;
    RateCounter video_bytes_;
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    uint64_t last_discover_reply_us_ = 0;
};

}  // namespace hikari

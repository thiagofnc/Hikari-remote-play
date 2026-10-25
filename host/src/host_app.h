#pragma once
// The Hikari host: owns every subsystem and runs the "Play" state machine.
//
//   Lobby -> Waking -> WaitingForVideo -> WaitingForController -> StartingStream -> Streaming
//                                     \-> Failed (with a human-readable reason)
//
// Everything is automatic: devices are detected and re-detected, the encoder
// is benchmarked once and cached, and any subsystem that fails is retried in
// the background without user involvement.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_capture.h"
#include "bridge.h"
#include "capture.h"
#include "encoder.h"
#include "gpu_convert.h"
#include "hikari/abr.h"
#include "hikari/config.h"
#include "hikari/sync.h"
#include "host_net.h"
#include "host_system.h"

namespace hikari {

struct HostAddress {
    std::string ip;
    std::string adapter;
    bool tailscale = false;
};
std::vector<HostAddress> local_addresses();

struct HostSnapshot {
    std::string host_name;
    std::string pairing_code;
    std::vector<HostAddress> addresses;
    uint16_t port = proto::kDefaultPort;
    std::string tailscale_ip;
    std::string tailscale_name;  // MagicDNS name, when Tailscale reports one
    std::string invite;          // hikari://... string for the client's paste box

    // Is this PC dependably reachable?
    FirewallStatus firewall;
    int lid_action = -1;         // see lid_close_action()
    bool autostart = false;

    bool bridge_ok = false;
    std::string bridge_port;
    uint8_t usb_state = HK_USB_DETACHED;
    uint8_t wake_methods = 0;
    bool remote_wakeup = false;
    BridgeStatus bridge;

    bool capture_ok = false;
    std::string capture_name;
    std::string capture_mode;
    bool signal = false;
    CaptureStats capture;

    bool audio_ok = false;
    std::string audio_name;

    bool encoder_ok = false;
    bool encoder_benchmarking = false;
    std::string encoder_name;
    std::string gpu_name;

    bool client_connected = false;
    std::string client_name;
    std::string client_address;

    proto::ConsoleState console = proto::ConsoleState::Unknown;
    proto::SessionPhase phase = proto::SessionPhase::Lobby;
    proto::HostIssue issue = proto::HostIssue::None;
    std::string message;

    VideoMode mode;
    uint32_t bitrate_kbps = 0;
    uint8_t fec_group = 0;
    double encode_fps = 0;
    LatencySummary encode_latency;
    HostNetStats net;
};

class HostApp {
public:
    HostApp();
    ~HostApp();

    // `cfg` must outlive the app; detected settings are written back to it.
    bool start(Config& cfg);
    void stop();

    HostSnapshot snapshot();
    void regenerate_pairing_code();
    void redetect_devices();
    void wake_console();  // manual wake from the host UI

    // "Always reachable" fixes offered by the host window.
    bool allow_through_firewall();  // one UAC prompt
    bool keep_awake_with_lid_closed();
    bool set_start_with_windows(bool on);

private:
    struct EncodeJob {
        ComPtr<ID3D11Texture2D> texture;
        uint64_t capture_us;
        uint32_t index;
    };

    void setup_thread();
    bool select_encoder();
    bool open_capture();
    bool recover_gpu();
    void encoder_thread();
    void orchestrator_thread();
    void tick(uint64_t now);
    void update_console_state(uint64_t now);
    void set_phase(proto::SessionPhase p, const std::string& msg, proto::HostIssue issue = proto::HostIssue::None);
    bool start_stream();
    void stop_stream();
    void on_captured(CapturedFrame&& f);
    void on_encoded(EncodedFrame&& f);
    void send_status();
    void send_stats();
    void apply_preset(proto::Preset p, uint32_t max_kbps);

    Config* cfg_ = nullptr;
    std::unique_ptr<MediaRuntime> media_owned_;
    std::atomic<bool> running_{false};
    std::thread setup_thread_, encoder_thread_, orchestrator_thread_, sysinfo_thread_;
    void sysinfo_thread();
    Event sysinfo_wake_;
    std::mutex sysinfo_mu_;
    std::string tailscale_name_;
    FirewallStatus firewall_;
    int lid_action_ = -1;
    Event wake_;      // orchestrator
    Event enc_wake_;  // encoder thread
    std::atomic<bool> input_enabled_{false};
    std::atomic<uint16_t> abr_fps_{60};
    std::atomic<uint8_t> abr_fec_{0};
    std::atomic<int> encoder_failures_{0};
    std::atomic<uint64_t> last_encoded_us_{0};
    std::atomic<uint64_t> converted_frames_{0};

    GpuDevice gpu_;
    std::mutex gpu_mu_;  // device rebuild vs. capture reopen
    std::atomic<bool> gpu_ready_{false};
    std::vector<EncoderInfo> encoders_;
    int encoder_index_ = -1;
    std::atomic<bool> benchmarking_{false};

    VideoCapture capture_;
    CaptureDeviceInfo capture_dev_;
    std::atomic<bool> capture_ok_{false};
    std::mutex convert_mu_;
    GpuConverter converter_;
    bool converter_ready_ = false;

    VideoEncoder encoder_;
    Mailbox<EncodeJob> encode_mailbox_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> want_reconfigure_{false};
    std::atomic<uint32_t> pending_bitrate_{0};
    std::atomic<uint32_t> stream_frame_id_{0};
    std::atomic<bool> recovery_next_{false};
    RateCounter encode_fps_;
    uint64_t last_keyframe_request_us_ = 0;

    AudioCapture audio_;
    AudioEndpointInfo audio_ep_;
    bool audio_found_ = false;
    std::atomic<uint32_t> audio_seq_{0};

    Bridge bridge_;
    HostNet net_;

    std::mutex abr_mu_;
    AdaptiveBitrate abr_;
    AbrDecision abr_now_;
    proto::Preset preset_ = proto::Preset::Balanced;
    uint32_t client_max_kbps_ = 0;

    // State machine (orchestrator thread + guarded reads for snapshot()).
    mutable std::mutex state_mu_;
    proto::ConsoleState console_ = proto::ConsoleState::Unknown;
    proto::SessionPhase phase_ = proto::SessionPhase::Lobby;
    proto::HostIssue issue_ = proto::HostIssue::None;
    std::string message_;
    uint64_t phase_since_ = 0;
    std::atomic<bool> play_requested_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> manual_wake_{false};
    std::atomic<bool> sleep_requested_{false};
    uint64_t next_wake_at_ = 0;
    int wake_attempts_ = 0;
    uint64_t client_gone_at_ = 0;
    uint64_t last_status_us_ = 0;
    uint64_t last_stats_us_ = 0;
    uint64_t last_capture_retry_us_ = 0;

    std::string pairing_code_;
    uint8_t host_id_[8]{};
    std::string host_name_;
    uint16_t port_ = proto::kDefaultPort;
};

}  // namespace hikari

#pragma once
// Hikari client <-> host network protocol (UDP).
//
// One UDP flow carries independent traffic classes. Nothing is ever
// retransmitted: input is newest-state-wins, video frames have a deadline and
// are abandoned when late, audio uses loss concealment. See docs/protocol.md.
//
// Every datagram starts with:
//     magic:u8  type:u8  session:u32
// Packets that can change console state (input, control) end with an 8-byte
// truncated HMAC-SHA256 tag keyed by the per-session key.

#include <cstddef>
#include <cstdint>

#include "hikari/byte_io.h"

namespace hikari::proto {

constexpr uint16_t kDefaultPort = 47800;
constexpr uint8_t kMagic = 0xB7;
constexpr uint16_t kVersion = 1;

// Tailscale/WireGuard MTU is 1280; leave room for IPv6 + UDP headers.
constexpr size_t kMaxDatagram = 1200;
constexpr size_t kHeaderSize = 6;
constexpr size_t kTagSize = 8;
constexpr size_t kNonceSize = 16;
constexpr size_t kMacSize = 32;

enum class PacketType : uint8_t {
    Hello = 1,
    Challenge = 2,
    Auth = 3,
    Welcome = 4,
    Reject = 5,
    Bye = 6,

    Input = 10,

    Video = 20,
    VideoParity = 21,
    StreamConfig = 22,

    Audio = 30,

    Ping = 40,
    Pong = 41,
    ReceiverReport = 42,
    KeyframeRequest = 43,
    HostStats = 44,
    SetPreset = 45,
    LatencyProbe = 46,

    // Lobby: a paired client is connected but not streaming yet.
    HostStatus = 48,
    Play = 49,
    Stop = 50,
    Power = 51,

    // LAN discovery (unauthenticated; reveals only the host's display name).
    Discover = 60,
    DiscoverReply = 61,
};

// What the host believes the console is doing, fused from the USB bus state
// reported by the ESP32 and the HDMI capture signal.
enum class ConsoleState : uint8_t {
    Unknown = 0,
    Off = 1,          // USB not enumerated and no HDMI signal (off / undocked)
    Asleep = 2,       // USB suspended, no HDMI signal
    Waking = 3,       // wake in progress
    AwakeNoVideo = 4, // USB active but no HDMI picture yet
    Ready = 5,        // USB active + live HDMI picture
};

enum class SessionPhase : uint8_t {
    Lobby = 0,
    Waking = 1,
    WaitingForVideo = 2,
    WaitingForController = 3,
    StartingStream = 4,
    Streaming = 5,
    Failed = 6,
};

enum class HostIssue : uint8_t {
    None = 0,
    NoBridge = 1,       // ESP32 not found
    NoCapture = 2,      // capture card not found
    NoSignal = 3,       // capture present, no HDMI signal
    NoEncoder = 4,
    WakeFailed = 5,
    ControllerNotAccepted = 6,
};

enum class VideoCodec : uint8_t { H264 = 1, HEVC = 2 };
enum class AudioCodec : uint8_t { None = 0, PcmS16 = 1, Opus = 2 };
enum class Preset : uint8_t { MinimumLatency = 0, Balanced = 1, Quality = 2 };

enum class RejectReason : uint8_t { BadVersion = 1, BadAuth = 2, Busy = 3 };

struct Header {
    PacketType type{};
    uint32_t session = 0;
};

void write_header(ByteWriter& w, PacketType type, uint32_t session);
bool read_header(ByteReader& r, Header& h);

// ---- handshake -----------------------------------------------------------

struct Hello {
    uint16_t version = kVersion;
    uint8_t client_nonce[kNonceSize]{};
    char client_name[32]{};
    Preset preset = Preset::Balanced;
    uint32_t max_bitrate_kbps = 20000;
    uint8_t video_codecs = 1;  // bit0 H264, bit1 HEVC
    uint8_t audio_codecs = 1;  // bit0 PCM, bit1 Opus
    uint16_t display_hz = 60;
};

struct Challenge {
    uint8_t host_nonce[kNonceSize]{};
};

struct Auth {
    uint8_t client_nonce[kNonceSize]{};
    uint8_t host_nonce[kNonceSize]{};
    uint8_t mac[kMacSize]{};
};

struct StreamConfig {
    VideoCodec codec = VideoCodec::H264;
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint16_t fps = 60;
    uint32_t bitrate_kbps = 12000;
    AudioCodec audio = AudioCodec::PcmS16;
    uint32_t audio_rate = 48000;
    uint8_t audio_channels = 2;
    uint16_t audio_frame_samples = 240;  // per channel, per packet
    uint8_t fec_group = 0;               // data fragments per parity packet, 0 = off
    char encoder_name[48]{};
};

struct Welcome {
    uint32_t session_id = 0;
    uint8_t mac[kMacSize]{};
    StreamConfig config;
};

struct Reject {
    RejectReason reason = RejectReason::BadAuth;
};

// ---- input -----------------------------------------------------------------

// Canonical controller state on the network. Sticks are signed 16-bit with
// +Y = up (XInput convention); triggers are 0..255. Buttons use HK_BTN_*.
struct PadState {
    uint32_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
    int16_t gyro[3]{};
    int16_t accel[3]{};
    bool has_imu = false;

    bool operator==(const PadState& o) const;
    bool operator!=(const PadState& o) const { return !(*this == o); }
};

enum InputFlags : uint8_t {
    kInputConnected = 1 << 0,  // a physical controller is attached on the client
    kInputLatencyMark = 1 << 1,
    kInputRedundant = 1 << 2,  // repeat of an already-sent state
    kInputHostClock = 1 << 3,  // client_time_us already translated to the host clock
};

struct Input {
    uint32_t seq = 0;
    // Controller sample time. Once the client's clock sync has converged it
    // is expressed on the *host* clock (kInputHostClock), so the host can
    // measure true one-way input latency on its own.
    uint64_t client_time_us = 0;
    uint8_t flags = 0;
    PadState pad;
};

// ---- video -------------------------------------------------------------------

enum VideoFlags : uint8_t {
    kVideoKeyframe = 1 << 0,
    kVideoRecovery = 1 << 1,  // first frame after a keyframe request
};

struct VideoFragment {
    uint32_t frame_id = 0;
    uint16_t index = 0;       // data fragment index, or parity group index
    uint16_t data_count = 0;  // data fragments in this frame
    uint8_t flags = 0;
    uint8_t fec_group = 0;    // K, 0 = no parity
    uint32_t frame_size = 0;  // total encoded bytes
    uint64_t capture_us = 0;  // host clock: frame left the capture card
    uint16_t encode_us = 0;   // host encode duration
    uint16_t host_us = 0;     // capture -> first packet sent on host
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
};

constexpr size_t kVideoHeaderSize = 4 + 2 + 2 + 1 + 1 + 4 + 8 + 2 + 2;
constexpr size_t kVideoPayload = kMaxDatagram - kHeaderSize - kVideoHeaderSize;

void write_video_header(ByteWriter& w, const VideoFragment& f);
bool read_video_header(ByteReader& r, VideoFragment& f);

// ---- audio -----------------------------------------------------------------

struct AudioPacket {
    uint32_t seq = 0;
    uint64_t capture_us = 0;
    AudioCodec codec = AudioCodec::PcmS16;
    uint8_t channels = 2;
    uint16_t samples = 0;  // per channel
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
};

// ---- control / feedback ----------------------------------------------------

struct Ping {
    uint32_t seq = 0;
    uint64_t sender_us = 0;
};

struct Pong {
    uint32_t seq = 0;
    uint64_t echo_us = 0;   // Ping::sender_us
    uint64_t remote_us = 0; // responder clock at reply
};

struct ReceiverReport {
    uint32_t highest_frame = 0;
    uint32_t interval_us = 0;
    uint32_t packets_received = 0;
    uint32_t packets_lost = 0;
    uint32_t bytes_received = 0;
    uint32_t frames_complete = 0;
    uint32_t frames_dropped = 0;
    uint32_t frames_fec_recovered = 0;
    uint32_t rtt_us = 0;
    uint32_t jitter_us = 0;
    int32_t queue_delay_us = 0;  // current one-way delay above the windowed minimum
    uint16_t decode_queue = 0;
    uint16_t decode_p95_us = 0;
    uint16_t present_fps_x100 = 0;
    uint16_t audio_buffer_ms = 0;
};

struct KeyframeRequest {
    uint32_t last_good_frame = 0;
    uint8_t reason = 0;  // 1 = loss, 2 = decoder error, 3 = startup
};

// Host-side telemetry shipped to the client overlay once per second.
struct HostStats {
    uint16_t capture_fps_x100 = 0;
    uint16_t encode_fps_x100 = 0;
    uint32_t capture_drops = 0;
    uint32_t encode_drops = 0;
    uint32_t bitrate_kbps = 0;
    uint16_t width = 0, height = 0, fps = 0;
    uint16_t capture_to_send_p50 = 0, capture_to_send_p95 = 0, capture_to_send_p99 = 0;
    uint16_t encode_p50 = 0, encode_p95 = 0, encode_p99 = 0;
    // Controller sampled on the client -> received by the host (one way, us).
    int32_t input_owd_p50 = 0, input_owd_p95 = 0, input_owd_p99 = 0;
    uint16_t input_host_p50 = 0, input_host_p99 = 0;  // host rx -> serial write
    uint8_t esp_connected = 0;
    uint8_t esp_usb_mounted = 0;
    uint8_t esp_failsafe = 0;
    uint16_t esp_reports_per_s = 0;
    uint16_t esp_rx_to_usb_p50 = 0, esp_rx_to_usb_p99 = 0;
    uint16_t esp_rx_to_console_p50 = 0, esp_rx_to_console_p99 = 0;
    uint16_t esp_link_rtt_p50 = 0;
    uint16_t input_rate = 0;
    char encoder_name[48]{};
};

struct SetPreset {
    Preset preset = Preset::Balanced;
    uint32_t max_bitrate_kbps = 0;
};

// Sent ~4x per second while a client is connected (lobby and streaming).
struct HostStatus {
    ConsoleState console = ConsoleState::Unknown;
    SessionPhase phase = SessionPhase::Lobby;
    HostIssue issue = HostIssue::None;
    uint8_t bridge_ok = 0;
    uint8_t capture_ok = 0;
    uint8_t signal_ok = 0;
    uint8_t encoder_ok = 0;
    uint8_t wake_methods = 0;     // HK_WAKE_* the bridge can perform
    uint8_t remote_wakeup = 0;    // console enabled USB remote wakeup
    uint16_t phase_elapsed_ms = 0;
    uint16_t phase_progress = 0;  // 0..1000
    char host_name[32]{};
    char message[80]{};
};

struct Play {
    uint8_t wake = 1;  // wake the console if needed
};

enum class PowerAction : uint8_t { Wake = 1, Sleep = 2 };
struct Power {
    PowerAction action = PowerAction::Wake;
};

struct DiscoverReply {
    uint16_t version = kVersion;
    uint16_t port = kDefaultPort;
    char host_name[32]{};
    uint8_t host_id[8]{};  // stable id so the client can match saved pairings
    char tailscale_ipv4[16]{}; // optional v1 discovery extension; empty for LAN-only hosts
};

// ---- codecs --------------------------------------------------------------

void write(ByteWriter& w, const Hello& v);
bool read(ByteReader& r, Hello& v);
void write(ByteWriter& w, const Challenge& v);
bool read(ByteReader& r, Challenge& v);
void write(ByteWriter& w, const Auth& v);
bool read(ByteReader& r, Auth& v);
void write(ByteWriter& w, const StreamConfig& v);
bool read(ByteReader& r, StreamConfig& v);
void write(ByteWriter& w, const Welcome& v);
bool read(ByteReader& r, Welcome& v);
void write(ByteWriter& w, const Reject& v);
bool read(ByteReader& r, Reject& v);
void write(ByteWriter& w, const Input& v);
bool read(ByteReader& r, Input& v);
void write(ByteWriter& w, const AudioPacket& v);  // header fields only; append payload after
bool read(ByteReader& r, AudioPacket& v);
void write(ByteWriter& w, const Ping& v);
bool read(ByteReader& r, Ping& v);
void write(ByteWriter& w, const Pong& v);
bool read(ByteReader& r, Pong& v);
void write(ByteWriter& w, const ReceiverReport& v);
bool read(ByteReader& r, ReceiverReport& v);
void write(ByteWriter& w, const KeyframeRequest& v);
bool read(ByteReader& r, KeyframeRequest& v);
void write(ByteWriter& w, const HostStats& v);
bool read(ByteReader& r, HostStats& v);
void write(ByteWriter& w, const SetPreset& v);
bool read(ByteReader& r, SetPreset& v);
void write(ByteWriter& w, const HostStatus& v);
bool read(ByteReader& r, HostStatus& v);
void write(ByteWriter& w, const Play& v);
bool read(ByteReader& r, Play& v);
void write(ByteWriter& w, const Power& v);
bool read(ByteReader& r, Power& v);
void write(ByteWriter& w, const DiscoverReply& v);
bool read(ByteReader& r, DiscoverReply& v);

const char* to_string(PacketType t);
const char* to_string(Preset p);
const char* to_string(ConsoleState s);
const char* to_string(SessionPhase p);
const char* to_string(HostIssue i);

}  // namespace hikari::proto

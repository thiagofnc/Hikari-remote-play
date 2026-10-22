#include "hikari/net_protocol.h"

#include <cstring>

namespace hikari::proto {

void write_header(ByteWriter& w, PacketType type, uint32_t session)
{
    w.u8(kMagic);
    w.u8(static_cast<uint8_t>(type));
    w.u32(session);
}

bool read_header(ByteReader& r, Header& h)
{
    if (r.u8() != kMagic) return false;
    h.type = static_cast<PacketType>(r.u8());
    h.session = r.u32();
    return r.ok();
}

bool PadState::operator==(const PadState& o) const
{
    return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry && lt == o.lt &&
           rt == o.rt && has_imu == o.has_imu && std::memcmp(gyro, o.gyro, sizeof(gyro)) == 0 &&
           std::memcmp(accel, o.accel, sizeof(accel)) == 0;
}

void write(ByteWriter& w, const Hello& v)
{
    w.u16(v.version);
    w.bytes(v.client_nonce, kNonceSize);
    w.str(v.client_name, sizeof(v.client_name));
    w.u8(static_cast<uint8_t>(v.preset));
    w.u32(v.max_bitrate_kbps);
    w.u8(v.video_codecs);
    w.u8(v.audio_codecs);
    w.u16(v.display_hz);
}

bool read(ByteReader& r, Hello& v)
{
    v.version = r.u16();
    r.bytes(v.client_nonce, kNonceSize);
    char name[sizeof(v.client_name) + 1];
    r.str(name, sizeof(v.client_name));
    std::memcpy(v.client_name, name, sizeof(v.client_name));
    v.client_name[sizeof(v.client_name) - 1] = 0;
    v.preset = static_cast<Preset>(r.u8());
    v.max_bitrate_kbps = r.u32();
    v.video_codecs = r.u8();
    v.audio_codecs = r.u8();
    v.display_hz = r.u16();
    return r.ok();
}

void write(ByteWriter& w, const Challenge& v) { w.bytes(v.host_nonce, kNonceSize); }
bool read(ByteReader& r, Challenge& v)
{
    r.bytes(v.host_nonce, kNonceSize);
    return r.ok();
}

void write(ByteWriter& w, const Auth& v)
{
    w.bytes(v.client_nonce, kNonceSize);
    w.bytes(v.host_nonce, kNonceSize);
    w.bytes(v.mac, kMacSize);
}
bool read(ByteReader& r, Auth& v)
{
    r.bytes(v.client_nonce, kNonceSize);
    r.bytes(v.host_nonce, kNonceSize);
    r.bytes(v.mac, kMacSize);
    return r.ok();
}

void write(ByteWriter& w, const StreamConfig& v)
{
    w.u8(static_cast<uint8_t>(v.codec));
    w.u16(v.width);
    w.u16(v.height);
    w.u16(v.fps);
    w.u32(v.bitrate_kbps);
    w.u8(static_cast<uint8_t>(v.audio));
    w.u32(v.audio_rate);
    w.u8(v.audio_channels);
    w.u16(v.audio_frame_samples);
    w.u8(v.fec_group);
    w.str(v.encoder_name, sizeof(v.encoder_name));
}
bool read(ByteReader& r, StreamConfig& v)
{
    v.codec = static_cast<VideoCodec>(r.u8());
    v.width = r.u16();
    v.height = r.u16();
    v.fps = r.u16();
    v.bitrate_kbps = r.u32();
    v.audio = static_cast<AudioCodec>(r.u8());
    v.audio_rate = r.u32();
    v.audio_channels = r.u8();
    v.audio_frame_samples = r.u16();
    v.fec_group = r.u8();
    char name[sizeof(v.encoder_name) + 1];
    r.str(name, sizeof(v.encoder_name));
    std::memcpy(v.encoder_name, name, sizeof(v.encoder_name));
    v.encoder_name[sizeof(v.encoder_name) - 1] = 0;
    return r.ok();
}

void write(ByteWriter& w, const Welcome& v)
{
    w.u32(v.session_id);
    w.bytes(v.mac, kMacSize);
    write(w, v.config);
}
bool read(ByteReader& r, Welcome& v)
{
    v.session_id = r.u32();
    r.bytes(v.mac, kMacSize);
    return read(r, v.config);
}

void write(ByteWriter& w, const Reject& v) { w.u8(static_cast<uint8_t>(v.reason)); }
bool read(ByteReader& r, Reject& v)
{
    v.reason = static_cast<RejectReason>(r.u8());
    return r.ok();
}

void write(ByteWriter& w, const Input& v)
{
    w.u32(v.seq);
    w.u64(v.client_time_us);
    w.u8(static_cast<uint8_t>(v.flags | (v.pad.has_imu ? 0x80 : 0)));
    w.u32(v.pad.buttons);
    w.i16(v.pad.lx);
    w.i16(v.pad.ly);
    w.i16(v.pad.rx);
    w.i16(v.pad.ry);
    w.u8(v.pad.lt);
    w.u8(v.pad.rt);
    if (v.pad.has_imu) {
        for (auto g : v.pad.gyro) w.i16(g);
        for (auto a : v.pad.accel) w.i16(a);
    }
}
bool read(ByteReader& r, Input& v)
{
    v.seq = r.u32();
    v.client_time_us = r.u64();
    const uint8_t f = r.u8();
    v.flags = f & 0x7F;
    v.pad.has_imu = (f & 0x80) != 0;
    v.pad.buttons = r.u32();
    v.pad.lx = r.i16();
    v.pad.ly = r.i16();
    v.pad.rx = r.i16();
    v.pad.ry = r.i16();
    v.pad.lt = r.u8();
    v.pad.rt = r.u8();
    if (v.pad.has_imu) {
        for (auto& g : v.pad.gyro) g = r.i16();
        for (auto& a : v.pad.accel) a = r.i16();
    } else {
        std::memset(v.pad.gyro, 0, sizeof(v.pad.gyro));
        std::memset(v.pad.accel, 0, sizeof(v.pad.accel));
    }
    return r.ok();
}

void write_video_header(ByteWriter& w, const VideoFragment& f)
{
    w.u32(f.frame_id);
    w.u16(f.index);
    w.u16(f.data_count);
    w.u8(f.flags);
    w.u8(f.fec_group);
    w.u32(f.frame_size);
    w.u64(f.capture_us);
    w.u16(f.encode_us);
    w.u16(f.host_us);
}

bool read_video_header(ByteReader& r, VideoFragment& f)
{
    f.frame_id = r.u32();
    f.index = r.u16();
    f.data_count = r.u16();
    f.flags = r.u8();
    f.fec_group = r.u8();
    f.frame_size = r.u32();
    f.capture_us = r.u64();
    f.encode_us = r.u16();
    f.host_us = r.u16();
    if (!r.ok()) return false;
    f.payload = r.cursor();
    f.payload_len = r.remaining();
    return f.data_count > 0 && f.payload_len > 0 && f.payload_len <= kVideoPayload;
}

void write(ByteWriter& w, const AudioPacket& v)
{
    w.u32(v.seq);
    w.u64(v.capture_us);
    w.u8(static_cast<uint8_t>(v.codec));
    w.u8(v.channels);
    w.u16(v.samples);
}
bool read(ByteReader& r, AudioPacket& v)
{
    v.seq = r.u32();
    v.capture_us = r.u64();
    v.codec = static_cast<AudioCodec>(r.u8());
    v.channels = r.u8();
    v.samples = r.u16();
    if (!r.ok()) return false;
    v.payload = r.cursor();
    v.payload_len = r.remaining();
    return true;
}

void write(ByteWriter& w, const Ping& v)
{
    w.u32(v.seq);
    w.u64(v.sender_us);
}
bool read(ByteReader& r, Ping& v)
{
    v.seq = r.u32();
    v.sender_us = r.u64();
    return r.ok();
}

void write(ByteWriter& w, const Pong& v)
{
    w.u32(v.seq);
    w.u64(v.echo_us);
    w.u64(v.remote_us);
}
bool read(ByteReader& r, Pong& v)
{
    v.seq = r.u32();
    v.echo_us = r.u64();
    v.remote_us = r.u64();
    return r.ok();
}

void write(ByteWriter& w, const ReceiverReport& v)
{
    w.u32(v.highest_frame);
    w.u32(v.interval_us);
    w.u32(v.packets_received);
    w.u32(v.packets_lost);
    w.u32(v.bytes_received);
    w.u32(v.frames_complete);
    w.u32(v.frames_dropped);
    w.u32(v.frames_fec_recovered);
    w.u32(v.rtt_us);
    w.u32(v.jitter_us);
    w.i32(v.queue_delay_us);
    w.u16(v.decode_queue);
    w.u16(v.decode_p95_us);
    w.u16(v.present_fps_x100);
    w.u16(v.audio_buffer_ms);
}
bool read(ByteReader& r, ReceiverReport& v)
{
    v.highest_frame = r.u32();
    v.interval_us = r.u32();
    v.packets_received = r.u32();
    v.packets_lost = r.u32();
    v.bytes_received = r.u32();
    v.frames_complete = r.u32();
    v.frames_dropped = r.u32();
    v.frames_fec_recovered = r.u32();
    v.rtt_us = r.u32();
    v.jitter_us = r.u32();
    v.queue_delay_us = r.i32();
    v.decode_queue = r.u16();
    v.decode_p95_us = r.u16();
    v.present_fps_x100 = r.u16();
    v.audio_buffer_ms = r.u16();
    return r.ok();
}

void write(ByteWriter& w, const KeyframeRequest& v)
{
    w.u32(v.last_good_frame);
    w.u8(v.reason);
}
bool read(ByteReader& r, KeyframeRequest& v)
{
    v.last_good_frame = r.u32();
    v.reason = r.u8();
    return r.ok();
}

void write(ByteWriter& w, const HostStats& v)
{
    w.u16(v.capture_fps_x100);
    w.u16(v.encode_fps_x100);
    w.u32(v.capture_drops);
    w.u32(v.encode_drops);
    w.u32(v.bitrate_kbps);
    w.u16(v.width);
    w.u16(v.height);
    w.u16(v.fps);
    w.u16(v.capture_to_send_p50);
    w.u16(v.capture_to_send_p95);
    w.u16(v.capture_to_send_p99);
    w.u16(v.encode_p50);
    w.u16(v.encode_p95);
    w.u16(v.encode_p99);
    w.i32(v.input_owd_p50);
    w.i32(v.input_owd_p95);
    w.i32(v.input_owd_p99);
    w.u16(v.input_host_p50);
    w.u16(v.input_host_p99);
    w.u8(v.esp_connected);
    w.u8(v.esp_usb_mounted);
    w.u8(v.esp_failsafe);
    w.u16(v.esp_reports_per_s);
    w.u16(v.esp_rx_to_usb_p50);
    w.u16(v.esp_rx_to_usb_p99);
    w.u16(v.esp_rx_to_console_p50);
    w.u16(v.esp_rx_to_console_p99);
    w.u16(v.esp_link_rtt_p50);
    w.u16(v.input_rate);
    w.str(v.encoder_name, sizeof(v.encoder_name));
}
bool read(ByteReader& r, HostStats& v)
{
    v.capture_fps_x100 = r.u16();
    v.encode_fps_x100 = r.u16();
    v.capture_drops = r.u32();
    v.encode_drops = r.u32();
    v.bitrate_kbps = r.u32();
    v.width = r.u16();
    v.height = r.u16();
    v.fps = r.u16();
    v.capture_to_send_p50 = r.u16();
    v.capture_to_send_p95 = r.u16();
    v.capture_to_send_p99 = r.u16();
    v.encode_p50 = r.u16();
    v.encode_p95 = r.u16();
    v.encode_p99 = r.u16();
    v.input_owd_p50 = r.i32();
    v.input_owd_p95 = r.i32();
    v.input_owd_p99 = r.i32();
    v.input_host_p50 = r.u16();
    v.input_host_p99 = r.u16();
    v.esp_connected = r.u8();
    v.esp_usb_mounted = r.u8();
    v.esp_failsafe = r.u8();
    v.esp_reports_per_s = r.u16();
    v.esp_rx_to_usb_p50 = r.u16();
    v.esp_rx_to_usb_p99 = r.u16();
    v.esp_rx_to_console_p50 = r.u16();
    v.esp_rx_to_console_p99 = r.u16();
    v.esp_link_rtt_p50 = r.u16();
    v.input_rate = r.u16();
    char name[sizeof(v.encoder_name) + 1];
    r.str(name, sizeof(v.encoder_name));
    std::memcpy(v.encoder_name, name, sizeof(v.encoder_name));
    v.encoder_name[sizeof(v.encoder_name) - 1] = 0;
    return r.ok();
}

void write(ByteWriter& w, const SetPreset& v)
{
    w.u8(static_cast<uint8_t>(v.preset));
    w.u32(v.max_bitrate_kbps);
}
bool read(ByteReader& r, SetPreset& v)
{
    v.preset = static_cast<Preset>(r.u8());
    v.max_bitrate_kbps = r.u32();
    return r.ok() && static_cast<uint8_t>(v.preset) <= 2;
}

namespace {
template <size_t N>
void read_str(ByteReader& r, char (&dst)[N])
{
    char tmp[N + 1];
    r.str(tmp, N);
    std::memcpy(dst, tmp, N);
    dst[N - 1] = 0;
}
}  // namespace

void write(ByteWriter& w, const HostStatus& v)
{
    w.u8(static_cast<uint8_t>(v.console));
    w.u8(static_cast<uint8_t>(v.phase));
    w.u8(static_cast<uint8_t>(v.issue));
    w.u8(v.bridge_ok);
    w.u8(v.capture_ok);
    w.u8(v.signal_ok);
    w.u8(v.encoder_ok);
    w.u8(v.wake_methods);
    w.u8(v.remote_wakeup);
    w.u16(v.phase_elapsed_ms);
    w.u16(v.phase_progress);
    w.str(v.host_name, sizeof(v.host_name));
    w.str(v.message, sizeof(v.message));
}
bool read(ByteReader& r, HostStatus& v)
{
    v.console = static_cast<ConsoleState>(r.u8());
    v.phase = static_cast<SessionPhase>(r.u8());
    v.issue = static_cast<HostIssue>(r.u8());
    v.bridge_ok = r.u8();
    v.capture_ok = r.u8();
    v.signal_ok = r.u8();
    v.encoder_ok = r.u8();
    v.wake_methods = r.u8();
    v.remote_wakeup = r.u8();
    v.phase_elapsed_ms = r.u16();
    v.phase_progress = r.u16();
    read_str(r, v.host_name);
    read_str(r, v.message);
    return r.ok();
}

void write(ByteWriter& w, const Play& v) { w.u8(v.wake); }
bool read(ByteReader& r, Play& v)
{
    v.wake = r.u8();
    return r.ok();
}

void write(ByteWriter& w, const Power& v) { w.u8(static_cast<uint8_t>(v.action)); }
bool read(ByteReader& r, Power& v)
{
    v.action = static_cast<PowerAction>(r.u8());
    return r.ok() && (v.action == PowerAction::Wake || v.action == PowerAction::Sleep);
}

void write(ByteWriter& w, const DiscoverReply& v)
{
    w.u16(v.version);
    w.u16(v.port);
    w.str(v.host_name, sizeof(v.host_name));
    w.bytes(v.host_id, sizeof(v.host_id));
}
bool read(ByteReader& r, DiscoverReply& v)
{
    v.version = r.u16();
    v.port = r.u16();
    read_str(r, v.host_name);
    r.bytes(v.host_id, sizeof(v.host_id));
    return r.ok();
}

const char* to_string(ConsoleState s)
{
    switch (s) {
    case ConsoleState::Unknown: return "Unknown";
    case ConsoleState::Off: return "Off";
    case ConsoleState::Asleep: return "Asleep";
    case ConsoleState::Waking: return "Waking";
    case ConsoleState::AwakeNoVideo: return "Awake (no picture)";
    case ConsoleState::Ready: return "Ready";
    }
    return "?";
}

const char* to_string(SessionPhase p)
{
    switch (p) {
    case SessionPhase::Lobby: return "Ready to play";
    case SessionPhase::Waking: return "Waking your Switch";
    case SessionPhase::WaitingForVideo: return "Waiting for picture";
    case SessionPhase::WaitingForController: return "Connecting controller";
    case SessionPhase::StartingStream: return "Starting stream";
    case SessionPhase::Streaming: return "Streaming";
    case SessionPhase::Failed: return "Could not start";
    }
    return "?";
}

const char* to_string(HostIssue i)
{
    switch (i) {
    case HostIssue::None: return "";
    case HostIssue::NoBridge: return "Controller bridge (ESP32) not connected to the host PC";
    case HostIssue::NoCapture: return "Capture card not found on the host PC";
    case HostIssue::NoSignal: return "Capture card has no HDMI signal";
    case HostIssue::NoEncoder: return "No usable hardware video encoder";
    case HostIssue::WakeFailed: return "The Switch did not wake up";
    case HostIssue::ControllerNotAccepted: return "The Switch has not accepted the controller";
    }
    return "?";
}

const char* to_string(PacketType t)
{
    switch (t) {
    case PacketType::Hello: return "Hello";
    case PacketType::Challenge: return "Challenge";
    case PacketType::Auth: return "Auth";
    case PacketType::Welcome: return "Welcome";
    case PacketType::Reject: return "Reject";
    case PacketType::Bye: return "Bye";
    case PacketType::Input: return "Input";
    case PacketType::Video: return "Video";
    case PacketType::VideoParity: return "VideoParity";
    case PacketType::StreamConfig: return "StreamConfig";
    case PacketType::Audio: return "Audio";
    case PacketType::Ping: return "Ping";
    case PacketType::Pong: return "Pong";
    case PacketType::ReceiverReport: return "ReceiverReport";
    case PacketType::KeyframeRequest: return "KeyframeRequest";
    case PacketType::HostStats: return "HostStats";
    case PacketType::SetPreset: return "SetPreset";
    case PacketType::LatencyProbe: return "LatencyProbe";
    case PacketType::HostStatus: return "HostStatus";
    case PacketType::Play: return "Play";
    case PacketType::Stop: return "Stop";
    case PacketType::Power: return "Power";
    case PacketType::Discover: return "Discover";
    case PacketType::DiscoverReply: return "DiscoverReply";
    }
    return "?";
}

const char* to_string(Preset p)
{
    switch (p) {
    case Preset::MinimumLatency: return "Minimum Latency";
    case Preset::Balanced: return "Balanced";
    case Preset::Quality: return "Quality";
    }
    return "?";
}

}  // namespace hikari::proto

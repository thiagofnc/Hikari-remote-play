#pragma once
// HDMI audio capture (WASAPI, event driven) with optional Opus encoding.
//
// The capture card's audio endpoint is found automatically by matching its
// PnP container ID with the video device (same physical USB device), falling
// back to name similarity. Audio is cut into 5 ms packets: small enough that
// audio never waits on packetization, large enough to keep packet rate sane.

#include <atomic>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "hikari/net_protocol.h"

namespace hikari {

struct AudioEndpointInfo {
    std::wstring id;
    std::string name;
    GUID container{};
};

std::vector<AudioEndpointInfo> enumerate_audio_capture_endpoints();
// Container ID of a video capture device (from its MF symbolic link).
bool container_id_for_symlink(const std::wstring& symlink, GUID& out);
// Returns the index of the endpoint belonging to the capture card, or -1.
int match_audio_endpoint(const std::vector<AudioEndpointInfo>& eps, const std::wstring& video_symlink,
                         const std::string& video_name);

struct AudioChunk {
    const uint8_t* data;
    size_t bytes;
    uint16_t samples;  // per channel
    uint64_t capture_us;
    proto::AudioCodec codec;
};

class AudioCapture {
public:
    using ChunkCallback = std::function<void(const AudioChunk&)>;

    ~AudioCapture();
    bool start(const AudioEndpointInfo& ep, bool use_opus, ChunkCallback cb);
    void stop();
    bool running() const { return running_.load(); }
    bool failed() const { return failed_.load(); }
    proto::AudioCodec codec() const { return codec_; }
    const std::string& name() const { return name_; }

    static constexpr uint32_t kRate = 48000;
    static constexpr uint32_t kChannels = 2;
    static constexpr uint32_t kChunk = 240;  // 5 ms

private:
    void run(std::wstring id);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    ChunkCallback cb_;
    proto::AudioCodec codec_ = proto::AudioCodec::PcmS16;
    bool want_opus_ = false;
    std::string name_;
};

}  // namespace hikari

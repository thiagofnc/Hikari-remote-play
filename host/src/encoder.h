#pragma once
// Hardware video encoding through Media Foundation transforms.
//
// The vendor encoders (Intel Quick Sync, NVIDIA NVENC, AMD AMF/VCN) all ship
// as hardware MFTs, so one code path drives all of them with GPU-resident
// NV12 input. Which one is fastest is *measured* (benchmark_encoder) on first
// start and cached, never assumed from the GPU vendor.
//
// Low-latency policy: CBR with a ~1-frame VBV, no B-frames, low-latency mode
// (no lookahead / no frame reordering), infinite GOP with IDR on demand
// (keyframes only when the client actually lost decoder state).

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <strmif.h>

#include "hikari/histogram.h"
#include "hikari/media.h"

namespace hikari {

struct EncoderInfo {
    std::string name;
    std::wstring clsid_str;
    GUID codec{};
    bool hardware = false;
    bool async = false;
    UINT vendor_id = 0;
    LUID adapter_luid{};
    ComPtr<IMFActivate> activate;
};

std::vector<EncoderInfo> enumerate_encoders(const GUID& codec, bool include_software = false);

struct EncoderConfig {
    GUID codec = MFVideoFormat_H264;
    UINT width = 1920, height = 1080, fps = 60;
    UINT bitrate_kbps = 12000;
    UINT gop = 0;                // 0 = infinite (IDR on demand)
    UINT quality_vs_speed = 33;  // 0 = fastest, 100 = best quality
    double vbv_frames = 1.0;     // rate-control buffer in frames
};

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint32_t frame_index = 0;
    uint64_t capture_us = 0;
    uint64_t submit_us = 0;
    uint64_t done_us = 0;
};

class VideoEncoder {
public:
    using OutputCallback = std::function<void(EncodedFrame&&)>;

    VideoEncoder();
    ~VideoEncoder();

    bool open(GpuDevice& gpu, const EncoderInfo& info, const EncoderConfig& cfg, OutputCallback cb);
    void close();
    bool is_open() const { return transform_ != nullptr; }

    // True when the MFT is ready for another input frame.
    bool can_accept() const;
    bool submit(ID3D11Texture2D* nv12, uint32_t frame_index, uint64_t capture_us);

    // Processes pending MFT events/outputs. Signals `event()` when work exists.
    void pump();
    HANDLE event() const { return wake_event_; }

    void request_keyframe() { force_idr_.store(true); }
    bool set_bitrate(UINT kbps);
    UINT bitrate() const { return cfg_.bitrate_kbps; }
    const EncoderConfig& config() const { return cfg_; }
    const std::string& name() const { return name_; }
    bool failed() const { return failed_.load(); }

    LatencySummary latency();  // submit -> output

    // Internal: async MFT event sink.
    void on_event(IMFMediaEvent* ev);

private:
    bool configure();
    void drain_outputs();
    bool process_output();
    void set_u32(const GUID& api, UINT32 v, const char* name, bool log = true);
    void set_bool(const GUID& api, bool v, const char* name);

    GpuDevice* gpu_ = nullptr;
    EncoderConfig cfg_;
    std::string name_;
    bool async_ = false;
    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codec_api_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<IUnknown> event_cb_;
    DWORD in_id_ = 0, out_id_ = 0;
    bool provides_samples_ = true;
    DWORD out_buffer_size_ = 0;
    std::vector<uint8_t> sequence_header_;
    OutputCallback cb_;

    HANDLE wake_event_ = nullptr;
    std::mutex ev_mu_;
    int pending_have_output_ = 0;
    std::atomic<int> need_input_{0};
    std::atomic<bool> force_idr_{true};
    std::atomic<bool> failed_{false};

    struct Pending {
        uint64_t sample_time;
        uint32_t frame_index;
        uint64_t capture_us;
        uint64_t submit_us;
    };
    std::deque<Pending> in_flight_;
    std::mutex flight_mu_;
    RollingHistogram latency_;
};

struct EncoderBenchmark {
    std::string name;
    bool ok = false;
    double fps = 0;
    LatencySummary latency;
    size_t avg_frame_bytes = 0;
};

// Encodes `frames` synthetic 1080p frames as fast as the real pipeline would
// feed them (paced at cfg.fps) and measures submit->output latency.
EncoderBenchmark benchmark_encoder(GpuDevice& gpu, const EncoderInfo& info, const EncoderConfig& cfg,
                                   int frames = 180, bool paced = true);

// Benchmarks all hardware encoders for `codec` and returns the index of the
// best one (lowest p95 latency that sustains the frame rate), or -1.
int select_best_encoder(GpuDevice& gpu, std::vector<EncoderInfo>& encoders, const EncoderConfig& cfg,
                        std::vector<EncoderBenchmark>* results = nullptr);

}  // namespace hikari

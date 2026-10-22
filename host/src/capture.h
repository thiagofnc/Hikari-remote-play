#pragma once
// HDMI capture via Media Foundation (async Source Reader).
//
// Design notes (measured choices live in docs/latency-budget.md):
//  * MF_LOW_LATENCY + async reader: the callback fires as soon as the
//    driver delivers a frame; we immediately request the next one.
//  * Uncompressed formats (NV12 > YUY2) are preferred over MJPEG, which
//    costs a decode stage. The capture_probe tool benchmarks this per card.
//  * System-memory frames are uploaded to the GPU once and never come back.
//  * Signal detection samples a sparse luma grid, used to tell the console's
//    "awake with picture" state apart from "asleep / no HDMI".

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "hikari/histogram.h"
#include "hikari/media.h"

struct IMFSourceReader;

namespace hikari {

struct CaptureMode {
    GUID subtype{};
    UINT32 width = 0, height = 0;
    UINT32 fps_num = 0, fps_den = 1;
    DWORD native_index = 0;
    double fps() const { return fps_den ? double(fps_num) / fps_den : 0.0; }
    std::string to_string() const;
};

struct CaptureDeviceInfo {
    std::string name;
    std::wstring symlink;
    std::vector<CaptureMode> modes;
};

std::vector<CaptureDeviceInfo> enumerate_capture_devices(bool with_modes = true);

// Picks the best mode for remote play: target resolution/fps, uncompressed
// preferred. `format_preference` (e.g. "NV12,YUY2,MJPG") comes from config or
// from capture_probe measurements.
bool choose_capture_mode(const CaptureDeviceInfo& dev, UINT32 want_w, UINT32 want_h, double want_fps,
                         const std::string& format_preference, CaptureMode& out);

// Heuristic: is this probably an HDMI capture card rather than a webcam?
int capture_device_score(const CaptureDeviceInfo& dev);

struct CapturedFrame {
    ComPtr<ID3D11Texture2D> texture;  // frame in capture format (NV12/YUY2/BGRA)
    UINT subresource = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
    uint64_t capture_us = 0;   // best estimate of when the frame left the card
    uint64_t callback_us = 0;  // when our callback ran
    uint32_t index = 0;
};

struct CaptureStats {
    double fps = 0;
    LatencySummary interval;   // frame-to-frame (jitter view)
    LatencySummary driver_us;  // device timestamp -> callback, when available
    uint64_t frames = 0;
    uint64_t errors = 0;
    bool signal = false;
    float mean_luma = 0;
};

class VideoCapture {
public:
    using FrameCallback = std::function<void(CapturedFrame&&)>;

    VideoCapture();
    ~VideoCapture();

    bool open(GpuDevice& gpu, const CaptureDeviceInfo& dev, const CaptureMode& mode);
    void close();
    bool is_open() const;

    // When `upload` is false, frames are only inspected for signal detection
    // (lobby mode: no GPU work while nobody is watching).
    void set_frame_callback(FrameCallback cb);
    void set_upload(bool upload) { upload_.store(upload); }

    CaptureStats stats();
    bool failed() const { return failed_.load(); }
    bool signal_present() const { return signal_.load(); }
    const CaptureMode& mode() const { return mode_; }
    const std::string& device_name() const { return device_name_; }

    // Called by the reader callback (internal).
    void on_sample(HRESULT hr, DWORD flags, LONGLONG timestamp, IMFSample* sample);

private:
    bool upload_frame(IMFMediaBuffer* buffer, CapturedFrame& out);
    void inspect_signal(const uint8_t* data, LONG pitch, bool yuy2);
    void inspect_signal_gpu(ID3D11Texture2D* tex, UINT sub);
    void request_next();

    GpuDevice* gpu_ = nullptr;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IUnknown> callback_;
    CaptureMode mode_;
    GUID output_subtype_{};
    DXGI_FORMAT upload_format_ = DXGI_FORMAT_UNKNOWN;
    std::string device_name_;

    static constexpr int kUploadRing = 4;
    ComPtr<ID3D11Texture2D> upload_ring_[kUploadRing];
    int upload_next_ = 0;
    ComPtr<ID3D11Texture2D> probe_staging_;

    std::mutex cb_mu_;
    FrameCallback callback_fn_;
    std::atomic<bool> upload_{true};
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> signal_{false};
    std::atomic<float> luma_{0};
    int signal_votes_ = 0;

    RateCounter fps_;
    RollingHistogram interval_hist_;
    RollingHistogram driver_hist_;
    uint64_t last_frame_us_ = 0;
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> errors_{0};
    uint32_t frame_index_ = 0;
};

}  // namespace hikari

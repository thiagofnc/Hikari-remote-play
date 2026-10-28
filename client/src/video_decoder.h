#pragma once
// Hardware video decoding (Media Foundation decoder MFT + DXVA on D3D11).
//
// CODECAPI_AVLowLatencyMode is the single most important setting here: it
// stops the Microsoft H.264/HEVC decoders from holding frames back for
// reordering, so each access unit comes out as soon as it is decoded.
// Decoded surfaces are copied (GPU->GPU) into our own NV12 texture so the
// decoder's surface pool is never starved by the renderer.

#include <string>

#include "hikari/histogram.h"
#include "hikari/media.h"
#include "hikari/net_protocol.h"

namespace hikari {

struct DecodedFrame {
    ID3D11Texture2D* texture = nullptr;  // NV12, owned by the decoder (valid until next decode)
    UINT width = 0, height = 0;
    uint64_t decode_us = 0;
};

// True when HEVC can be decoded on this GPU in hardware: a Media Foundation
// HEVC decoder is installed (HEVC Video Extensions) and the GPU decodes HEVC
// Main itself. Software HEVC at 1080p60 would cost more latency than it saves.
bool hevc_decode_supported(GpuDevice& gpu);

class VideoDecoder {
public:
    ~VideoDecoder();

    bool open(GpuDevice& gpu, proto::VideoCodec codec);
    void close();
    bool is_open() const { return transform_ != nullptr; }
    proto::VideoCodec codec() const { return codec_; }
    bool hardware() const { return hardware_; }
    const std::string& name() const { return name_; }

    // Feeds one access unit. Returns true and fills `out` if a picture came out.
    bool decode(const uint8_t* data, size_t len, uint64_t timestamp_us, DecodedFrame& out);
    bool failed() const { return failed_; }
    void flush();

    LatencySummary latency(uint64_t now) { return latency_.summary(now); }

private:
    bool configure_output();
    bool pull_output(DecodedFrame& out);
    bool copy_to_output(IMFSample* sample, DecodedFrame& out);

    GpuDevice* gpu_ = nullptr;
    proto::VideoCodec codec_ = proto::VideoCodec::H264;
    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> transform_;
    bool d3d_ = false;
    bool hardware_ = false;
    bool provides_samples_ = true;
    DWORD out_size_ = 0;
    UINT width_ = 0, height_ = 0;
    ComPtr<ID3D11Texture2D> output_;
    std::string name_;
    bool failed_ = false;
    RollingHistogram latency_{2'000'000};
};

}  // namespace hikari

#pragma once
// Colour conversion + scaling on the GPU (D3D11 video processor).
// Any capture format (YUY2/NV12/BGRA) at any size -> NV12 at the encode size,
// in one fixed-function pass that typically costs well under 1 ms.

#include <map>

#include "hikari/media.h"

namespace hikari {

class GpuConverter {
public:
    bool init(GpuDevice& gpu, UINT out_w, UINT out_h, UINT fps);
    void reset();

    // Converts `src` into the next NV12 texture of the output ring. The
    // returned texture stays valid until the ring wraps (kRing frames).
    ID3D11Texture2D* convert(ID3D11Texture2D* src, UINT subresource, UINT src_w, UINT src_h);

    UINT width() const { return out_w_; }
    UINT height() const { return out_h_; }
    static constexpr int kRing = 6;

private:
    bool ensure_processor(UINT src_w, UINT src_h, DXGI_FORMAT fmt);
    ID3D11VideoProcessorInputView* bounce_view(ID3D11Texture2D* src, UINT sub, const D3D11_TEXTURE2D_DESC& d);

    GpuDevice* gpu_ = nullptr;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> venum_;
    ComPtr<ID3D11VideoProcessor> vproc_;
    UINT in_w_ = 0, in_h_ = 0, out_w_ = 0, out_h_ = 0, fps_ = 60;
    DXGI_FORMAT in_fmt_ = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11Texture2D> ring_[kRing];
    ComPtr<ID3D11VideoProcessorOutputView> ring_views_[kRing];
    int next_ = 0;

    struct InKey {
        ID3D11Texture2D* tex;
        UINT sub;
        bool operator<(const InKey& o) const { return tex != o.tex ? tex < o.tex : sub < o.sub; }
    };
    std::map<InKey, ComPtr<ID3D11VideoProcessorInputView>> in_views_;

    // Some drivers (Intel) refuse input views on textures they can't sample
    // from the video engine, e.g. YUY2 created with only BIND_SHADER_RESOURCE.
    // Those frames are copied into a texture the processor accepts first.
    bool use_bounce_ = false;
    bool bounce_failed_ = false;
    ComPtr<ID3D11Texture2D> bounce_;
    ComPtr<ID3D11VideoProcessorInputView> bounce_in_view_;
};

}  // namespace hikari

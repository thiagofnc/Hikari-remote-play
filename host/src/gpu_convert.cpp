#include "gpu_convert.h"

#include "hikari/log.h"
#include "hikari/system.h"

namespace hikari {

bool GpuConverter::init(GpuDevice& gpu, UINT out_w, UINT out_h, UINT fps)
{
    reset();
    gpu_ = &gpu;
    out_w_ = out_w;
    out_h_ = out_h;
    fps_ = fps;
    HK_CHECK_HR(gpu.device.As(&vdev_), "ID3D11VideoDevice");
    HK_CHECK_HR(gpu.context.As(&vctx_), "ID3D11VideoContext");

    for (int i = 0; i < kRing; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = out_w;
        d.Height = out_h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        // RENDER_TARGET for the video processor; SHADER_RESOURCE because
        // several encoder MFTs bind their input as an SRV internally.
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HK_CHECK_HR(gpu.device->CreateTexture2D(&d, nullptr, &ring_[i]), "CreateTexture2D(NV12 ring)");
    }
    return true;
}

void GpuConverter::reset()
{
    for (auto& v : ring_views_) v.Reset();
    for (auto& t : ring_) t.Reset();
    in_views_.clear();
    vproc_.Reset();
    venum_.Reset();
    vctx_.Reset();
    vdev_.Reset();
    in_w_ = in_h_ = 0;
    in_fmt_ = DXGI_FORMAT_UNKNOWN;
    next_ = 0;
}

bool GpuConverter::ensure_processor(UINT src_w, UINT src_h, DXGI_FORMAT fmt)
{
    if (vproc_ && src_w == in_w_ && src_h == in_h_ && fmt == in_fmt_) return true;
    vproc_.Reset();
    venum_.Reset();
    in_views_.clear();
    for (auto& v : ring_views_) v.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {fps_, 1};
    cd.InputWidth = src_w;
    cd.InputHeight = src_h;
    cd.OutputFrameRate = {fps_, 1};
    cd.OutputWidth = out_w_;
    cd.OutputHeight = out_h_;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HK_CHECK_HR(vdev_->CreateVideoProcessorEnumerator(&cd, &venum_), "CreateVideoProcessorEnumerator");
    HK_CHECK_HR(vdev_->CreateVideoProcessor(venum_.Get(), 0, &vproc_), "CreateVideoProcessor");

    // Fixed-function path only: no denoise/edge enhancement/auto processing,
    // which some drivers enable by default and which cost time and add blur.
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vproc_.Get(), 0, FALSE);
    vctx_->VideoProcessorSetStreamFrameFormat(vproc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamOutputRate(vproc_.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE,
                                             nullptr);

    // HDMI from the Switch is BT.709 limited range for YUV; RGB captures are full range.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.YCbCr_Matrix = 1;  // BT.709
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    in_cs.RGB_Range = fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? 0 : 1;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.YCbCr_Matrix = 1;
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx_->VideoProcessorSetStreamColorSpace(vproc_.Get(), 0, &in_cs);
    vctx_->VideoProcessorSetOutputColorSpace(vproc_.Get(), &out_cs);

    RECT src_rect{0, 0, static_cast<LONG>(src_w), static_cast<LONG>(src_h)};
    RECT dst_rect{0, 0, static_cast<LONG>(out_w_), static_cast<LONG>(out_h_)};
    vctx_->VideoProcessorSetStreamSourceRect(vproc_.Get(), 0, TRUE, &src_rect);
    vctx_->VideoProcessorSetStreamDestRect(vproc_.Get(), 0, TRUE, &dst_rect);
    vctx_->VideoProcessorSetOutputTargetRect(vproc_.Get(), TRUE, &dst_rect);

    for (int i = 0; i < kRing; ++i) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        HK_CHECK_HR(vdev_->CreateVideoProcessorOutputView(ring_[i].Get(), venum_.Get(), &od, &ring_views_[i]),
                    "CreateVideoProcessorOutputView");
    }
    in_w_ = src_w;
    in_h_ = src_h;
    in_fmt_ = fmt;
    HK_LOGI("GPU converter: %ux%u -> %ux%u NV12", src_w, src_h, out_w_, out_h_);
    return true;
}

ID3D11Texture2D* GpuConverter::convert(ID3D11Texture2D* src, UINT sub, UINT src_w, UINT src_h)
{
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);
    if (!ensure_processor(src_w, src_h, d.Format)) return nullptr;

    const InKey key{src, sub};
    auto it = in_views_.find(key);
    if (it == in_views_.end()) {
        if (in_views_.size() > 64) in_views_.clear();  // capture pools rotate; keep the cache bounded
        ComPtr<ID3D11VideoProcessorInputView> v;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        id.Texture2D.ArraySlice = sub;
        if (FAILED(vdev_->CreateVideoProcessorInputView(src, venum_.Get(), &id, &v))) {
            HK_LOGE("CreateVideoProcessorInputView failed (format %d)", d.Format);
            return nullptr;
        }
        it = in_views_.emplace(key, v).first;
    }
    auto* in_view = it->second.Get();

    const int i = next_;
    next_ = (next_ + 1) % kRing;
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in_view;
    const HRESULT hr = vctx_->VideoProcessorBlt(vproc_.Get(), ring_views_[i].Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        HK_LOGE("VideoProcessorBlt failed: %s", hresult_string(hr).c_str());
        return nullptr;
    }
    return ring_[i].Get();
}

}  // namespace hikari

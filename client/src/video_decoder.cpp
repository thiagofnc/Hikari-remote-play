#include "video_decoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <strmif.h>

#include <cstring>

#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/system.h"

namespace hikari {

namespace {
// D3D11_DECODER_PROFILE_HEVC_VLD_MAIN (not declared by every SDK).
const GUID kHevcVldMain = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
}  // namespace

bool hevc_decode_supported(GpuDevice& gpu)
{
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_HEVC};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts,
                         &n)))
        n = 0;
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (n == 0) return false;

    ComPtr<ID3D11VideoDevice> vdev;
    if (!gpu.device || FAILED(gpu.device.As(&vdev))) return false;
    BOOL ok = FALSE;
    return SUCCEEDED(vdev->CheckVideoDecoderFormat(&kHevcVldMain, DXGI_FORMAT_NV12, &ok)) && ok;
}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close()
{
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
    transform_.Reset();
    if (activate_) activate_->ShutdownObject();
    activate_.Reset();
    output_.Reset();
    width_ = height_ = 0;
    failed_ = false;
}

bool VideoDecoder::open(GpuDevice& gpu, proto::VideoCodec codec, UINT width, UINT height)
{
    close();
    gpu_ = &gpu;
    codec_ = codec;
    const GUID sub = codec == proto::VideoCodec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;

    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, sub};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    // Synchronous MFTs only: the Microsoft decoders are sync and use DXVA
    // internally once given a D3D manager, which keeps this path simple.
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                         &acts, &n)) ||
        n == 0) {
        HK_LOGE("No %s decoder available", codec == proto::VideoCodec::HEVC ? "HEVC" : "H.264");
        CoTaskMemFree(acts);
        return false;
    }
    activate_.Attach(acts[0]);
    for (UINT32 i = 1; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    name_ = mf_attribute_string(activate_.Get(), MFT_FRIENDLY_NAME_Attribute);
    HK_CHECK_HR(activate_->ActivateObject(IID_PPV_ARGS(&transform_)), "ActivateObject(decoder)");

    ComPtr<IMFAttributes> attrs;
    d3d_ = false;
    if (SUCCEEDED(transform_->GetAttributes(&attrs)) && attrs) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 aware = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware) {
            if (SUCCEEDED(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                     reinterpret_cast<ULONG_PTR>(gpu.manager.Get()))))
                d3d_ = true;
        }
    }
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(transform_.As(&api))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        if (FAILED(api->SetValue(&CODECAPI_AVLowLatencyMode, &v))) HK_LOGW("Decoder ignores low-latency mode");
    }
    hardware_ = d3d_;

    ComPtr<IMFMediaType> it;
    MFCreateMediaType(&it);
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, sub);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, 60, 1);
    // The HEVC decoder crashes in BEGIN_STREAMING without a frame size.
    MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, width, height);
    HK_CHECK_HR(transform_->SetInputType(0, it.Get(), 0), "Decoder SetInputType");
    if (!configure_output()) return false;
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    HK_LOGI("Decoder: %s (%s)", name_.c_str(), d3d_ ? "GPU" : "software");
    return true;
}

bool VideoDecoder::configure_output()
{
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(transform_->GetOutputAvailableType(0, i, &t))) break;
        GUID s{};
        t->GetGUID(MF_MT_SUBTYPE, &s);
        if (s != MFVideoFormat_NV12) continue;
        if (FAILED(transform_->SetOutputType(0, t.Get(), 0))) continue;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &width_, &height_);
        // H.264 codes 1080p as 1088 lines; the display aperture has the real size.
        MFVideoArea area{};
        if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                 nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            width_ = static_cast<UINT>(area.Area.cx);
            height_ = static_cast<UINT>(area.Area.cy);
        }
        MFT_OUTPUT_STREAM_INFO info{};
        transform_->GetOutputStreamInfo(0, &info);
        provides_samples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        out_size_ = info.cbSize;
        return true;
    }
    // Before the first SPS the decoder may not offer output types yet.
    return true;
}

void VideoDecoder::flush()
{
    if (transform_) transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}

bool VideoDecoder::decode(const uint8_t* data, size_t len, uint64_t ts, DecodedFrame& out)
{
    if (!transform_) return false;
    const uint64_t start = now_us();
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(len), &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    std::memcpy(p, data, len);
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(len));
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    s->SetSampleTime(static_cast<LONGLONG>(ts) * 10);
    s->SetSampleDuration(166666);

    HRESULT hr = transform_->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // Drain what is pending, then retry once.
        DecodedFrame tmp;
        pull_output(tmp);
        hr = transform_->ProcessInput(0, s.Get(), 0);
    }
    if (FAILED(hr)) {
        HK_LOGW("Decoder rejected frame: %s", hresult_string(hr).c_str());
        failed_ = true;
        return false;
    }
    const bool got = pull_output(out);
    if (got) {
        out.decode_us = now_us() - start;
        latency_.record(out.decode_us, now_us());
    }
    return got;
}

bool VideoDecoder::pull_output(DecodedFrame& out)
{
    bool got = false;
    for (int guard = 0; guard < 4; ++guard) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> own;
        if (!provides_samples_) {
            ComPtr<IMFMediaBuffer> mb;
            MFCreateMemoryBuffer(out_size_ ? out_size_ : width_ * height_ * 3 / 2, &mb);
            MFCreateSample(&own);
            own->AddBuffer(mb.Get());
            ob.pSample = own.Get();
        }
        DWORD status = 0;
        const HRESULT hr = transform_->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            configure_output();
            output_.Reset();
            continue;
        }
        if (FAILED(hr)) {
            failed_ = true;
            break;
        }
        ComPtr<IMFSample> sample;
        if (provides_samples_)
            sample.Attach(ob.pSample);
        else
            sample = own;
        // Keep only the newest picture if the decoder released several.
        if (sample && copy_to_output(sample.Get(), out)) got = true;
    }
    return got;
}

bool VideoDecoder::copy_to_output(IMFSample* sample, DecodedFrame& out)
{
    if (!width_ || !height_) return false;
    if (!output_) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width_;
        d.Height = height_;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(gpu_->device->CreateTexture2D(&d, nullptr, &output_))) return false;
    }
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
    ComPtr<IMFDXGIBuffer> dx;
    if (SUCCEEDED(buf.As(&dx))) {
        ComPtr<ID3D11Texture2D> tex;
        UINT sub = 0;
        if (FAILED(dx->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dx->GetSubresourceIndex(&sub))) return false;
        // Coded size can exceed display size (1088 vs 1080): copy the visible part.
        D3D11_BOX box{0, 0, 0, width_ & ~1u, height_ & ~1u, 1};
        gpu_->context->CopySubresourceRegion(output_.Get(), 0, 0, 0, 0, tex.Get(), sub, &box);
    } else {
        // Software decode fallback: one upload.
        BYTE* p = nullptr;
        LONG pitch = 0;
        ComPtr<IMF2DBuffer> b2;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
            gpu_->context->UpdateSubresource(output_.Get(), 0, nullptr, p, static_cast<UINT>(pitch), 0);
            b2->Unlock2D();
        } else {
            DWORD len = 0;
            if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
            gpu_->context->UpdateSubresource(output_.Get(), 0, nullptr, p, width_, 0);
            buf->Unlock();
        }
    }
    out.texture = output_.Get();
    out.width = width_;
    out.height = height_;
    return true;
}

}  // namespace hikari

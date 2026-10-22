#include "encoder.h"

#include <codecapi.h>
#include <mferror.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/system.h"

namespace hikari {

namespace {

// MFT_ENUM_ADAPTER_LUID: the DXGI adapter a hardware MFT runs on (not
// declared by every SDK).
const GUID kMftEnumAdapterLuid = {0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};

class EventCallback final : public IMFAsyncCallback {
public:
    EventCallback(VideoEncoder* owner, IMFMediaEventGenerator* gen) : owner_(owner), gen_(gen) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override
    {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(gen_->EndGetEvent(result, &ev))) return S_OK;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!owner_) return S_OK;
            owner_->on_event(ev.Get());
        }
        if (!stopped_) gen_->BeginGetEvent(this, nullptr);
        return S_OK;
    }
    void detach()
    {
        std::lock_guard<std::mutex> lk(mu_);
        owner_ = nullptr;
        stopped_ = true;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::mutex mu_;
    VideoEncoder* owner_;
    ComPtr<IMFMediaEventGenerator> gen_;  // owned: a pending callback may outlive the encoder
    std::atomic<bool> stopped_{false};
};

bool has_parameter_sets(const std::vector<uint8_t>& d, bool hevc)
{
    for (size_t i = 0; i + 3 < d.size() && i < 512; ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const uint8_t h = d[i + 3];
            if (!hevc && (h & 0x1F) == 7) return true;
            if (hevc && ((h >> 1) & 0x3F) == 32) return true;
        }
    }
    return false;
}

bool contains_idr(const std::vector<uint8_t>& d, bool hevc)
{
    for (size_t i = 0; i + 3 < d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const uint8_t h = d[i + 3];
            if (!hevc && (h & 0x1F) == 5) return true;
            if (hevc) {
                const int t = (h >> 1) & 0x3F;
                if (t >= 16 && t <= 21) return true;
            }
            if (!hevc && (h & 0x1F) == 1) return false;  // first slice is non-IDR
        }
    }
    return false;
}

}  // namespace

std::vector<EncoderInfo> enumerate_encoders(const GUID& codec, bool include_software)
{
    std::vector<EncoderInfo> out;
    MFT_REGISTER_TYPE_INFO out_type{MFMediaType_Video, codec};
    UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    if (include_software) flags |= MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT;
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &out_type, &acts, &n))) return out;
    for (UINT32 i = 0; i < n; ++i) {
        EncoderInfo e;
        e.activate.Attach(acts[i]);
        e.codec = codec;
        e.name = mf_attribute_string(acts[i], MFT_FRIENDLY_NAME_Attribute);
        UINT32 tflags = 0;
        acts[i]->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &tflags);
        e.hardware = (tflags & MFT_ENUM_FLAG_HARDWARE) != 0 ||
                     !mf_attribute_string(acts[i], MFT_ENUM_HARDWARE_URL_Attribute).empty();
        e.async = (tflags & MFT_ENUM_FLAG_ASYNCMFT) != 0 || e.hardware;
        const std::string vendor = mf_attribute_string(acts[i], MFT_ENUM_HARDWARE_VENDOR_ID_Attribute);
        if (vendor.size() >= 8) e.vendor_id = static_cast<UINT>(std::strtoul(vendor.c_str() + 4, nullptr, 16));
        UINT64 luid = 0;
        if (SUCCEEDED(acts[i]->GetUINT64(kMftEnumAdapterLuid, &luid))) {
            e.adapter_luid.LowPart = static_cast<DWORD>(luid & 0xFFFFFFFF);
            e.adapter_luid.HighPart = static_cast<LONG>(luid >> 32);
        }
        GUID clsid{};
        if (SUCCEEDED(acts[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
            wchar_t buf[64];
            StringFromGUID2(clsid, buf, 64);
            e.clsid_str = buf;
        }
        if (e.name.empty()) e.name = e.hardware ? "Hardware encoder" : "Software encoder";
        out.push_back(std::move(e));
    }
    CoTaskMemFree(acts);
    return out;
}

VideoEncoder::VideoEncoder() : latency_(2'000'000) { wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

VideoEncoder::~VideoEncoder()
{
    close();
    CloseHandle(wake_event_);
}

void VideoEncoder::set_u32(const GUID& api, UINT32 v, const char* name, bool log)
{
    if (!codec_api_) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    const HRESULT hr = codec_api_->SetValue(&api, &var);
    if (FAILED(hr) && log) HK_LOGD("Encoder ignores %s=%u (%s)", name, v, hresult_string(hr).c_str());
}

void VideoEncoder::set_bool(const GUID& api, bool v, const char* name)
{
    if (!codec_api_) return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = codec_api_->SetValue(&api, &var);
    if (FAILED(hr)) HK_LOGD("Encoder ignores %s (%s)", name, hresult_string(hr).c_str());
}

bool VideoEncoder::configure()
{
    const UINT bps = cfg_.bitrate_kbps * 1000;
    set_bool(CODECAPI_AVLowLatencyMode, true, "AVLowLatencyMode");
    set_u32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "RateControlMode");
    set_u32(CODECAPI_AVEncCommonMeanBitRate, bps, "MeanBitRate");
    set_u32(CODECAPI_AVEncCommonBufferSize, static_cast<UINT32>(bps * cfg_.vbv_frames / cfg_.fps), "BufferSize");
    set_u32(CODECAPI_AVEncMPVDefaultBPictureCount, 0, "BPictureCount");
    set_u32(CODECAPI_AVEncMPVGOPSize, cfg_.gop ? cfg_.gop : cfg_.fps * 60, "GOPSize");
    set_u32(CODECAPI_AVEncCommonQualityVsSpeed, cfg_.quality_vs_speed, "QualityVsSpeed");

    ComPtr<IMFMediaType> out;
    HK_CHECK_HR(MFCreateMediaType(&out), "MFCreateMediaType");
    HK_CHECK_HR(set_video_type(out.Get(), cfg_.codec, cfg_.width, cfg_.height, cfg_.fps, 1), "output type");
    out->SetUINT32(MF_MT_AVG_BITRATE, bps);
    if (cfg_.codec == MFVideoFormat_H264)
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    else
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
    HK_CHECK_HR(transform_->SetOutputType(out_id_, out.Get(), 0), "SetOutputType");

    ComPtr<IMFMediaType> in;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(transform_->GetInputAvailableType(in_id_, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            in = t;
            break;
        }
    }
    if (!in) HK_CHECK_HR(MFCreateMediaType(&in), "MFCreateMediaType");
    HK_CHECK_HR(set_video_type(in.Get(), MFVideoFormat_NV12, cfg_.width, cfg_.height, cfg_.fps, 1), "input type");
    HK_CHECK_HR(transform_->SetInputType(in_id_, in.Get(), 0), "SetInputType(NV12)");
    return true;
}

bool VideoEncoder::open(GpuDevice& gpu, const EncoderInfo& info, const EncoderConfig& cfg, OutputCallback cb)
{
    close();
    gpu_ = &gpu;
    cfg_ = cfg;
    cb_ = std::move(cb);
    name_ = info.name;
    failed_ = false;
    force_idr_ = true;
    need_input_ = 0;
    sequence_header_.clear();

    activate_ = info.activate;
    HK_CHECK_HR(info.activate->ActivateObject(IID_PPV_ARGS(&transform_)), "ActivateObject(encoder)");
    ComPtr<IMFAttributes> attrs;
    async_ = false;
    if (SUCCEEDED(transform_->GetAttributes(&attrs)) && attrs) {
        UINT32 is_async = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
            HK_CHECK_HR(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "MF_TRANSFORM_ASYNC_UNLOCK");
            async_ = true;
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    const HRESULT dhr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                                   reinterpret_cast<ULONG_PTR>(gpu.manager.Get()));
    if (FAILED(dhr)) {
        HK_LOGE("%s does not accept GPU frames: %s", name_.c_str(), hresult_string(dhr).c_str());
        close();
        return false;
    }
    transform_.As(&codec_api_);

    DWORD nin = 0, nout = 0;
    transform_->GetStreamCount(&nin, &nout);
    if (transform_->GetStreamIDs(1, &in_id_, 1, &out_id_) == E_NOTIMPL) in_id_ = out_id_ = 0;

    if (!configure()) {
        close();
        return false;
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    transform_->GetOutputStreamInfo(out_id_, &osi);
    provides_samples_ = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    out_buffer_size_ = std::max<DWORD>(osi.cbSize, cfg_.width * cfg_.height);

    if (async_) {
        HK_CHECK_HR(transform_.As(&events_), "IMFMediaEventGenerator");
        auto* cbk = new EventCallback(this, events_.Get());
        event_cb_.Attach(static_cast<IUnknown*>(static_cast<IMFAsyncCallback*>(cbk)));
        events_->BeginGetEvent(cbk, nullptr);
    }
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (!async_) need_input_ = 1;

    HK_LOGI("Encoder: %s (%s, %ux%u@%u, %u kbps, %s)", name_.c_str(), async_ ? "async" : "sync", cfg_.width,
            cfg_.height, cfg_.fps, cfg_.bitrate_kbps, cfg_.codec == MFVideoFormat_H264 ? "H.264" : "HEVC");
    return true;
}

void VideoEncoder::close()
{
    if (event_cb_) static_cast<EventCallback*>(static_cast<IMFAsyncCallback*>(event_cb_.Get()))->detach();
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    events_.Reset();
    event_cb_.Reset();
    codec_api_.Reset();
    transform_.Reset();
    // IMFActivate caches the MFT; shutting it down lets the next open()
    // (e.g. after the benchmark, or a resolution change) get a fresh one.
    if (activate_) activate_->ShutdownObject();
    activate_.Reset();
    {
        std::lock_guard<std::mutex> lk(flight_mu_);
        in_flight_.clear();
    }
    std::lock_guard<std::mutex> lk(ev_mu_);
    pending_have_output_ = 0;
}

void VideoEncoder::on_event(IMFMediaEvent* ev)
{
    MediaEventType type = MEUnknown;
    ev->GetType(&type);
    HRESULT status = S_OK;
    ev->GetStatus(&status);
    if (FAILED(status)) {
        HK_LOGE("Encoder event %lu failed: %s", type, hresult_string(status).c_str());
        failed_ = true;
    }
    {
        std::lock_guard<std::mutex> lk(ev_mu_);
        if (type == METransformNeedInput) ++need_input_;
        if (type == METransformHaveOutput) ++pending_have_output_;
    }
    SetEvent(wake_event_);
}

bool VideoEncoder::can_accept() const { return transform_ && need_input_.load() > 0 && !failed_.load(); }

bool VideoEncoder::submit(ID3D11Texture2D* tex, uint32_t frame_index, uint64_t capture_us)
{
    if (!can_accept()) return false;
    if (force_idr_.exchange(false)) set_u32(CODECAPI_AVEncVideoForceKeyFrame, 1, "ForceKeyFrame");

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &buf))) return false;
    ComPtr<IMF2DBuffer> b2;
    DWORD len = 0;
    if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    // Real capture time as the timestamp keeps CBR accounting correct when
    // frames are dropped upstream.
    const LONGLONG t = static_cast<LONGLONG>(capture_us) * 10;
    sample->SetSampleTime(t);
    sample->SetSampleDuration(10'000'000 / cfg_.fps);

    const uint64_t submit_us = now_us();
    {
        std::lock_guard<std::mutex> lk(flight_mu_);
        in_flight_.push_back({static_cast<uint64_t>(t), frame_index, capture_us, submit_us});
        while (in_flight_.size() > 16) in_flight_.pop_front();
    }
    const HRESULT hr = transform_->ProcessInput(in_id_, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) return false;
    if (FAILED(hr)) {
        HK_LOGE("Encoder ProcessInput: %s", hresult_string(hr).c_str());
        failed_ = true;
        return false;
    }
    if (async_)
        --need_input_;
    else
        drain_outputs();
    return true;
}

void VideoEncoder::pump()
{
    if (!async_) return;
    int outputs = 0;
    {
        std::lock_guard<std::mutex> lk(ev_mu_);
        outputs = pending_have_output_;
        pending_have_output_ = 0;
    }
    for (int i = 0; i < outputs; ++i) process_output();
}

void VideoEncoder::drain_outputs()
{
    for (int i = 0; i < 8 && process_output(); ++i) {
    }
}

bool VideoEncoder::process_output()
{
    if (!transform_) return false;
    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = out_id_;
    ComPtr<IMFSample> own;
    if (!provides_samples_) {
        ComPtr<IMFMediaBuffer> mb;
        MFCreateMemoryBuffer(out_buffer_size_, &mb);
        MFCreateSample(&own);
        own->AddBuffer(mb.Get());
        ob.pSample = own.Get();
    }
    DWORD status = 0;
    HRESULT hr = transform_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> t;
        if (SUCCEEDED(transform_->GetOutputAvailableType(out_id_, 0, &t))) transform_->SetOutputType(out_id_, t.Get(), 0);
        sequence_header_.clear();
        return true;
    }
    if (FAILED(hr)) {
        HK_LOGE("Encoder ProcessOutput: %s", hresult_string(hr).c_str());
        failed_ = true;
        return false;
    }
    ComPtr<IMFSample> sample;
    if (provides_samples_)
        sample.Attach(ob.pSample);
    else
        sample = own;
    if (!sample) return true;

    EncodedFrame f;
    f.done_us = now_us();
    LONGLONG t = 0;
    sample->GetSampleTime(&t);
    ComPtr<IMFMediaBuffer> cb;
    if (FAILED(sample->ConvertToContiguousBuffer(&cb))) return true;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(cb->Lock(&p, nullptr, &len))) return true;
    f.data.assign(p, p + len);
    cb->Unlock();

    const bool hevc = cfg_.codec == MFVideoFormat_HEVC;
    f.keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0) != 0 || contains_idr(f.data, hevc);
    if (f.keyframe && !has_parameter_sets(f.data, hevc)) {
        if (sequence_header_.empty()) {
            ComPtr<IMFMediaType> ot;
            UINT32 sz = 0;
            if (SUCCEEDED(transform_->GetOutputCurrentType(out_id_, &ot)) &&
                SUCCEEDED(ot->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &sz)) && sz) {
                sequence_header_.resize(sz);
                ot->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence_header_.data(), sz, nullptr);
            }
        }
        f.data.insert(f.data.begin(), sequence_header_.begin(), sequence_header_.end());
    }

    {
        std::lock_guard<std::mutex> lk(flight_mu_);
        while (!in_flight_.empty() && in_flight_.front().sample_time < static_cast<uint64_t>(t)) in_flight_.pop_front();
        if (!in_flight_.empty() && in_flight_.front().sample_time == static_cast<uint64_t>(t)) {
            const auto& pf = in_flight_.front();
            f.frame_index = pf.frame_index;
            f.capture_us = pf.capture_us;
            f.submit_us = pf.submit_us;
            in_flight_.pop_front();
        } else {
            f.capture_us = static_cast<uint64_t>(t) / 10;
            f.submit_us = f.done_us;
        }
    }
    latency_.record(f.done_us - f.submit_us, f.done_us);
    if (cb_) cb_(std::move(f));
    return true;
}

bool VideoEncoder::set_bitrate(UINT kbps)
{
    if (!codec_api_ || kbps == cfg_.bitrate_kbps) return true;
    cfg_.bitrate_kbps = kbps;
    const UINT bps = kbps * 1000;
    set_u32(CODECAPI_AVEncCommonMeanBitRate, bps, "MeanBitRate");
    set_u32(CODECAPI_AVEncCommonBufferSize, static_cast<UINT32>(bps * cfg_.vbv_frames / cfg_.fps), "BufferSize", false);
    return true;
}

LatencySummary VideoEncoder::latency() { return latency_.summary(now_us()); }

// ---- benchmark -----------------------------------------------------------

EncoderBenchmark benchmark_encoder(GpuDevice& gpu, const EncoderInfo& info, const EncoderConfig& cfg, int frames,
                                   bool paced)
{
    EncoderBenchmark r;
    r.name = info.name;

    // Eight distinct synthetic frames (gradients + noise blocks) so the
    // encoder does real motion work instead of coding a static image.
    constexpr int kFrames = 8;
    ComPtr<ID3D11Texture2D> tex[kFrames];
    std::vector<uint8_t> cpu(size_t(cfg.width) * cfg.height * 3 / 2);
    std::mt19937 rng(1);
    for (int i = 0; i < kFrames; ++i) {
        for (UINT y = 0; y < cfg.height; ++y)
            for (UINT x = 0; x < cfg.width; ++x)
                cpu[size_t(y) * cfg.width + x] =
                    static_cast<uint8_t>(((x + i * 24) ^ (y + i * 8)) + ((x / 64 + y / 64 + i) % 3 == 0 ? rng() % 64 : 0));
        std::memset(cpu.data() + size_t(cfg.width) * cfg.height, 128 + i * 4, size_t(cfg.width) * cfg.height / 2);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = cfg.width;
        d.Height = cfg.height;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{cpu.data(), cfg.width, 0};
        if (FAILED(gpu.device->CreateTexture2D(&d, &sd, &tex[i]))) return r;
    }

    VideoEncoder enc;
    size_t bytes = 0;
    int outputs = 0;
    if (!enc.open(gpu, info, cfg, [&](EncodedFrame&& f) {
            bytes += f.data.size();
            ++outputs;
        }))
        return r;

    const uint64_t period = 1'000'000 / cfg.fps;
    const uint64_t start = now_us();
    uint64_t next = start;
    PreciseTimer timer;
    int submitted = 0;
    while (outputs < frames && now_us() - start < 15'000'000) {
        enc.pump();
        if (enc.failed()) return r;
        if (submitted < frames && (!paced || now_us() >= next) && enc.can_accept()) {
            enc.submit(tex[submitted % kFrames].Get(), submitted, now_us());
            ++submitted;
            next += period;
        }
        if (paced && now_us() < next && !enc.can_accept())
            WaitForSingleObject(enc.event(), 1);
        else if (paced && now_us() < next)
            timer.sleep_until_us(std::min(next, now_us() + 500));
        else
            WaitForSingleObject(enc.event(), 1);
    }
    const double secs = double(now_us() - start) / 1e6;
    r.ok = outputs >= frames * 9 / 10;
    r.fps = secs > 0 ? outputs / secs : 0;
    r.latency = enc.latency();
    r.avg_frame_bytes = outputs ? bytes / size_t(outputs) : 0;
    return r;
}

int select_best_encoder(GpuDevice& gpu, std::vector<EncoderInfo>& encoders, const EncoderConfig& cfg,
                        std::vector<EncoderBenchmark>* results)
{
    int best = -1;
    double best_score = 1e18;
    for (size_t i = 0; i < encoders.size(); ++i) {
        EncoderBenchmark b = benchmark_encoder(gpu, encoders[i], cfg, 120, true);
        HK_LOGI("Encoder benchmark %-40s ok=%d fps=%.1f latency %s", b.name.c_str(), b.ok, b.fps,
                b.latency.to_ms_string().c_str());
        if (results) results->push_back(b);
        if (!b.ok || b.fps < cfg.fps * 0.95) continue;
        // p95 dominates; p99 breaks ties so a jittery encoder loses.
        const double score = b.latency.p95 + 0.25 * b.latency.p99;
        if (score < best_score) {
            best_score = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace hikari

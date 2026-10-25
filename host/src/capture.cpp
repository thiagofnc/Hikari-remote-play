#include "capture.h"

#include <mferror.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio_capture.h"
#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/system.h"

namespace hikari {

namespace {

// {6523775a-ba2d-405f-b2c5-01ff88e2e8f6}: QPC time (100 ns) at which the
// device captured the frame. Not declared by every SDK, so defined here.
const GUID kDeviceReferenceSystemTime = {0x6523775a, 0xba2d, 0x405f, {0xb2, 0xc5, 0x01, 0xff, 0x88, 0xe2, 0xe8, 0xf6}};

class ReaderCallback final : public IMFSourceReaderCallback {
public:
    explicit ReaderCallback(VideoCapture* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
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
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG ts, IMFSample* sample) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (owner_) owner_->on_sample(hr, flags, ts, sample);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

    void detach()
    {
        std::lock_guard<std::mutex> lk(mu_);
        owner_ = nullptr;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::mutex mu_;
    VideoCapture* owner_;
};

int format_rank(const GUID& g, const std::string& pref)
{
    const std::string name = subtype_name(g);
    const auto pos = pref.find(name);
    if (pos != std::string::npos) return static_cast<int>(pos);
    return 1000;
}

DXGI_FORMAT dxgi_for(const GUID& g)
{
    if (g == MFVideoFormat_NV12) return DXGI_FORMAT_NV12;
    if (g == MFVideoFormat_YUY2) return DXGI_FORMAT_YUY2;
    if (g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string CaptureMode::to_string() const
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ux%u @ %.2f %s", width, height, fps(), subtype_name(subtype).c_str());
    return buf;
}

std::vector<CaptureDeviceInfo> enumerate_capture_devices(bool with_modes)
{
    std::vector<CaptureDeviceInfo> out;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1))) return out;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return out;

    for (UINT32 i = 0; i < count; ++i) {
        CaptureDeviceInfo info;
        info.name = mf_attribute_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        wchar_t* link = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link,
                                                     &len))) {
            info.symlink = link;
            CoTaskMemFree(link);
        }
        if (with_modes) {
            ComPtr<IMFMediaSource> source;
            if (SUCCEEDED(devices[i]->ActivateObject(IID_PPV_ARGS(&source)))) {
                ComPtr<IMFSourceReader> reader;
                if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader))) {
                    for (DWORD k = 0;; ++k) {
                        ComPtr<IMFMediaType> t;
                        if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, k, &t))) break;
                        CaptureMode m;
                        m.native_index = k;
                        t->GetGUID(MF_MT_SUBTYPE, &m.subtype);
                        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &m.width, &m.height);
                        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &m.fps_num, &m.fps_den);
                        info.modes.push_back(m);
                    }
                }
                source->Shutdown();
            }
            devices[i]->ShutdownObject();
        }
        devices[i]->Release();
        out.push_back(std::move(info));
    }
    CoTaskMemFree(devices);
    return out;
}

int capture_device_score(const CaptureDeviceInfo& dev)
{
    const std::string n = lower(dev.name);
    int score = 0;
    for (const char* kw : {"capture", "hdmi", "cam link", "elgato", "avermedia", "live gamer", "usb3", "ugreen",
                           "video", "uvc", "ezcap", "razer ripsaw", "magewell", "game"})
        if (n.find(kw) != std::string::npos) score += 10;
    for (const char* kw : {"integrated", "webcam", "camera", "facetime", "front", "rear", "virtual", "obs"})
        if (n.find(kw) != std::string::npos) score -= 50;
    // Built into the PC: a laptop camera, never a capture card.
    GUID container{};
    if (container_id_for_symlink(dev.symlink, container) && is_builtin_container(container)) score -= 100;
    for (const auto& m : dev.modes)
        if (m.width >= 1920 && m.height >= 1080 && m.fps() >= 59) {
            score += 30;
            break;
        }
    return score;
}

bool choose_capture_mode(const CaptureDeviceInfo& dev, UINT32 want_w, UINT32 want_h, double want_fps,
                         const std::string& pref, CaptureMode& out)
{
    const CaptureMode* best = nullptr;
    double best_cost = 1e18;
    for (const auto& m : dev.modes) {
        if (dxgi_for(m.subtype) == DXGI_FORMAT_UNKNOWN && m.subtype != MFVideoFormat_MJPG) continue;
        // Cost: resolution mismatch dominates, then frame rate, then format.
        const double res = std::fabs(double(m.width) * m.height - double(want_w) * want_h) / (double(want_w) * want_h);
        const double fps_short = std::max(0.0, want_fps - m.fps() - 0.5);
        const double fps_over = std::max(0.0, m.fps() - want_fps - 0.5);
        const double cost = res * 1000 + fps_short * 100 + fps_over * 2 + format_rank(m.subtype, pref);
        if (cost < best_cost) {
            best_cost = cost;
            best = &m;
        }
    }
    if (!best) return false;
    out = *best;
    return true;
}

VideoCapture::VideoCapture() : interval_hist_(2'000'000), driver_hist_(2'000'000) {}

VideoCapture::~VideoCapture() { close(); }

bool VideoCapture::is_open() const { return running_.load(); }

void VideoCapture::set_frame_callback(FrameCallback cb)
{
    std::lock_guard<std::mutex> lk(cb_mu_);
    callback_fn_ = std::move(cb);
}

bool VideoCapture::open(GpuDevice& gpu, const CaptureDeviceInfo& dev, const CaptureMode& mode)
{
    close();
    gpu_ = &gpu;
    mode_ = mode;
    device_name_ = dev.name;
    failed_ = false;

    ComPtr<IMFAttributes> src_attrs;
    HK_CHECK_HR(MFCreateAttributes(&src_attrs, 2), "MFCreateAttributes");
    src_attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    src_attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, dev.symlink.c_str());
    ComPtr<IMFMediaSource> source;
    HK_CHECK_HR(MFCreateDeviceSource(src_attrs.Get(), &source), "MFCreateDeviceSource");

    auto* cb = new ReaderCallback(this);
    callback_.Attach(static_cast<IUnknown*>(static_cast<IMFSourceReaderCallback*>(cb)));

    const bool mjpeg = mode.subtype == MFVideoFormat_MJPG;
    ComPtr<IMFAttributes> attrs;
    HK_CHECK_HR(MFCreateAttributes(&attrs, 6), "MFCreateAttributes");
    attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.Get());
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, mjpeg ? FALSE : TRUE);
    if (mjpeg) {
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu.manager.Get());
    }
    HK_CHECK_HR(MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader_),
                "MFCreateSourceReaderFromMediaSource");

    ComPtr<IMFMediaType> native;
    HK_CHECK_HR(reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, mode.native_index, &native),
                "GetNativeMediaType");
    HK_CHECK_HR(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native.Get()),
                "SetCurrentMediaType(native)");
    if (mjpeg) {
        ComPtr<IMFMediaType> nv12;
        MFCreateMediaType(&nv12);
        set_video_type(nv12.Get(), MFVideoFormat_NV12, mode.width, mode.height, mode.fps_num, mode.fps_den);
        HK_CHECK_HR(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nv12.Get()),
                    "SetCurrentMediaType(NV12 from MJPG)");
    }
    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    ComPtr<IMFMediaType> cur;
    reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    cur->GetGUID(MF_MT_SUBTYPE, &output_subtype_);
    upload_format_ = dxgi_for(output_subtype_);
    if (upload_format_ == DXGI_FORMAT_UNKNOWN) {
        HK_LOGE("Capture output format %s is not supported", subtype_name(output_subtype_).c_str());
        return false;
    }
    for (auto& t : upload_ring_) t.Reset();
    probe_staging_.Reset();
    last_frame_us_ = 0;
    signal_votes_ = 0;

    HK_LOGI("Capture: %s, %s -> %s", dev.name.c_str(), mode.to_string().c_str(),
            subtype_name(output_subtype_).c_str());
    running_ = true;
    request_next();
    return true;
}

void VideoCapture::close()
{
    running_ = false;
    if (callback_) static_cast<ReaderCallback*>(static_cast<IMFSourceReaderCallback*>(callback_.Get()))->detach();
    if (reader_) reader_->Flush(MF_SOURCE_READER_ALL_STREAMS);
    reader_.Reset();
    callback_.Reset();
    signal_ = false;
}

void VideoCapture::request_next()
{
    if (!running_ || !reader_) return;
    const HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        HK_LOGE("ReadSample failed: %s", hresult_string(hr).c_str());
        failed_ = true;
    }
}

void VideoCapture::on_sample(HRESULT hr, DWORD flags, LONGLONG, IMFSample* sample)
{
    if (!running_) return;
    if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        ++errors_;
        if (!failed_) HK_LOGW("Capture stopped: %s flags=0x%lx", hresult_string(hr).c_str(), flags);
        failed_ = true;
        signal_ = false;
        return;
    }
    const uint64_t now = now_us();
    if (sample) {
        ++frames_;
        fps_.add();
        if (last_frame_us_) interval_hist_.record(now - last_frame_us_, now);
        last_frame_us_ = now;

        CapturedFrame f;
        f.callback_us = now;
        f.capture_us = now;
        UINT64 dev_time = 0;
        if (SUCCEEDED(sample->GetUINT64(kDeviceReferenceSystemTime, &dev_time))) {
            const uint64_t t = hns_to_us(static_cast<int64_t>(dev_time));
            if (t <= now && now - t < 200'000) {
                f.capture_us = t;
                driver_hist_.record(now - t, now);
            }
        }
        f.index = frame_index_++;

        ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer))) {
            const bool inspect = (f.index % 15) == 0;
            FrameCallback cb;
            {
                std::lock_guard<std::mutex> lk(cb_mu_);
                cb = callback_fn_;
            }
            const bool want_upload = upload_.load() && cb;
            ComPtr<IMFDXGIBuffer> dxgi;
            if (SUCCEEDED(buffer.As(&dxgi))) {
                ComPtr<ID3D11Texture2D> tex;
                UINT sub = 0;
                if (SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(&tex))) && SUCCEEDED(dxgi->GetSubresourceIndex(&sub))) {
                    if (inspect) inspect_signal_gpu(tex.Get(), sub);
                    if (want_upload) {
                        D3D11_TEXTURE2D_DESC d;
                        tex->GetDesc(&d);
                        f.texture = tex;
                        f.subresource = sub;
                        f.format = d.Format;
                        f.width = mode_.width;
                        f.height = mode_.height;
                        cb(std::move(f));
                    }
                }
            } else if (want_upload || inspect) {
                if (want_upload && upload_frame(buffer.Get(), f))
                    cb(std::move(f));
                else if (inspect) {
                    BYTE* data = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(buffer->Lock(&data, nullptr, &len))) {
                        const LONG pitch = static_cast<LONG>(
                            output_subtype_ == MFVideoFormat_YUY2 ? mode_.width * 2
                            : upload_format_ == DXGI_FORMAT_B8G8R8A8_UNORM ? mode_.width * 4
                                                                           : mode_.width);
                        inspect_signal(data, pitch, output_subtype_ == MFVideoFormat_YUY2);
                        buffer->Unlock();
                    }
                }
            }
        }
    }
    request_next();
}

bool VideoCapture::upload_frame(IMFMediaBuffer* buffer, CapturedFrame& out)
{
    auto& dev = gpu_->device;
    auto& ctx = gpu_->context;
    auto& tex = upload_ring_[upload_next_];
    upload_next_ = (upload_next_ + 1) % kUploadRing;
    if (!tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = mode_.width;
        d.Height = mode_.height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = upload_format_;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) {
            d.BindFlags = 0;
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return false;
        }
    }

    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    BYTE* start = nullptr;
    DWORD blen = 0;
    ComPtr<IMF2DBuffer2> b2;
    ComPtr<IMF2DBuffer> b1;
    enum { kLockPlain, kLock2D, kLock2DSize } lock = kLockPlain;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&b2))) &&
        SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &blen))) {
        lock = kLock2DSize;
    } else if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&b1))) && SUCCEEDED(b1->Lock2D(&scan0, &pitch))) {
        lock = kLock2D;
        blen = 0;
    } else {
        DWORD len = 0;
        if (FAILED(buffer->Lock(&scan0, nullptr, &len))) return false;
        blen = len;
        pitch = static_cast<LONG>(upload_format_ == DXGI_FORMAT_YUY2 ? mode_.width * 2
                                  : upload_format_ == DXGI_FORMAT_NV12 ? mode_.width
                                                                       : mode_.width * 4);
    }
    auto unlock = [&] {
        if (lock == kLock2DSize)
            b2->Unlock2D();
        else if (lock == kLock2D)
            b1->Unlock2D();
        else
            buffer->Unlock();
    };
    if (pitch < 0) {  // bottom-up RGB: not worth supporting, ask for NV12/YUY2 instead
        unlock();
        return false;
    }

    const bool yuy2 = upload_format_ == DXGI_FORMAT_YUY2;
    if ((out.index % 15) == 0) inspect_signal(scan0, pitch, yuy2);

    if (upload_format_ == DXGI_FORMAT_NV12) {
        // D3D11 expects the UV plane to follow the Y plane at pitch*height.
        // Some drivers pad the luma plane; repack in that (rare) case.
        const size_t y_bytes = size_t(pitch) * mode_.height;
        const size_t uv_bytes = size_t(pitch) * mode_.height / 2;
        if (blen != 0 && blen > y_bytes + uv_bytes) {
            static thread_local std::vector<uint8_t> repack;
            repack.resize(y_bytes + uv_bytes);
            std::memcpy(repack.data(), scan0, y_bytes);
            std::memcpy(repack.data() + y_bytes, scan0 + blen - uv_bytes, uv_bytes);
            ctx->UpdateSubresource(tex.Get(), 0, nullptr, repack.data(), static_cast<UINT>(pitch), 0);
        } else {
            ctx->UpdateSubresource(tex.Get(), 0, nullptr, scan0, static_cast<UINT>(pitch), 0);
        }
    } else {
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, scan0, static_cast<UINT>(pitch), 0);
    }

    unlock();
    out.texture = tex;
    out.subresource = 0;
    out.format = upload_format_;
    out.width = mode_.width;
    out.height = mode_.height;
    return true;
}

void VideoCapture::inspect_signal(const uint8_t* data, LONG pitch, bool yuy2)
{
    // 8x8 luma grid over the central 80% of the frame. A disconnected or
    // sleeping console makes capture cards emit a uniform colour (black,
    // blue or grey), so "non-uniform" is a robust "picture present" test.
    double sum = 0, sum2 = 0;
    int n = 0;
    const bool rgb = upload_format_ == DXGI_FORMAT_B8G8R8A8_UNORM;
    for (int gy = 0; gy < 8; ++gy) {
        const UINT y = mode_.height / 10 + gy * (mode_.height * 8 / 10) / 8;
        for (int gx = 0; gx < 8; ++gx) {
            const UINT x = mode_.width / 10 + gx * (mode_.width * 8 / 10) / 8;
            const uint8_t* row = data + size_t(y) * size_t(pitch);
            const double l = rgb ? row[x * 4 + 1] : yuy2 ? row[x * 2] : row[x];
            sum += l;
            sum2 += l * l;
            ++n;
        }
    }
    const double mean = sum / n;
    const double var = std::max(0.0, sum2 / n - mean * mean);
    luma_ = static_cast<float>(mean);
    const bool vote = std::sqrt(var) >= 3.0;
    // Two consecutive agreeing votes (~0.5 s apart) before changing state.
    if (vote == signal_.load()) {
        signal_votes_ = 0;
        return;
    }
    if (++signal_votes_ >= 2) {
        signal_ = vote;
        signal_votes_ = 0;
        HK_LOGI("HDMI picture %s (luma %.0f, sd %.1f)", vote ? "detected" : "lost", mean, std::sqrt(var));
    }
}

void VideoCapture::inspect_signal_gpu(ID3D11Texture2D* tex, UINT sub)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (!probe_staging_) {
        D3D11_TEXTURE2D_DESC s{};
        s.Width = d.Width;
        s.Height = d.Height;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.Format = d.Format;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(gpu_->device->CreateTexture2D(&s, nullptr, &probe_staging_))) return;
    }
    // Full-frame copy every 15th frame (~4 per second): cheap on the GPU,
    // and the map below only touches 64 bytes.
    gpu_->context->CopySubresourceRegion(probe_staging_.Get(), 0, 0, 0, 0, tex, sub, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(gpu_->context->Map(probe_staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    const auto saved = upload_format_;
    upload_format_ = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM : upload_format_;
    inspect_signal(static_cast<const uint8_t*>(m.pData), static_cast<LONG>(m.RowPitch), d.Format == DXGI_FORMAT_YUY2);
    upload_format_ = saved;
    gpu_->context->Unmap(probe_staging_.Get(), 0);
}

CaptureStats VideoCapture::stats()
{
    const uint64_t now = now_us();
    CaptureStats s;
    s.fps = fps_.sample(now);
    s.interval = interval_hist_.summary(now);
    s.driver_us = driver_hist_.summary(now);
    s.frames = frames_.load();
    s.errors = errors_.load();
    // Frames stopped arriving: the card lost lock (console off / asleep).
    if (last_frame_us_ && now - last_frame_us_ > 1'000'000) signal_ = false;
    s.signal = signal_.load();
    s.mean_luma = luma_.load();
    return s;
}

}  // namespace hikari

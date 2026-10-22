#include "hikari/media.h"

#include <mferror.h>

#include <cstdio>

#include "hikari/log.h"
#include "hikari/system.h"

namespace hikari {

MediaRuntime::MediaRuntime()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_ = SUCCEEDED(hr);
    ok_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    if (!ok_) HK_LOGE("MFStartup failed");
}

MediaRuntime::~MediaRuntime()
{
    if (ok_) MFShutdown();
    if (com_) CoUninitialize();
}

std::vector<ComPtr<IDXGIAdapter1>> enumerate_adapters()
{
    std::vector<ComPtr<IDXGIAdapter1>> out;
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f6)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; SUCCEEDED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                   IID_PPV_ARGS(&a)));
             ++i) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) out.push_back(a);
            a.Reset();
        }
        return out;
    }
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        out.push_back(a);
        a.Reset();
    }
    return out;
}

const char* vendor_name(UINT v)
{
    switch (v) {
    case 0x8086: return "Intel";
    case 0x10DE: return "NVIDIA";
    case 0x1002:
    case 0x1022: return "AMD";
    case 0x1414: return "Microsoft";
    case 0x5143: return "Qualcomm";
    }
    return "Unknown";
}

bool GpuDevice::create(IDXGIAdapter1* adapter, bool bgra_support)
{
    ComPtr<IDXGIAdapter1> chosen = adapter;
    if (!chosen) {
        auto all = enumerate_adapters();
        if (!all.empty()) chosen = all.front();
    }
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (bgra_support) flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                               D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr,
                                   &context);
    if (FAILED(hr)) {
        HK_LOGE("D3D11CreateDevice failed: %s", hresult_string(hr).c_str());
        return false;
    }
    // MF components call into the context from their own threads.
    if (SUCCEEDED(device.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> ad;
    if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC d;
        ad->GetDesc(&d);
        adapter_name = utf8(d.Description);
        adapter_luid = d.AdapterLuid;
        vendor_id = d.VendorId;
        // Lowest possible queueing in the DXGI layer for anything we present.
        dxgi->SetGPUThreadPriority(7);
    }

    HK_CHECK_HR(MFCreateDXGIDeviceManager(&reset_token, &manager), "MFCreateDXGIDeviceManager");
    HK_CHECK_HR(manager->ResetDevice(device.Get(), reset_token), "IMFDXGIDeviceManager::ResetDevice");
    HK_LOGI("GPU: %s (%s)", adapter_name.c_str(), vendor_name(vendor_id));
    return true;
}

std::string mf_attribute_string(IMFAttributes* attrs, const GUID& key)
{
    wchar_t* s = nullptr;
    UINT32 len = 0;
    if (FAILED(attrs->GetAllocatedString(key, &s, &len))) return {};
    std::string r = utf8(s);
    CoTaskMemFree(s);
    return r;
}

std::string subtype_name(const GUID& g)
{
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_MJPG) return "MJPG";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    if (g == MFVideoFormat_ARGB32) return "ARGB32";
    if (g == MFVideoFormat_RGB24) return "RGB24";
    if (g == MFVideoFormat_I420) return "I420";
    if (g == MFVideoFormat_IYUV) return "IYUV";
    if (g == MFVideoFormat_UYVY) return "UYVY";
    if (g == MFVideoFormat_P010) return "P010";
    if (g == MFVideoFormat_H264) return "H264";
    if (g == MFVideoFormat_HEVC) return "HEVC";
    // Unknown: FOURCC is the first DWORD for video subtypes.
    char buf[8] = {};
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((g.Data1 >> (8 * i)) & 0xFF);
        buf[i] = (c >= 32 && c < 127) ? c : '?';
    }
    return buf;
}

HRESULT set_video_type(IMFMediaType* t, const GUID& subtype, UINT32 w, UINT32 h, UINT32 fps_num, UINT32 fps_den)
{
    HRESULT hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps_num, fps_den);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return hr;
}

}  // namespace hikari

#pragma once
// Shared Direct3D 11 / Media Foundation plumbing for host and client.

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "hikari/log.h"
#include "hikari/system.h"

namespace hikari {

using Microsoft::WRL::ComPtr;

// RAII for COM (MTA) + Media Foundation on the calling thread/process.
class MediaRuntime {
public:
    MediaRuntime();
    ~MediaRuntime();
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
    bool com_ = false;
};

// One D3D11 device shared by capture, conversion and encode (host) or
// decode and present (client) so frames never leave the GPU.
struct GpuDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Multithread> multithread;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT reset_token = 0;
    std::string adapter_name;
    LUID adapter_luid{};
    UINT vendor_id = 0;

    // adapter == nullptr picks the high-performance adapter.
    bool create(IDXGIAdapter1* adapter = nullptr, bool bgra_support = true);
};

std::vector<ComPtr<IDXGIAdapter1>> enumerate_adapters();
const char* vendor_name(UINT vendor_id);

// Media Foundation helpers.
std::string mf_attribute_string(IMFAttributes* attrs, const GUID& key);
std::string subtype_name(const GUID& subtype);
HRESULT set_video_type(IMFMediaType* t, const GUID& subtype, UINT32 w, UINT32 h, UINT32 fps_num, UINT32 fps_den);

#define HK_CHECK_HR(expr, msg)                                                     \
    do {                                                                           \
        const HRESULT hr_ = (expr);                                                \
        if (FAILED(hr_)) {                                                         \
            HK_LOGE("%s: %s", msg, ::hikari::hresult_string(hr_).c_str());         \
            return false;                                                          \
        }                                                                          \
    } while (0)

}  // namespace hikari

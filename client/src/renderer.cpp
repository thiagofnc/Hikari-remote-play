#include "renderer.h"

#include <d3dcompiler.h>
#include <wincodec.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/system.h"

namespace hikari {

namespace {

// Fullscreen triangle; UVs letterbox via the viewport.
const char* kVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
})";

// BT.709 limited range YCbCr -> RGB (what the Switch outputs over HDMI).
// Luma is scaled with Catmull-Rom (9 bilinear taps): it keeps edges and text
// crisp when the picture is scaled or stretched, and returns the exact pixels
// at 1:1. Chroma is half resolution and stays bilinear.
const char* kPS = R"(
Texture2D<float> texY : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState samp : register(s0);
float catmull_rom(float2 uv) {
    float2 size;
    texY.GetDimensions(size.x, size.y);
    float2 p = uv * size;
    float2 t1 = floor(p - 0.5) + 0.5;
    float2 f = p - t1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (t1 - 1.0) / size, t3 = (t1 + 2.0) / size, t12 = (t1 + w2 / w12) / size;
    float y = 0;
    y += (texY.SampleLevel(samp, float2(t0.x, t0.y), 0) * w0.x + texY.SampleLevel(samp, float2(t12.x, t0.y), 0) * w12.x +
          texY.SampleLevel(samp, float2(t3.x, t0.y), 0) * w3.x) * w0.y;
    y += (texY.SampleLevel(samp, float2(t0.x, t12.y), 0) * w0.x + texY.SampleLevel(samp, float2(t12.x, t12.y), 0) * w12.x +
          texY.SampleLevel(samp, float2(t3.x, t12.y), 0) * w3.x) * w12.y;
    y += (texY.SampleLevel(samp, float2(t0.x, t3.y), 0) * w0.x + texY.SampleLevel(samp, float2(t12.x, t3.y), 0) * w12.x +
          texY.SampleLevel(samp, float2(t3.x, t3.y), 0) * w3.x) * w3.y;
    return y;
}
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float y = catmull_rom(uv);
    float2 c = texUV.Sample(samp, uv) - 0.5;
    y = (y - 16.0 / 255.0) * (255.0 / 219.0);
    c *= 255.0 / 224.0;
    float3 rgb = float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);
    return float4(saturate(rgb), 1);
})";

ComPtr<ID3DBlob> compile(const char* src, const char* target)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        HK_LOGE("Shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

}  // namespace

const char* to_string(PresentMode m)
{
    switch (m) {
    case PresentMode::MinimumLatency: return "Minimum latency";
    case PresentMode::Balanced: return "Balanced";
    case PresentMode::Smooth: return "Smooth";
    }
    return "?";
}

bool Renderer::init(HWND hwnd, GpuDevice& gpu)
{
    hwnd_ = hwnd;
    gpu_ = &gpu;

    ComPtr<IDXGIDevice> dxgi_dev;
    gpu.device.As(&dxgi_dev);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            tearing_ = allow != FALSE;
    }
    ComPtr<IDXGIFactory2> f2;
    HK_CHECK_HR(adapter->GetParent(IID_PPV_ARGS(&f2)), "IDXGIFactory2");

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;  // minimum for flip model: one on screen, one being drawn
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ComPtr<IDXGISwapChain1> sc1;
    HK_CHECK_HR(f2->CreateSwapChainForHwnd(gpu.device.Get(), hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd");
    f2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // we do borderless fullscreen ourselves
    HK_CHECK_HR(sc1.As(&swap_), "IDXGISwapChain2");
    swap_->SetMaximumFrameLatency(1);
    waitable_ = swap_->GetFrameLatencyWaitableObject();

    auto vs = compile(kVS, "vs_4_0");
    auto ps = compile(kPS, "ps_4_0");
    if (!vs || !ps) return false;
    gpu.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    gpu.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    gpu.device->CreateSamplerState(&smp, &sampler_);

    D2D1_FACTORY_OPTIONS opts{};
    HK_CHECK_HR(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                  reinterpret_cast<void**>(d2d_factory_.GetAddressOf())),
                "D2D1CreateFactory");
    HK_CHECK_HR(d2d_factory_->CreateDevice(dxgi_dev.Get(), &d2d_device_), "ID2D1Device");
    HK_CHECK_HR(d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_), "ID2D1DeviceContext");
    painter_.init();
    return create_targets();
}

void Renderer::shutdown()
{
    release_targets();
    srv_y_.Reset();
    srv_uv_.Reset();
    d2d_.Reset();
    d2d_device_.Reset();
    swap_.Reset();
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
}

void Renderer::release_targets()
{
    if (d2d_) d2d_->SetTarget(nullptr);
    d2d_target_.Reset();
    rtv_.Reset();
}

bool Renderer::create_targets()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = std::max<LONG>(1, rc.right - rc.left);
    height_ = std::max<LONG>(1, rc.bottom - rc.top);
    dpi_scale_ = GetDpiForWindow(hwnd_) / 96.0f;

    ComPtr<ID3D11Texture2D> back;
    HK_CHECK_HR(swap_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
    HK_CHECK_HR(gpu_->device->CreateRenderTargetView(back.Get(), nullptr, &rtv_), "CreateRenderTargetView");

    ComPtr<IDXGISurface> surface;
    back.As(&surface);
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    HK_CHECK_HR(d2d_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &d2d_target_), "D2D target");
    d2d_->SetTarget(d2d_target_.Get());
    return true;
}

void Renderer::resize()
{
    if (!swap_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = std::max<LONG>(1, rc.right - rc.left), h = std::max<LONG>(1, rc.bottom - rc.top);
    if (w == width_ && h == height_) return;
    release_targets();
    gpu_->context->ClearState();
    gpu_->context->Flush();
    swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
                         DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    create_targets();
}

void Renderer::wait_for_slot(DWORD timeout_ms)
{
    if (waitable_) WaitForSingleObjectEx(waitable_, timeout_ms, TRUE);
}

bool Renderer::ensure_video_views(ID3D11Texture2D* tex)
{
    if (tex == video_tex_ && srv_y_) return true;
    srv_y_.Reset();
    srv_uv_.Reset();
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UNORM;  // luma plane
    if (FAILED(gpu_->device->CreateShaderResourceView(tex, &d, &srv_y_))) return false;
    d.Format = DXGI_FORMAT_R8G8_UNORM;  // interleaved chroma plane
    if (FAILED(gpu_->device->CreateShaderResourceView(tex, &d, &srv_uv_))) return false;
    video_tex_ = tex;
    return true;
}

void Renderer::render(ID3D11Texture2D* video, UINT vw, UINT vh, const Draw2D& draw2d, double time_s, bool paced_ui)
{
    if (!swap_ || !rtv_) return;
    auto* ctx = gpu_->context.Get();
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(rtv_.Get(), black);

    if (video && vw && vh && ensure_video_views(video)) {
        // Stretch fills the window; otherwise letterbox to the stream's aspect
        // ratio. Whole-pixel edges keep a 1:1 picture from being resampled.
        D3D11_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
        if (!stretch_) {
            const double sa = double(vw) / vh, wa = double(width_) / height_;
            if (wa > sa) {
                vp.Width = std::round(height_ * sa);
                vp.TopLeftX = std::floor((width_ - vp.Width) / 2);
            } else {
                vp.Height = std::round(width_ / sa);
                vp.TopLeftY = std::floor((height_ - vp.Height) / 2);
            }
        }
        ctx->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {srv_y_.Get(), srv_uv_.Get()};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
    }

    if (draw2d) {
        d2d_->BeginDraw();
        painter_.begin(d2d_.Get(), dpi_scale_, time_s);
        draw2d(painter_, ui::Rect{0, 0, float(width_), float(height_)});
        painter_.end();
        if (d2d_->EndDraw() == D2DERR_RECREATE_TARGET) {
            release_targets();
            create_targets();
        }
    }

    const uint64_t t0 = now_us();
    UINT interval = 0, flags = 0;
    switch (paced_ui ? PresentMode::Smooth : mode_.load()) {
    case PresentMode::MinimumLatency: flags = tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0; break;
    case PresentMode::Balanced: break;
    case PresentMode::Smooth: interval = 1; break;
    }
    swap_->Present(interval, flags);
    present_hist_.record(now_us() - t0, now_us());
}

bool Renderer::save_screenshot(const std::wstring& path, ID3D11Texture2D* video, UINT vw, UINT vh)
{
    // Renders the current frame into an offscreen BGRA target and writes a PNG.
    if (!video || !vw || !vh || !ensure_video_views(video)) return false;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = vw;
    d.Height = vh;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt, staging;
    if (FAILED(gpu_->device->CreateTexture2D(&d, nullptr, &rt))) return false;
    ComPtr<ID3D11RenderTargetView> view;
    gpu_->device->CreateRenderTargetView(rt.Get(), nullptr, &view);
    auto* ctx = gpu_->context.Get();
    D3D11_VIEWPORT vp{0, 0, float(vw), float(vh), 0, 1};
    ctx->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {srv_y_.Get(), srv_uv_.Get()};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->Draw(3, 0);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(gpu_->device->CreateTexture2D(&d, nullptr, &staging))) return false;
    ctx->CopyResource(staging.Get(), rt.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

    bool ok = false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetSize(vw, vh);
        frame->SetPixelFormat(&fmt);
        ok = SUCCEEDED(frame->WritePixels(vh, m.RowPitch, m.RowPitch * vh, static_cast<BYTE*>(m.pData))) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    ctx->Unmap(staging.Get(), 0);
    return ok;
}

}  // namespace hikari

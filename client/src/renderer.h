#pragma once
// Presentation: flip-model swap chain + NV12->RGB pixel shader + D2D overlay.
//
// Present modes (measured trade-offs, see docs/latency-budget.md):
//   MinimumLatency  Present(0, ALLOW_TEARING): the frame reaches the screen
//                   immediately, may tear. Best with fullscreen independent flip.
//   Balanced        Present(0, 0): newest frame wins at the next vblank; older
//                   queued frames are discarded by the flip model (no tearing,
//                   at most one refresh of added latency).
//   Smooth          Present(1, 0) with a frame-latency waitable object and
//                   max latency 1: even pacing, still no queue build-up.

#include <atomic>
#include <functional>

#include "hikari/histogram.h"
#include "hikari/media.h"
#include "hikari/ui_kit.h"

namespace hikari {

enum class PresentMode { MinimumLatency = 0, Balanced = 1, Smooth = 2 };
const char* to_string(PresentMode m);

class Renderer {
public:
    using Draw2D = std::function<void(ui::Painter&, const ui::Rect& client)>;

    bool init(HWND hwnd, GpuDevice& gpu);
    void shutdown();
    void resize();
    void set_mode(PresentMode m) { mode_ = m; }
    PresentMode mode() const { return mode_; }
    // Stretch the picture to fill the window instead of letterboxing it.
    void set_stretch(bool on) { stretch_ = on; }
    bool stretch() const { return stretch_; }
    bool tearing_supported() const { return tearing_; }

    // Draws the last video frame (if any) letterboxed or stretched, then the 2D layer, then presents.
    // `video` may be null (lobby).
    void render(ID3D11Texture2D* video, UINT vw, UINT vh, const Draw2D& draw2d, double time_s, bool paced_ui = false);
    // Blocks until the swap chain can accept a frame (Smooth mode pacing).
    void wait_for_slot(DWORD timeout_ms);

    float dpi_scale() const { return dpi_scale_; }
    ui::Painter& painter() { return painter_; }
    LatencySummary present_cost(uint64_t now) { return present_hist_.summary(now); }
    bool save_screenshot(const std::wstring& path, ID3D11Texture2D* video, UINT vw, UINT vh);

private:
    bool create_targets();
    void release_targets();
    bool ensure_video_views(ID3D11Texture2D* tex);

    HWND hwnd_ = nullptr;
    GpuDevice* gpu_ = nullptr;
    ComPtr<IDXGISwapChain2> swap_;
    HANDLE waitable_ = nullptr;
    bool tearing_ = false;
    std::atomic<PresentMode> mode_{PresentMode::Balanced};
    std::atomic<bool> stretch_{false};
    UINT width_ = 0, height_ = 0;
    float dpi_scale_ = 1.0f;

    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ID3D11Texture2D* video_tex_ = nullptr;
    ComPtr<ID3D11ShaderResourceView> srv_y_, srv_uv_;

    ComPtr<ID2D1Factory1> d2d_factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<ID2D1DeviceContext> d2d_;
    ComPtr<ID2D1Bitmap1> d2d_target_;
    ui::Painter painter_;
    RollingHistogram present_hist_{2'000'000};
};

}  // namespace hikari

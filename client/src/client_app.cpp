#include "client_app.h"

#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "controller_settings.h"
#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/system.h"
#include "hk_link.h"

namespace hikari {

using proto::ConsoleState;
using proto::SessionPhase;
using ui::Align;
using ui::ButtonStyle;
using ui::Rect;
using ui::Weight;
namespace th = ui::theme;

namespace {
constexpr UINT WM_HIKARI_ACTION = WM_APP + 1;
constexpr UINT WM_HIKARI_REFRESH = WM_APP + 2;

std::string fmt(const char* f, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

LRESULT CALLBACK client_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* app = reinterpret_cast<ClientApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app ? app->handle_message(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

proto::Preset preset_from(const std::string& s)
{
    if (s == "latency") return proto::Preset::MinimumLatency;
    if (s == "quality") return proto::Preset::Quality;
    return proto::Preset::Balanced;
}
const char* preset_key(proto::Preset p)
{
    return p == proto::Preset::MinimumLatency ? "latency" : p == proto::Preset::Quality ? "quality" : "balanced";
}
PresentMode present_for(proto::Preset p)
{
    return p == proto::Preset::MinimumLatency ? PresentMode::MinimumLatency
           : p == proto::Preset::Quality      ? PresentMode::Smooth
                                              : PresentMode::Balanced;
}
uint32_t audio_ms_for(proto::Preset p)
{
    return p == proto::Preset::MinimumLatency ? 20 : p == proto::Preset::Quality ? 50 : 30;
}
uint64_t reorder_for(proto::Preset p)
{
    return p == proto::Preset::MinimumLatency ? 2000 : p == proto::Preset::Quality ? 6000 : 3500;
}
}  // namespace

ClientApp::ClientApp() = default;
ClientApp::~ClientApp() = default;

double ClientApp::now_s() const { return double(now_us() - start_qpc_us_) / 1e6; }

void ClientApp::load_settings()
{
    std::lock_guard<std::mutex> lk(mu_);
    preset_ = preset_from(cfg_->get("client", "preset", "balanced"));
    host_label_ = cfg_->get("client", "host_name");
    if (host_label_.empty()) host_label_ = cfg_->get("client", "host");
    mapping_ = InputMapping::defaults_positional();
    const std::string profile = cfg_->get("client", "mapping_profile", "Default");
    if (cfg_->has_section("mapping." + profile)) mapping_.load(*cfg_, "mapping." + profile);
    input_.set_mapping(mapping_);
    input_.set_refresh_ms(static_cast<uint32_t>(cfg_->get_int("client", "input_refresh_ms", 16)));
    input_.pin_device(cfg_->get("client", "controller"));
    const std::string pm = cfg_->get("client", "present_mode", "auto");
    renderer_.set_mode(pm == "latency" ? PresentMode::MinimumLatency
                       : pm == "smooth"  ? PresentMode::Smooth
                       : pm == "balanced" ? PresentMode::Balanced
                                          : present_for(preset_));
    audio_.set_target_ms(audio_ms_for(preset_));
    net_.set_reorder_window(reorder_for(preset_));
}

void ClientApp::connect()
{
    if (!hooks_.preview.empty()) return;
    const std::string host = cfg_->get("client", "host");
    if (host.empty()) return;
    net_.connect(host, cfg_->get("client", "pairing_code"), preset_,
                 static_cast<uint32_t>(cfg_->get_int("client", "max_kbps", 0)));
}

void ClientApp::preview_state(const std::string& state)
{
    std::lock_guard<std::mutex> lk(mu_);
    status_ = {};
    have_status_ = true;
    std::snprintf(status_.host_name, sizeof(status_.host_name), "Living room");
    status_.console = (state == "asleep" || state == "switch2") ? ConsoleState::Asleep : ConsoleState::Ready;
    status_.bridge_ok = status_.capture_ok = status_.encoder_ok = 1;
    status_.signal_ok = state != "asleep" && state != "switch2";
    status_.wake_methods = 3;
    if (state == "waking") {
        status_.signal_ok = 0;
        status_.console = ConsoleState::Waking;
        status_.phase = SessionPhase::WaitingForVideo;
        std::snprintf(status_.message, sizeof(status_.message), "Waiting for the Switch's picture");
    } else if (state == "failed") {
        status_.signal_ok = 0;
        status_.console = ConsoleState::AwakeNoVideo;
        status_.phase = SessionPhase::Failed;
        status_.issue = proto::HostIssue::NoSignal;
        std::snprintf(status_.message, sizeof(status_.message), "No HDMI signal. Check the dock and HDMI cable, then try again.");
    }
    screen_ = state == "stream" ? Screen::Stream : Screen::Lobby;
    show_stats_ = state == "stream";
}

int ClientApp::run(Config& cfg, ClientHooks hooks)
{
    cfg_ = &cfg;
    hooks_ = std::move(hooks);
    start_qpc_us_ = now_us();
    apply_process_performance_policy();

    if (!gpu_.create()) {
        MessageBoxW(nullptr, L"Hikari needs a DirectX 11 GPU with hardware video decoding.", L"Hikari", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = client_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"HikariClient";
    wc.hbrBackground = CreateSolidBrush(RGB(17, 18, 22));
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const bool compact_preview = hooks_.preview == "ready-compact";
    const int w = MulDiv(compact_preview ? 860 : 1280, dpi, 96), h = MulDiv(compact_preview ? 600 : 780, dpi, 96);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, hooks_.preview.empty() ? L"Hikari" : L"Hikari - Offline preview", WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - w) / 2,
                            wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return 1;
    if (!hooks_.preview.empty()) SetWindowTextW(hwnd_, widen("Hikari - Preview: " + hooks_.preview).c_str());
    // Dark title bar to match the UI.
    const BOOL dark = TRUE;
    using SetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (auto f = reinterpret_cast<SetAttr>(reinterpret_cast<void*>(
            GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"))))
        f(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    load_settings();

    if (hooks_.preview.empty()) {
    ClientNetEvents ev;
    ev.state = [this](ConnState s, const std::string&) {
        if (s != ConnState::Connected && screen_ == Screen::Stream) toast("Connection lost - reconnecting...", 2.0);
        PostMessageW(hwnd_, WM_HIKARI_REFRESH, 0, 0);
    };
    ev.stream_config = [this](const proto::StreamConfig& c) {
        std::lock_guard<std::mutex> lk(mu_);
        stream_ = c;
    };
    ev.host_status = [this](const proto::HostStatus& s) {
        std::lock_guard<std::mutex> lk(mu_);
        // The host left the stream (Stop, failure): fall back to the lobby.
        if (have_status_ && status_.phase == SessionPhase::Streaming && s.phase != SessionPhase::Streaming)
            PostMessageW(hwnd_, WM_HIKARI_ACTION, kActStop, 0);
        status_ = s;
        have_status_ = true;
    };
    ev.host_stats = [this](const proto::HostStats& s) {
        std::lock_guard<std::mutex> lk(mu_);
        host_stats_ = s;
    };
    ev.audio = [this](const proto::AudioPacket& a) { audio_.push(a); };
    net_.start(std::move(ev));
    net_.set_discovery(false);
    connect();

    input_.start([this](const proto::Input& in) { net_.send_input(in); },
                 [this](SwitchInput b) {
                     // Controller navigation in the lobby: A = Play, D-pad = presets.
                     if (screen_ != Screen::Lobby || !focused_) return;
                     if (b == SwitchInput::A) post_action(kActPlay);
                     if (b == SwitchInput::DpadLeft || b == SwitchInput::DpadRight) {
                         const int dir = b == SwitchInput::DpadLeft ? -1 : 1;
                         const int cur = static_cast<int>(preset_.load());
                         const int next = std::clamp(cur + dir, 0, 2);
                         if (next != cur) post_action(kActPresetLatency + next);
                     }
                 });
    audio_.start(audio_ms_for(preset_));
    }

    if (cfg.get_bool("client", "fullscreen", false)) toggle_fullscreen();
    ShowWindow(hwnd_, SW_SHOW);
    show_stats_ = cfg.get_bool("client", "stats_overlay", false);
    if (!hooks_.preview.empty()) preview_state(hooks_.preview);

    running_ = true;
    render_ = std::thread(&ClientApp::render_thread, this);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    running_ = false;
    if (render_.joinable()) render_.join();
    input_.set_sending(false);
    Sleep(20);
    input_.stop();
    net_.stop();
    audio_.stop();
    cfg.set_bool("client", "fullscreen", fullscreen_);
    cfg.set_bool("client", "stats_overlay", show_stats_);
    if (hooks_.preview.empty()) cfg.save();
    return 0;
}

void ClientApp::toast(const std::string& msg, double seconds)
{
    std::lock_guard<std::mutex> lk(mu_);
    toast_ = msg;
    toast_until_ = now_s() + seconds;
}

void ClientApp::post_action(int action) { PostMessageW(hwnd_, WM_HIKARI_ACTION, static_cast<WPARAM>(action), 0); }

void ClientApp::set_screen(Screen s)
{
    if (screen_.exchange(s) == s) return;
    if (s == Screen::Stream) {
        toast("Ctrl+Alt+Q  back to Hikari     F1  stats     F11  fullscreen", 4.0);
        last_mouse_move_us_ = now_us();
    }
    update_sending();
}

void ClientApp::update_sending()
{
    const bool background = cfg_->get_bool("client", "input_background", false);
    input_.set_sending(screen_ == Screen::Stream && (focused_ || background) && net_.state() == ConnState::Connected);
}

void ClientApp::do_action(int action)
{
    if (!hooks_.preview.empty()) {
        if (action == kActPlay || action == kActRetry) { preview_state("stream"); return; }
        if (action == kActStop) { preview_state("ready"); return; }
    }
    switch (action) {
    case kActPlay:
    case kActRetry:
        if (net_.state() != ConnState::Connected) {
            if (net_.state() == ConnState::Rejected) do_action(kActSettings);
            break;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (have_status_ && status_.phase != SessionPhase::Lobby && status_.phase != SessionPhase::Failed &&
                status_.phase != SessionPhase::Streaming)
                break;  // already starting
            status_.phase = SessionPhase::Waking;  // optimistic: show progress immediately
            status_.phase_elapsed_ms = 0;
        }
        need_keyframe_ = true;
        net_.play();
        break;
    case kActStop:
        net_.stop_stream();
        set_screen(Screen::Lobby);
        break;
    case kActConnection:
    case kActSettings:
        if (hooks_.open_settings && hooks_.open_settings(hwnd_)) {
            load_settings();
            connect();
        }
        break;
    case kActController:
        open_controller_settings();
        break;
    case kActWake: net_.power(proto::PowerAction::Wake); break;
    case kActSleep: net_.power(proto::PowerAction::Sleep); break;
    case kActPresetLatency:
    case kActPresetBalanced:
    case kActPresetQuality: {
        const auto p = static_cast<proto::Preset>(action - kActPresetLatency);
        cfg_->set("client", "preset", preset_key(p));
        cfg_->save();
        load_settings();
        net_.set_preset(p, static_cast<uint32_t>(cfg_->get_int("client", "max_kbps", 0)));
        break;
    }
    case kActFullscreen: toggle_fullscreen(); break;
    default: break;
    }
}

void ClientApp::open_controller_settings()
{
    ControllerSettingsDeps deps;
    if (hooks_.preview == "switch2") deps.model = joycon::Model::Switch2;
    deps.poll_pad = [this] { return input_.raw(); };
    deps.list_devices = [this] { return input_.devices(); };
    deps.select_device = [this](const std::string& name) {
        cfg_->set("client", "controller", name);
        input_.pin_device(name);
    };
    InputMapping m;
    {
        std::lock_guard<std::mutex> lk(mu_);
        m = mapping_;
    }
    if (show_controller_settings(hwnd_, *cfg_, m, deps)) {
        std::lock_guard<std::mutex> lk(mu_);
        mapping_ = m;
        input_.set_mapping(m);
    }
}

void ClientApp::toggle_fullscreen()
{
    // Borderless fullscreen: lets DWM hand us independent flip (no compositor
    // latency) while alt-tab stays instant.
    if (!fullscreen_) {
        GetWindowRect(hwnd_, &windowed_rect_);
        windowed_style_ = GetWindowLongW(hwnd_, GWL_STYLE);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, static_cast<LONG>(WS_POPUP | WS_VISIBLE));
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, windowed_style_ ? windowed_style_ : static_cast<LONG>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd_, nullptr, windowed_rect_.left, windowed_rect_.top, windowed_rect_.right - windowed_rect_.left,
                     windowed_rect_.bottom - windowed_rect_.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        fullscreen_ = false;
    }
    resize_pending_ = true;
}

void ClientApp::on_key(UINT vk, bool down)
{
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (down) {
        if (screen_ == Screen::Lobby && (vk == VK_TAB || vk == VK_SPACE || (vk == VK_RETURN && !alt))) {
            std::lock_guard<std::mutex> lk(mu_);
            auto focused = std::find_if(ui_.hits.begin(), ui_.hits.end(),
                [this](const auto& hit) { return hit.first == ui_.focus_id; });
            if (vk == VK_TAB && !ui_.hits.empty()) {
                const int direction = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
                const int count = static_cast<int>(ui_.hits.size());
                const int current = focused == ui_.hits.end() ? (direction > 0 ? -1 : 0)
                    : static_cast<int>(focused - ui_.hits.begin());
                ui_.focus_id = ui_.hits[(current + direction + count) % count].first;
            } else if (focused != ui_.hits.end()) {
                post_action(focused->first);
            }
            return;
        }
        if (vk == VK_F11 || (vk == VK_RETURN && alt)) {
            toggle_fullscreen();
            return;
        }
        if (vk == VK_F1) {
            show_stats_ = !show_stats_;
            return;
        }
        if (vk == VK_F2) {
            const auto next = static_cast<PresentMode>((static_cast<int>(renderer_.mode()) + 1) % 3);
            renderer_.set_mode(next);
            cfg_->set("client", "present_mode",
                      next == PresentMode::MinimumLatency ? "latency" : next == PresentMode::Smooth ? "smooth" : "balanced");
            cfg_->save();
            toast(std::string("Display: ") + to_string(next));
            return;
        }
        if (vk == VK_F10) {
            input_.mark_next_latency();
            toast("Latency marker sent");
            return;
        }
        if (vk == VK_F12) {
            screenshot_ = true;
            return;
        }
        if (vk == 'Q' && ctrl && alt) {
            post_action(kActStop);
            return;
        }
    }
    if (vk == VK_F8) input_.set_injected(down ? HK_BTN_HOME : 0);
    if (vk == VK_F9) input_.set_injected(down ? HK_BTN_CAPTURE : 0);
    if (screen_ == Screen::Stream && vk < 256) {
        KeyboardState kb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            keys_.down.set(vk, down);
            kb = keys_;
        }
        input_.set_keyboard(kb);
    }
}

LRESULT ClientApp::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        resize_pending_ = true;
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        const UINT dpi = GetDpiForWindow(hwnd);
        mm->ptMinTrackSize = {MulDiv(860, dpi, 96), MulDiv(600, dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_MOUSEMOVE: {
        std::lock_guard<std::mutex> lk(mu_);
        ui_.mouse_x = static_cast<float>(GET_X_LPARAM(lp));
        ui_.mouse_y = static_cast<float>(GET_Y_LPARAM(lp));
        last_mouse_move_us_ = now_us();
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: {
        std::lock_guard<std::mutex> lk(mu_);
        ui_.mouse_x = ui_.mouse_y = -1;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        std::lock_guard<std::mutex> lk(mu_);
        ui_.mouse_down = true;
        ui_.pressed_id = ui_.hit(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
        if (ui_.pressed_id >= 0) ui_.focus_id = ui_.pressed_id;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: {
        std::lock_guard<std::mutex> lk(mu_);
        const int id = ui_.hit(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
        if (id >= 0 && id == ui_.pressed_id) ui_.clicked_id = id;
        ui_.mouse_down = false;
        ui_.pressed_id = -1;
        ReleaseCapture();
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (screen_ == Screen::Stream) toggle_fullscreen();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && screen_ == Screen::Stream && now_us() - last_mouse_move_us_ > 2'000'000) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!(lp & (1 << 30))) on_key(static_cast<UINT>(wp), true);  // ignore auto-repeat
        if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;  // no menu beep / Alt menu
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        on_key(static_cast<UINT>(wp), false);
        return 0;
    case WM_SETFOCUS:
        focused_ = true;
        update_sending();
        return 0;
    case WM_KILLFOCUS: {
        focused_ = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            keys_ = KeyboardState{};
        }
        input_.set_keyboard(KeyboardState{});
        input_.set_injected(0);
        update_sending();
        return 0;
    }
    case WM_HIKARI_ACTION:
        do_action(static_cast<int>(wp));
        return 0;
    case WM_HIKARI_REFRESH:
        if (net_.state() != ConnState::Connected && screen_ == Screen::Stream) update_sending();
        if (lp == 1) set_screen(Screen::Stream);
        update_sending();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        running_ = false;
        if (render_.joinable()) render_.join();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- render thread -------------------------------------------------------------

void ClientApp::render_thread()
{
    set_thread_role(ThreadRole::Video, "hikari-render");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!renderer_.init(hwnd_, gpu_)) {
        HK_LOGE("Renderer init failed");
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        renderer_.shutdown();
        CoUninitialize();
        return;
    }
    ComPtr<ID3D11Texture2D> picture;
    UINT pic_w = 0, pic_h = 0;
    uint64_t last_render = 0;
    uint64_t next_feedback = 0;

    while (running_) {
        if (resize_pending_.exchange(false)) renderer_.resize();
        proto::StreamConfig sc;
        SessionPhase phase;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sc = stream_;
            phase = have_status_ ? status_.phase : SessionPhase::Lobby;
        }

        // Decode everything that arrived; show only the newest picture.
        bool fresh = false;
        uint64_t fresh_capture = 0;
        while (auto item = net_.frames().pop()) {
            std::unique_ptr<AssembledFrame> f = std::move(*item);
            if (!decoder_.is_open() || decoder_.codec() != sc.codec) {
                if (!decoder_.open(gpu_, sc.codec)) {
                    net_.recycle(std::move(f));
                    continue;
                }
                need_keyframe_ = true;
            }
            const bool key = (f->flags & proto::kVideoKeyframe) != 0;
            if (need_keyframe_ && !key) {
                ++decoder_drops_;
                net_.recycle(std::move(f));
                continue;
            }
            if (key) need_keyframe_ = false;
            DecodedFrame out;
            const bool ok = decoder_.decode(f->data.data(), f->data.size(), f->capture_us, out);
            if (decoder_.failed()) {
                HK_LOGW("Decoder error; resetting and requesting a keyframe");
                decoder_.open(gpu_, sc.codec);
                need_keyframe_ = true;
                net_.request_keyframe(2);
            } else if (ok) {
                picture = out.texture;
                pic_w = out.width;
                pic_h = out.height;
                fresh = true;
                fresh_capture = f->capture_us;
                decode_rate_.add();
                decode_hist_.record(out.decode_us, now_us());
            }
            net_.recycle(std::move(f));
        }

        // First picture of a started stream: switch from the lobby to video.
        if (fresh && screen_ == Screen::Lobby && phase == SessionPhase::Streaming)
            PostMessageW(hwnd_, WM_HIKARI_REFRESH, 0, 1);

        const uint64_t now = now_us();
        const bool streaming = screen_ == Screen::Stream;
        double toast_until;
        {
            std::lock_guard<std::mutex> lk(mu_);
            toast_until = toast_until_;
        }
        const bool overlay_live = !hooks_.preview.empty() || show_stats_ || now_s() < toast_until || net_.state() != ConnState::Connected;

        if (streaming) {
            if (fresh || (overlay_live && now - last_render > 50'000)) {
                if (renderer_.mode() == PresentMode::Smooth) renderer_.wait_for_slot(20);
                renderer_.render(picture.Get(), pic_w, pic_h,
                                 [this](ui::Painter& p, const Rect& r) { draw_stream_overlay(p, r); }, now_s());
                last_render = now_us();
                if (fresh) {
                    present_rate_.add();
                    if (net_.clock().valid()) {
                        const uint64_t cap_local = net_.clock().to_local(fresh_capture);
                        if (last_render > cap_local && last_render - cap_local < 2'000'000) {
                            const uint64_t e2e = last_render - cap_local;
                            e2e_.record(e2e, last_render);
                            e2e_spark_[spark_pos_++ % 120] = e2e / 1000.0f;
                        }
                    }
                }
            } else {
                WaitForSingleObject(net_.frames().event(), 4);
            }
        } else {
            // Lobby: vsync-paced UI (low CPU), video frames keep decoding behind it.
            renderer_.wait_for_slot(20);
            renderer_.render(nullptr, 0, 0, [this](ui::Painter& p, const Rect& r) { draw_lobby(p, r); }, now_s(), true);
        }

        if (screenshot_.exchange(false) && picture) {
            wchar_t* pics = nullptr;
            std::wstring dir;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) dir = std::wstring(pics) + L"\\Hikari";
            CoTaskMemFree(pics);
            CreateDirectoryW(dir.c_str(), nullptr);
            wchar_t name[64];
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            wcsftime(name, 64, L"\\Hikari_%Y%m%d_%H%M%S.png", &tm);
            toast(renderer_.save_screenshot(dir + name, picture.Get(), pic_w, pic_h) ? "Screenshot saved to Pictures\\Hikari"
                                                                                 : "Screenshot failed");
        }

        if (now >= next_feedback) {
            next_feedback = now + 100'000;
            present_fps_ = present_rate_.sample(now);
            decode_fps_ = decode_rate_.sample(now);
            net_.set_decode_feedback(static_cast<uint16_t>(net_.frames().size()),
                                     static_cast<uint16_t>(std::min<uint32_t>(decode_hist_.summary(now).p95, 65535)),
                                     present_fps_, audio_.buffered_ms());
        }
    }
    decoder_.close();
    renderer_.shutdown();
    CoUninitialize();
}

// ---- lobby -----------------------------------------------------------------

void ClientApp::draw_lobby(ui::Painter& p, const Rect& r)
{
    std::lock_guard<std::mutex> lk(mu_);
    ui_.begin_frame();
    const ConnState conn = hooks_.preview.empty() ? net_.state()
        : hooks_.preview == "connection-needed" ? ConnState::Idle : ConnState::Connected;
    const bool connected = conn == ConnState::Connected;
    const proto::HostStatus st = status_;
    const bool have = have_status_ && connected;
    p.background(r);
    const float m = p.s(24), right = r.r-m;
    p.text("Remote play", {m,p.s(18),right,p.s(50)},p.s(22),ui::rgb(th::kText),Weight::SemiBold);
    p.text("Your console, connection and stream preferences.", {m,p.s(52),right,p.s(76)},p.s(13),ui::rgb(th::kText2));
    if(p.button(ui_,kActController,{right-p.s(236),p.s(24),right-p.s(124),p.s(60)},"Controller")) post_action(kActController);
    if(p.button(ui_,kActConnection,{right-p.s(112),p.s(24),right,p.s(60)},"Connection")) post_action(kActSettings);

    const float split = m+(r.w()-m*3)*0.62f, side=split+p.s(16);
    const float top=p.s(100), bottom=r.b-p.s(52), session_bottom=top+p.s(220);
    p.fill({m,top,split,session_bottom},ui::rgb(th::kBg2),p.s(12));
    p.fill({m,session_bottom+p.s(16),split,bottom},ui::rgb(th::kBg2),p.s(12));
    p.fill({side,top,right,bottom},ui::rgb(th::kBg2),p.s(12));
    const float x=m+p.s(22), inner=split-p.s(22), sx=side+p.s(22), sr=right-p.s(22);
    const bool awake=have && st.console==ConsoleState::Ready;
    const std::string host=have && st.host_name[0] ? st.host_name : host_label_;
    const float art_width=std::clamp((inner-x)*0.44f,p.s(174),p.s(300));
    const float text_right=inner-art_width-p.s(20);
    const bool switch2_preview=hooks_.preview=="switch2";
    const std::string console_name=switch2_preview?"Switch 2":"Switch";
    p.text(host.empty()?"Your "+console_name:host+"'s "+console_name,{x,top+p.s(18),text_right,top+p.s(48)},p.s(18),ui::rgb(th::kText),Weight::SemiBold);
    std::string state="Waiting for the host";
    if(have) switch(st.console) {
    case ConsoleState::Ready: state="Ready to play"; break;
    case ConsoleState::Asleep: state="Asleep. Play will wake your console."; break;
    case ConsoleState::Off: state="Off or undocked"; break;
    case ConsoleState::Waking: state="Waking your console"; break;
    case ConsoleState::AwakeNoVideo: state="Waiting for an HDMI signal"; break;
    default: state="Checking your console"; break;
    }
    p.text(state,{x,top+p.s(52),text_right,top+p.s(92)},p.s(13),ui::rgb(th::kText2),Weight::Regular,Align::Leading,Align::Leading,true);
    p.console({inner-art_width,top+p.s(18),inner,session_bottom-p.s(18)},awake?th::kGood:th::kWarn,
              switch2_preview?ui::ConsoleModel::Switch2:ui::ConsoleModel::Switch);
    const bool progress=have && st.phase!=SessionPhase::Lobby && st.phase!=SessionPhase::Failed;
    float y=top+p.s(100);
    if(progress) {
        const int current=st.phase==SessionPhase::Waking?0:st.phase==SessionPhase::WaitingForVideo?1:st.phase==SessionPhase::WaitingForController?2:3;
        const char* steps[]={"Wake","Picture","Controller","Stream"};
        const float step=(text_right-x)/4;
        const bool compact_steps=text_right-x<p.s(330);
        if(compact_steps) {
            p.spinner(x+p.s(8),y+p.s(10),p.s(7),ui::rgb(th::kAccent));
            p.text(steps[current],{x+p.s(24),y,text_right,y+p.s(24)},p.s(13),ui::rgb(th::kText));
        } else for(int i=0;i<4;++i) {
            const float cx=x+step*i+p.s(8);
            if(i<3) p.line(cx+p.s(12),y+p.s(8),cx+step-p.s(12),y+p.s(8),ui::rgb(i<current?th::kAccent:th::kBorder),p.s(1));
            if(i==current) p.spinner(cx,y+p.s(8),p.s(7),ui::rgb(th::kAccent));
            else p.circle(cx,y+p.s(8),p.s(4),ui::rgb(i<current?th::kAccent:th::kBorder));
            p.text(steps[i],{cx-p.s(8),y+p.s(21),cx+step-p.s(10),y+p.s(40)},p.s(11),ui::rgb(i<=current?th::kText:th::kText3));
        }
        const float message_y=y+p.s(compact_steps?28:42);
        p.text(st.message[0]?st.message:proto::to_string(st.phase),{x,message_y,text_right,message_y+p.s(30)},p.s(12),ui::rgb(th::kText2),Weight::Regular,Align::Leading,Align::Leading,true);
        const float cancel_y=y+p.s(compact_steps?64:76);
        if(p.button(ui_,kActStop,{x,cancel_y,x+p.s(88),cancel_y+p.s(34)},"Cancel")) post_action(kActStop);
    } else {
        const bool failed=have && st.phase==SessionPhase::Failed;
        const bool setup=conn==ConnState::Rejected || conn==ConnState::Idle;
        const int action=setup?kActSettings:failed?kActRetry:kActPlay;
        if (ui_.focus_id < 0) ui_.focus_id=action;
        if(failed) {
            p.text(st.message[0]?st.message:"Could not start the session",{x,y-p.s(12),text_right,y+p.s(34)},p.s(13),ui::rgb(th::kBad),Weight::Regular,Align::Leading,Align::Center,true);
            y+=p.s(40);
        }
        if(p.button(ui_,action,{x,y,x+p.s(164),y+p.s(38)},setup?"Set up connection":failed?"Try again":"Play",ButtonStyle::Primary,setup||have)) post_action(action);
        p.text(setup?"Enter your host address and pairing code.":failed?"Check the host, then retry.":have?"Or press A on your controller.":"Keep the host app running on your other PC.",
               {x,y+p.s(46),text_right,y+p.s(80)},p.s(12),ui::rgb(th::kText3),Weight::Regular,Align::Leading,Align::Leading,true);
        if(have && !failed && st.issue!=proto::HostIssue::None)
            p.text(proto::to_string(st.issue),{x,y+p.s(82),text_right,y+p.s(106)},p.s(12),ui::rgb(th::kWarn));
    }
    const float prefs=session_bottom+p.s(16), row_start=prefs+p.s(60);
    p.text("Stream preference",{x,prefs+p.s(16),inner,prefs+p.s(42)},p.s(15),ui::rgb(th::kText),Weight::SemiBold);
    const char* names[]={"Minimum latency","Balanced","Quality"};
    const char* descriptions[]={"Prioritize response time.","Balance responsiveness and picture quality.","Prioritize picture quality."};
    const float pitch=std::min(p.s(64),(bottom-row_start-p.s(16))/3);
    for(int i=0;i<3;++i) {
        const Rect row{x-p.s(6),row_start+pitch*i,inner+p.s(6),row_start+pitch*(i+1)-p.s(4)};
        const bool selected=static_cast<int>(preset_.load())==i;
        if(p.button(ui_,kActPresetLatency+i,row,"",selected?ButtonStyle::Secondary:ButtonStyle::Ghost)) post_action(kActPresetLatency+i);
        const float cx=x+p.s(12),cy=row.cy();
        p.circle(cx,cy,p.s(7),ui::rgb(selected?th::kAccent:th::kText3),false,p.s(1));
        if(selected) p.circle(cx,cy,p.s(3),ui::rgb(th::kAccent));
        if(pitch<p.s(48)) {
            p.text(names[i],{x+p.s(32),row.t,inner,row.b},p.s(13),ui::rgb(th::kText),Weight::Medium);
        } else {
            p.text(names[i],{x+p.s(32),row.t+p.s(4),inner,row.cy()+p.s(2)},p.s(13),ui::rgb(th::kText),Weight::Medium);
            p.text(descriptions[i],{x+p.s(32),row.cy(),inner,row.b-p.s(3)},p.s(12),ui::rgb(th::kText2));
        }
    }
    p.text("Host connection",{sx,top+p.s(18),sr,top+p.s(44)},p.s(15),ui::rgb(th::kText),Weight::SemiBold);
    p.pill(sx,top+p.s(56),connected?"Connected":to_string(conn),connected?th::kGood:th::kWarn);
    p.text(host.empty()?"No host configured":host,{sx,top+p.s(96),sr,top+p.s(120)},p.s(13),ui::rgb(th::kText));
    p.line(sx,top+p.s(140),sr,top+p.s(140),ui::rgb(th::kBorder),p.s(1));
    const char* labels[]={"Controller bridge","Capture card","HDMI signal","Hardware encoder"};
    const bool checks[]={bool(st.bridge_ok),bool(st.capture_ok),bool(st.signal_ok),bool(st.encoder_ok)};
    const bool can_sleep=have && (st.wake_methods&4) && awake;
    const float check_pitch=can_sleep?std::clamp((bottom-top-p.s(276))/3,p.s(40),p.s(62)):p.s(62);
    for(int i=0;i<4;++i) {
        const float ry=top+p.s(158)+check_pitch*i;
        p.text(labels[i],{sx,ry,sr,ry+p.s(22)},p.s(13),ui::rgb(th::kText));
        p.circle(sx+p.s(4),ry+p.s(35),p.s(3),ui::rgb(!have?th::kText3:checks[i]?th::kGood:th::kWarn));
        p.text(!have?"Waiting for host":checks[i]?"Ready":"Not ready",{sx+p.s(15),ry+p.s(24),sr,ry+p.s(46)},p.s(12),ui::rgb(th::kText2));
    }
    if(can_sleep)
        if(p.button(ui_,kActSleep,{sx,bottom-p.s(54),sr,bottom-p.s(18)},"Put console to sleep")) post_action(kActSleep);
    p.logo({m,r.b-p.s(34),m+p.s(18),r.b-p.s(16)});
    p.text(!hooks_.preview.empty()?"Offline preview - Sample device status":"Hikari",{m+p.s(26),r.b-p.s(38),right,r.b-p.s(12)},p.s(12),ui::rgb(th::kText3));
    // State changes can replace Play with Retry, Settings or Cancel.
    if (std::none_of(ui_.hits.begin(), ui_.hits.end(),
            [this](const auto& hit) { return hit.first == ui_.focus_id; })) {
        const int preferred = progress ? kActStop : conn == ConnState::Rejected || conn == ConnState::Idle
            ? kActSettings : have && st.phase == SessionPhase::Failed ? kActRetry : kActPlay;
        const auto next = std::find_if(ui_.hits.begin(), ui_.hits.end(),
            [preferred](const auto& hit) { return hit.first == preferred; });
        ui_.focus_id = next != ui_.hits.end() ? preferred : ui_.hits.empty() ? -1 : ui_.hits.front().first;
    }
    ui_.take_click();
}

// ---- stream overlay -----------------------------------------------------------

void ClientApp::draw_stream_overlay(ui::Painter& p, const Rect& r)
{
    if (!hooks_.preview.empty()) {
        p.background(r);
        p.text("Stream preview", {0, r.cy() - p.s(38), r.r, r.cy()}, p.s(30), ui::rgb(th::kText), Weight::SemiBold, Align::Center);
        p.text("No live video - connect a host to see your game", {0, r.cy() + p.s(8), r.r, r.cy() + p.s(38)}, p.s(15), ui::rgb(th::kText2), Weight::Regular, Align::Center);
    }
    std::string toast;
    double until;
    {
        std::lock_guard<std::mutex> lk(mu_);
        toast = toast_;
        until = toast_until_;
    }
    if (show_stats_) draw_stats(p, r);

    if (hooks_.preview.empty() && net_.state() != ConnState::Connected) {
        p.fill(r, ui::rgb(0x000000, 0.55f));
        p.spinner(r.cx(), r.cy() - p.s(20), p.s(22), ui::rgb(th::kAccent));
        p.text("Reconnecting...", Rect{r.l, r.cy() + p.s(16), r.r, r.cy() + p.s(46)}, p.s(18), ui::rgb(th::kText),
               Weight::SemiBold, Align::Center);
    }
    if (now_s() < until && !toast.empty()) {
        const float alpha = static_cast<float>(std::min(1.0, (until - now_s()) * 2));
        const float w = p.text_width(toast, p.s(14), Weight::Medium) + p.s(40);
        const Rect t{r.cx() - w / 2, r.b - p.s(80), r.cx() + w / 2, r.b - p.s(40)};
        p.fill(t, ui::rgb(0x111216, 0.85f * alpha), p.s(20));
        p.text(toast, t, p.s(14), ui::rgb(th::kText, alpha), Weight::Medium, Align::Center);
    }
}

void ClientApp::draw_stats(ui::Painter& p, const Rect& r)
{
    const uint64_t now = now_us();
    proto::HostStats hs;
    proto::StreamConfig sc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        hs = host_stats_;
        sc = stream_;
    }
    const ClientNetStats ns = net_.stats();
    const LatencySummary e2e = e2e_.summary(now);
    const LatencySummary dec = decode_hist_.summary(now);
    auto ms = [](uint32_t us) { return us / 1000.0; };

    std::vector<std::pair<std::string, std::string>> rows = {
        {"Stream", fmt("%ux%u @ %u  |  %.1f Mbps  |  %s", hs.width, hs.height, hs.fps, ns.mbps,
                       sc.codec == proto::VideoCodec::HEVC ? "HEVC" : "H.264")},
        {"Encoder", hs.encoder_name[0] ? hs.encoder_name : "-"},
        {"Network", fmt("RTT %.1f ms  |  jitter %.1f ms  |  loss %.2f%%  |  queue %.1f ms", ns.rtt_ms, ns.jitter_ms,
                        ns.loss_pct, ns.queue_delay_ms)},
        {"Glass-to-glass*", e2e.count ? fmt("%.1f / %.1f / %.1f ms  (p50/p95/p99)", ms(e2e.p50), ms(e2e.p95), ms(e2e.p99))
                                      : "syncing clocks..."},
        {"Host", fmt("capture->sent %.1f/%.1f ms  |  encode %.1f/%.1f ms", ms(hs.capture_to_send_p50),
                     ms(hs.capture_to_send_p95), ms(hs.encode_p50), ms(hs.encode_p95))},
        {"Decode", fmt("%.1f / %.1f ms  |  present %s", ms(dec.p50), ms(dec.p95), to_string(renderer_.mode()))},
        {"Input", fmt("PC->host %.1f/%.1f ms  |  host->bridge %.2f ms  |  bridge->Switch %.2f/%.2f ms",
                      hs.input_owd_p50 / 1000.0, hs.input_owd_p95 / 1000.0, hs.input_host_p50 / 1000.0,
                      hs.esp_rx_to_console_p50 / 1000.0, hs.esp_rx_to_console_p99 / 1000.0)},
        {"Controller", fmt("%s  |  %.0f Hz sent  |  bridge %s, %u reports/s",
                           input_.active_device().empty() ? "none" : input_.active_device().c_str(), input_.send_rate(),
                           hs.esp_connected ? (hs.esp_failsafe ? "failsafe" : "ok") : "offline", hs.esp_reports_per_s)},
        {"FPS", fmt("capture %.1f  |  encode %.1f  |  decode %.1f  |  present %.1f", hs.capture_fps_x100 / 100.0,
                    hs.encode_fps_x100 / 100.0, decode_fps_, present_fps_)},
        {"Drops", fmt("capture %u  |  sender %u  |  network %llu (FEC fixed %llu)  |  decoder %llu", hs.capture_drops,
                      hs.encode_drops, static_cast<unsigned long long>(ns.frames_dropped),
                      static_cast<unsigned long long>(ns.frames_fec_recovered),
                      static_cast<unsigned long long>(decoder_drops_))},
        {"Audio", fmt("buffer %u ms  |  underruns %llu", audio_.buffered_ms(),
                      static_cast<unsigned long long>(audio_.underruns()))},
    };

    const float lh = p.s(21), lw = p.s(118);
    const float w = std::min(r.w() - p.s(32), p.s(700));
    const Rect panel{p.s(16), p.s(16), p.s(16) + w, p.s(16) + lh * rows.size() + p.s(78)};
    p.fill(panel, ui::rgb(0x0B0C0F, 0.78f), p.s(12));
    float y = panel.t + p.s(10);
    for (auto& [k, v] : rows) {
        p.text(k, Rect{panel.l + p.s(14), y, panel.l + lw, y + lh}, p.s(12.5f), ui::rgb(th::kText2), Weight::Medium);
        p.text(v, Rect{panel.l + lw, y, panel.r - p.s(12), y + lh}, p.s(12.5f), ui::rgb(th::kText), Weight::Regular);
        y += lh;
    }
    // Sparkline of glass-to-glass latency (last 120 frames, 0-100 ms).
    const Rect sp{panel.l + p.s(14), y + p.s(8), panel.r - p.s(14), panel.b - p.s(22)};
    p.fill(sp, ui::rgb(0xFFFFFF, 0.04f), p.s(4));
    for (int i = 1; i < 120; ++i) {
        const float a = e2e_spark_[(spark_pos_ + i - 1) % 120], b = e2e_spark_[(spark_pos_ + i) % 120];
        if (a <= 0 || b <= 0) continue;
        const float x0 = sp.l + sp.w() * (i - 1) / 119.0f, x1 = sp.l + sp.w() * i / 119.0f;
        p.line(x0, sp.b - sp.h() * std::min(a, 100.0f) / 100.0f, x1, sp.b - sp.h() * std::min(b, 100.0f) / 100.0f,
               ui::rgb(th::kAccent), p.s(1.5f));
    }
    p.text("* capture card -> this screen, excluding display scan-out.  F1 hides this panel.",
           Rect{panel.l + p.s(14), panel.b - p.s(20), panel.r, panel.b - p.s(4)}, p.s(10.5f), ui::rgb(th::kText3));
}

}  // namespace hikari

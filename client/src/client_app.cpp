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
#include "hikari/tailscale.h"
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
constexpr UINT WM_HIKARI_MENU_NAV = WM_APP + 3;  // wParam: SwitchInput pressed while the stream menu is open

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
    switch2_ = hooks_.preview == "switch2" || cfg_->get("client", "console_model") == "switch2";
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
                 static_cast<uint32_t>(cfg_->get_int("client", "max_kbps", 0)),
                 cfg_->get("client", "host_fallback"));
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
    const bool stream = state.rfind("stream", 0) == 0;
    screen_ = stream ? Screen::Stream : Screen::Lobby;
    show_stats_ = state == "stream";
    menu_open_ = state == "stream-menu" || state == "stream-end";
    menu_confirm_ = state == "stream-end";
    menu_sel_ = menu_confirm_ ? 1 : 0;
    ended_by_user_ = state == "ended";
    for (int i = 0; i < 120; ++i)  // sample latency trace for the offline stats preview
        e2e_spark_[i] = 34.0f + 3.5f * std::sin(i * 0.31f) + 1.8f * std::sin(i * 1.7f) + (i % 37 == 0 ? 9.0f : 0.0f);
    spark_pos_ = 0;
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
    wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x0F, 0x11));
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const bool compact_preview = hooks_.preview == "ready-compact";
    const int w = MulDiv(compact_preview ? 860 : 1120, dpi, 96), h = MulDiv(compact_preview ? 600 : 700, dpi, 96);
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
    ui::style_window(hwnd_);

    load_settings();

    if (hooks_.preview.empty()) {
    ClientNetEvents ev;
    ev.state = [this](ConnState s, const std::string&) {
        if (s == ConnState::Replaced) {
            toast("Another device took over your host", 4.0);
            PostMessageW(hwnd_, WM_HIKARI_ACTION, kActStop, 0);
        } else if (s != ConnState::Connected && screen_ == Screen::Stream) {
            toast("Connection lost - reconnecting...", 2.0);
        }
        if (s == ConnState::Connected) path_wake_.set();
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
                     if (screen_ == Screen::Stream && menu_open_) {
                         PostMessageW(hwnd_, WM_HIKARI_MENU_NAV, static_cast<WPARAM>(b), 0);
                         return;
                     }
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
    input_.set_menu_chord([this] {
        if (screen_ == Screen::Stream && focused_) post_action(kActMenu);
    });
    audio_.start(audio_ms_for(preset_));
    }

    if (cfg.get_bool("client", "fullscreen", false)) toggle_fullscreen();
    ShowWindow(hwnd_, SW_SHOW);
    show_stats_ = cfg.get_bool("client", "stats_overlay", false);
    if (!hooks_.preview.empty()) preview_state(hooks_.preview);

    running_ = true;
    render_ = std::thread(&ClientApp::render_thread, this);
    if (hooks_.preview.empty()) path_ = std::thread(&ClientApp::path_thread, this);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    running_ = false;
    path_wake_.set();
    if (render_.joinable()) render_.join();
    if (path_.joinable()) path_.join();
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

void ClientApp::path_thread()
{
    // Tailscale falls back to its DERP relays when the two PCs cannot reach
    // each other directly; that works but adds latency and caps bandwidth.
    // Ask the local Tailscale client which route we got and say so.
    set_thread_role(ThreadRole::Background, "hikari-path");
    while (running_) {
        int state = 0;
        std::string relay;
        const std::string ip = net_.state() == ConnState::Connected ? net_.remote_ip() : std::string();
        if (is_tailscale_ipv4(ip)) {
            const TailscalePath path = tailscale_path_to(ip);
            if (path.known) {
                state = path.direct ? 1 : 2;
                relay = path.relay;
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            path_relay_ = relay;
        }
        const int before = path_state_.exchange(state);
        if (state == 2 && before != 2 && !relay_toast_shown_) {
            relay_toast_shown_ = true;
            toast("Tailscale is relaying this connection - expect extra latency", 5.0);
        }
        // Direct paths usually form within seconds of traffic starting, so
        // look again soon while relayed, rarely once direct.
        path_wake_.wait(state == 1 ? 60000 : state == 2 ? 10000 : 5000);
    }
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
        toast(mouse_aim_enabled() ? "Stream menu: Ctrl+Alt+M, or hold Minus and Plus     Click  mouse aim"
                                  : "Stream menu: Ctrl+Alt+M, or hold Minus and Plus",
              5.0);
        last_mouse_move_us_ = now_us();
        ended_by_user_ = false;
    } else {
        menu_open_ = false;
    }
    update_sending();
}

bool ClientApp::mouse_aim_enabled()
{
    std::lock_guard<std::mutex> lk(mu_);
    return mapping_.keyboard_enabled && mapping_.mouse_aim;
}

void ClientApp::set_mouse_captured(bool on)
{
    if (on == mouse_captured_) return;
    mouse_captured_ = on;
    input_.set_mouse_captured(on);
    if (on) {
        clip_cursor();
        SetCursor(nullptr);
        toast("Mouse captured     Esc  release", 2.5);
    } else {
        ClipCursor(nullptr);
        last_mouse_move_us_ = now_us();  // show the pointer again right away
    }
}

void ClientApp::clip_cursor()
{
    RECT rc;
    GetClientRect(hwnd_, &rc);
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&rc), 2);
    ClipCursor(&rc);
}

void ClientApp::update_sending()
{
    if (screen_ != Screen::Stream || menu_open_ || !focused_ || !mouse_aim_enabled()) set_mouse_captured(false);
    const bool background = cfg_->get_bool("client", "input_background", false);
    input_.set_sending(screen_ == Screen::Stream && !menu_open_ && (focused_ || background) && net_.state() == ConnState::Connected);
}

void ClientApp::do_action(int action)
{
    if (!hooks_.preview.empty()) {
        if (action == kActPlay || action == kActRetry) { preview_state("stream"); return; }
        if (action == kActStop) { preview_state("ready"); return; }
        if (action == kActScreenshot) { toast("Screenshots need live video"); return; }
        if (action == kActEndConfirm) { preview_state("ended"); return; }
    }
    switch (action) {
    case kActPlay:
    case kActRetry:
        ended_by_user_ = false;
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
    case kActChangeRole:
        if (hooks_.change_role && hooks_.change_role(hwnd_)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    case kActTakeOver:
        connect();  // the host hands the session to whoever connects last
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
    case kActStats: show_stats_ = !show_stats_; break;
    case kActScreenshot: screenshot_ = true; break;
    case kActMenu:
        if (menu_open_) close_menu();
        else open_menu(false);
        break;
    case kActResume: close_menu(); break;
    case kActEndAsk:
    case kActEndCancel: {
        std::lock_guard<std::mutex> lk(mu_);
        menu_confirm_ = action == kActEndAsk;
        menu_sel_ = menu_confirm_ ? 1 : 4;  // confirmation defaults to End stream; cancel returns to it
        break;
    }
    case kActEndConfirm:
        // Inputs are already released (the menu paused them); the host keeps the session.
        close_menu();
        ended_by_user_ = true;
        do_action(kActStop);
        toast("Stream ended. Press Resume or A to continue.", 4.0);
        break;
    default: break;
    }
}

void ClientApp::open_controller_settings()
{
    ControllerSettingsDeps deps;
    if (switch2_) deps.model = joycon::Model::Switch2;
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
        {
            std::lock_guard<std::mutex> lk(mu_);
            mapping_ = m;
            input_.set_mapping(m);
        }
        update_sending();
    }
    switch2_ = cfg_->get("client", "console_model") == "switch2";
}

void ClientApp::open_menu(bool confirm_end)
{
    if (screen_ != Screen::Stream) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        menu_confirm_ = confirm_end;
        menu_sel_ = confirm_end ? 1 : 0;
        keys_ = KeyboardState{};
    }
    menu_open_ = true;
    // Nothing held on the keyboard or controller stays pressed on the console.
    input_.set_keyboard(KeyboardState{});
    input_.set_injected(0);
    update_sending();
}

void ClientApp::close_menu()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        menu_confirm_ = false;
    }
    menu_open_ = false;
    update_sending();
}

std::vector<int> ClientApp::menu_items()
{
    if (menu_confirm_) return {kActEndCancel, kActEndConfirm};
    return {kActResume, kActStats, kActController, kActFullscreen, kActEndAsk};
}

void ClientApp::menu_key(UINT vk)
{
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
    int activate = kActNone;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const std::vector<int> items = menu_items();
        const int n = static_cast<int>(items.size());
        menu_sel_ = std::clamp(menu_sel_, 0, n - 1);
        const bool horizontal = menu_confirm_;
        if (vk == (horizontal ? VK_LEFT : VK_UP)) menu_sel_ = (menu_sel_ + n - 1) % n;
        else if (vk == (horizontal ? VK_RIGHT : VK_DOWN) || vk == VK_TAB) menu_sel_ = (menu_sel_ + 1) % n;
        else if (vk == VK_RETURN || vk == VK_SPACE) activate = items[static_cast<size_t>(menu_sel_)];
        else if (vk == VK_ESCAPE) activate = menu_confirm_ ? kActEndCancel : kActResume;
        else if (vk == 'M' && ctrl && alt) activate = kActResume;
        else if (vk == 'Q' && ctrl && alt) activate = menu_confirm_ ? kActEndConfirm : kActEndAsk;
        else if (vk == VK_F1) activate = kActStats;
        else if (vk == VK_F11) activate = kActFullscreen;
    }
    if (activate != kActNone) do_action(activate);
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
    if (screen_ == Screen::Stream && menu_open_) {
        if (down) menu_key(vk);  // nothing reaches the console while the menu is open
        return;
    }
    if (down) {
        if (screen_ == Screen::Stream && ctrl && alt && (vk == 'M' || vk == 'Q')) {
            open_menu(vk == 'Q');
            return;
        }
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
            post_action(kActStop);  // lobby: leave a starting session
            return;
        }
    }
    if (vk == VK_ESCAPE && mouse_captured_) {
        if (down) set_mouse_captured(false);
        return;
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
    case WM_MOVE:
        if (msg == WM_SIZE) resize_pending_ = true;
        if (mouse_captured_) clip_cursor();
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
        if (mouse_captured_) return 0;  // aim comes from Raw Input; keep the overlay hidden
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
    case WM_LBUTTONDBLCLK:
        if (mouse_captured_) return 0;  // buttons come from Raw Input
        if (screen_ == Screen::Stream) {
            std::lock_guard<std::mutex> lk(mu_);
            if (ui_.hit(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))) < 0) {
                PostMessageW(hwnd, WM_HIKARI_ACTION, kActFullscreen, 0);
                return 0;
            }
        }
        [[fallthrough]];
    case WM_LBUTTONDOWN: {
        if (mouse_captured_) return 0;  // buttons come from Raw Input
        int hit;
        {
            std::lock_guard<std::mutex> lk(mu_);
            hit = ui_.hit(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
        }
        // A click on the picture (not on an overlay control) locks the pointer for aiming.
        if (hit < 0 && screen_ == Screen::Stream && focused_ && mouse_aim_enabled()) {
            set_mouse_captured(true);
            return 0;
        }
        std::lock_guard<std::mutex> lk(mu_);
        ui_.mouse_down = true;
        ui_.pressed_id = hit;
        if (ui_.pressed_id >= 0) ui_.focus_id = ui_.pressed_id;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (mouse_captured_) return 0;
        std::lock_guard<std::mutex> lk(mu_);
        const int id = ui_.hit(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
        if (id >= 0 && id == ui_.pressed_id) ui_.clicked_id = id;
        ui_.mouse_down = false;
        ui_.pressed_id = -1;
        ReleaseCapture();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && screen_ == Screen::Stream && !menu_open_ &&
            (mouse_captured_ || now_us() - last_mouse_move_us_ > 2'000'000)) {
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
    case WM_HIKARI_MENU_NAV: {
        // Controller: D-pad moves, A selects, B backs out.
        const auto b = static_cast<SwitchInput>(wp);
        const UINT vk = b == SwitchInput::DpadUp ? VK_UP : b == SwitchInput::DpadDown ? VK_DOWN
            : b == SwitchInput::DpadLeft ? VK_LEFT : b == SwitchInput::DpadRight ? VK_RIGHT
            : b == SwitchInput::A ? VK_RETURN : b == SwitchInput::B ? VK_ESCAPE : 0;
        if (vk && menu_open_) menu_key(vk);
        return 0;
    }
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
        const bool overlay_live = !hooks_.preview.empty() || show_stats_ || now_s() < toast_until ||
                                  net_.state() != ConnState::Connected || menu_open_;

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
    p.logo({m,p.s(28),m+p.s(28),p.s(56)});
    p.text("Remote play", {m+p.s(42),p.s(22),right-p.s(384),p.s(52)},p.s(22),ui::rgb(th::kText),Weight::SemiBold);
    p.text("Your console and stream preferences.", {m+p.s(42),p.s(52),right-p.s(384),p.s(74)},p.s(13),ui::rgb(th::kText2));
    if(p.button(ui_,kActChangeRole,{right-p.s(372),p.s(28),right-p.s(248),p.s(64)},"Change role",ButtonStyle::Ghost,bool(hooks_.change_role))) post_action(kActChangeRole);
    if(p.button(ui_,kActController,{right-p.s(236),p.s(28),right-p.s(124),p.s(64)},"Controller")) post_action(kActController);
    if(p.button(ui_,kActConnection,{right-p.s(112),p.s(28),right,p.s(64)},"Connection")) post_action(kActSettings);

    const Rect status_bar{r.l,r.b-p.s(36),r.r,r.b};
    const float split = m+(r.w()-m*3)*0.62f, side=split+m;
    const float top=p.s(100), bottom=status_bar.t-p.s(20), session_bottom=top+p.s(220);
    p.card({m,top,split,session_bottom});
    const float x=m+p.s(22), inner=split-p.s(22);
    const bool awake=have && st.console==ConsoleState::Ready;
    const std::string host=have && st.host_name[0] ? st.host_name : host_label_;
    // The console sits slightly in from the card edge.
    const float art_width=std::clamp((inner-x)*0.44f,p.s(174),p.s(300)), art_right=inner-p.s(24);
    const float text_right=art_right-art_width-p.s(20);
    const bool switch2=switch2_;
    const std::string console_name=switch2?"Switch 2":"Switch";
    p.text(host.empty()?"Your "+console_name:host+"'s "+console_name,{x,top+p.s(18),text_right,top+p.s(48)},p.s(18),ui::rgb(th::kText),Weight::SemiBold);
    std::string state="Waiting for the host";
    const bool resume=ended_by_user_ && have && st.console==ConsoleState::Ready && st.phase==SessionPhase::Lobby;
    if(have) switch(st.console) {
    case ConsoleState::Ready: state=resume?"Stream ended. Your console is still on.":"Ready to play"; break;
    case ConsoleState::Asleep: state="Asleep. Play will wake your console."; break;
    case ConsoleState::Off: state="Off or undocked"; break;
    case ConsoleState::Waking: state="Waking your console"; break;
    case ConsoleState::AwakeNoVideo: state="Waiting for an HDMI signal"; break;
    default: state="Checking your console"; break;
    }
    p.text(state,{x,top+p.s(52),text_right,top+p.s(92)},p.s(13),ui::rgb(th::kText2),Weight::Regular,Align::Leading,Align::Leading,true);
    p.console({art_right-art_width,top+p.s(18),art_right,session_bottom-p.s(18)},awake?th::kGood:th::kWarn,
              switch2?ui::ConsoleModel::Switch2:ui::ConsoleModel::Switch);
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
            if(i<3) p.line(cx+p.s(12),y+p.s(8),cx+step-p.s(12),y+p.s(8),ui::rgb(i<current?th::kAccent:th::kBorderStrong),p.s(1));
            if(i==current) p.spinner(cx,y+p.s(8),p.s(7),ui::rgb(th::kAccent));
            else p.circle(cx,y+p.s(8),p.s(4),ui::rgb(i<current?th::kAccent:th::kBorderStrong));
            p.text(steps[i],{cx-p.s(8),y+p.s(21),cx+step-p.s(10),y+p.s(40)},p.s(11),ui::rgb(i<=current?th::kText:th::kText3));
        }
        const float message_y=y+p.s(compact_steps?28:42);
        const char* message=st.message[0]?st.message:st.phase==SessionPhase::Streaming?"Waiting for the first picture":proto::to_string(st.phase);
        p.text(message,{x,message_y,text_right,message_y+p.s(30)},p.s(12),ui::rgb(th::kText2),Weight::Regular,Align::Leading,Align::Leading,true);
        const float cancel_y=y+p.s(compact_steps?64:76);
        if(p.button(ui_,kActStop,{x,cancel_y,x+p.s(88),cancel_y+p.s(34)},"Cancel")) post_action(kActStop);
    } else {
        const bool failed=have && st.phase==SessionPhase::Failed;
        const bool setup=conn==ConnState::Rejected || conn==ConnState::Idle;
        const bool replaced=conn==ConnState::Replaced;
        const bool unreachable=conn==ConnState::Unreachable;
        const int action=replaced?kActTakeOver:setup?kActSettings:failed?kActRetry:kActPlay;
        if (ui_.focus_id < 0) ui_.focus_id=action;
        if(failed) {
            p.text(st.message[0]?st.message:"Could not start the session",{x,y-p.s(12),text_right,y+p.s(34)},p.s(13),ui::rgb(th::kBad),Weight::Regular,Align::Leading,Align::Center,true);
            y+=p.s(40);
        }
        const char* label=replaced?"Take over":setup?"Set up connection":failed?"Try again":resume?"Resume":"Play";
        if(p.button(ui_,action,{x,y,x+p.s(164),y+p.s(38)},label,ButtonStyle::Primary,setup||replaced||have)) post_action(action);
        if(have && !setup && !failed && !replaced && !unreachable) {
            // "Or press [A] on your controller." with a drawn button cap.
            const Rect hint{x,y+p.s(46),text_right,y+p.s(64)};
            const float lead=p.text_width("Or press",p.s(12));
            p.text("Or press",hint,p.s(12),ui::rgb(th::kText3));
            const float kw=p.keycap(hint.l+lead+p.s(5),hint.cy(),"A");
            p.text("on your controller.",{hint.l+lead+kw+p.s(10),hint.t,hint.r,hint.b},p.s(12),ui::rgb(th::kText3));
        } else {
            const char* hint=replaced?"Another device is connected to your host. Take over to play here; the other device will be disconnected."
                :conn==ConnState::Rejected?"The host did not accept the pairing code. Paste a fresh invite from the host."
                :setup?"Paste the invite from your host PC."
                :unreachable?"No answer from the host. Check that Hikari is running there, both PCs are signed in to Tailscale, and the host allowed Hikari through Windows Firewall."
                :failed?"Check the host, then retry.":"Keep the host app running on your other PC.";
            p.text(hint,{x,y+p.s(46),text_right,y+p.s(unreachable||replaced?112:80)},p.s(12),ui::rgb(unreachable?th::kWarn:th::kText3),Weight::Regular,Align::Leading,Align::Leading,true);
        }
        if(have && !failed && st.issue!=proto::HostIssue::None)
            p.text(proto::to_string(st.issue),{x,y+p.s(82),text_right,y+p.s(106)},p.s(12),ui::rgb(th::kWarn));
        else if(have && path_state_==2)
            p.text("Relayed by Tailscale - latency will be higher than usual.",{x,y+p.s(82),text_right,y+p.s(106)},p.s(12),ui::rgb(th::kWarn));
    }

    const float prefs=session_bottom+p.s(16), row_start=prefs+p.s(60);
    p.card({m,prefs,split,bottom});
    p.text("Stream preference",{x,prefs+p.s(16),inner,prefs+p.s(42)},p.s(15),ui::rgb(th::kText),Weight::SemiBold);
    const char* names[]={"Minimum latency","Balanced","Quality"};
    const char* descriptions[]={"Prioritize response time.","Balance responsiveness and picture quality.","Prioritize picture quality."};
    const float pitch=std::min(p.s(64),(bottom-row_start-p.s(16))/3);
    for(int i=0;i<3;++i) {
        const Rect row{x-p.s(6),row_start+pitch*i,inner+p.s(6),row_start+pitch*(i+1)-p.s(4)};
        const bool selected=static_cast<int>(preset_.load())==i;
        if(p.button(ui_,kActPresetLatency+i,row,"",selected?ButtonStyle::Secondary:ButtonStyle::Ghost)) post_action(kActPresetLatency+i);
        const float cx=x+p.s(12),cy=row.cy();
        p.circle(cx,cy,p.s(7),ui::rgb(selected?th::kAccent:0xA0A4AC),false,p.s(1));
        if(selected) p.circle(cx,cy,p.s(3),ui::rgb(th::kAccent));
        if(pitch<p.s(48)) {
            p.text(names[i],{x+p.s(32),row.t,inner,row.b},p.s(13),ui::rgb(th::kText),Weight::Medium);
        } else {
            p.text(names[i],{x+p.s(32),row.t+p.s(4),inner,row.cy()+p.s(2)},p.s(13),ui::rgb(th::kText),Weight::Medium);
            p.text(descriptions[i],{x+p.s(32),row.cy(),inner,row.b-p.s(3)},p.s(12),ui::rgb(th::kText2));
        }
    }

    const Rect hostc{side,top,right,bottom};
    p.card(hostc);
    const float sx=hostc.l+p.s(20), sr=hostc.r-p.s(20);
    p.text("Host connection",{sx,top+p.s(18),sr-p.s(110),top+p.s(44)},p.s(15),ui::rgb(th::kText),Weight::SemiBold);
    p.pill(sr,top+p.s(19),connected?"Connected":to_string(conn),connected?th::kGood:th::kWarn,true);
    p.text("Host",{sx,top+p.s(58),sr,top+p.s(76)},p.s(12),ui::rgb(th::kText3));
    if(connected && path_state_!=0) {
        const bool direct=path_state_==1;
        p.text(direct?"Tailscale direct":"Tailscale relayed"+(path_relay_.empty()?std::string():" via "+path_relay_),
               {sx+p.s(40),top+p.s(58),sr,top+p.s(76)},p.s(12),ui::rgb(direct?th::kText2:th::kWarn),Weight::Regular,Align::Trailing);
    }
    p.text(host.empty()?"No host configured":host,{sx,top+p.s(76),sr,top+p.s(100)},p.s(14),ui::rgb(th::kText),Weight::Medium);
    p.divider(sx,sr,top+p.s(116));
    const char* labels[]={"Controller bridge","Capture card","HDMI signal","Hardware encoder"};
    const bool checks[]={bool(st.bridge_ok),bool(st.capture_ok),bool(st.signal_ok),bool(st.encoder_ok)};
    const bool can_sleep=have && (st.wake_methods&4) && awake;
    const float check_pitch=p.s(48);
    for(int i=0;i<4;++i) {
        const float ry=top+p.s(122)+check_pitch*i, cy=ry+check_pitch/2;
        p.status_icon(sx+p.s(10),cy,checks[i],!have);
        p.text(labels[i],{sx+p.s(32),ry,sr-p.s(80),ry+check_pitch},p.s(13),ui::rgb(th::kText));
        p.text(!have?"Waiting":checks[i]?"Ready":"Not ready",{sr-p.s(80),ry,sr,ry+check_pitch},p.s(12),ui::rgb(!have?th::kText3:checks[i]?th::kGood:th::kWarn),Weight::Medium,Align::Trailing);
        if(i<3) p.divider(sx+p.s(32),sr,ry+check_pitch);
    }
    if(can_sleep)
        if(p.button(ui_,kActSleep,{sx,bottom-p.s(56),sr,bottom-p.s(20)},"Put console to sleep")) post_action(kActSleep);

    p.footer_bar(status_bar);
    const bool preview=!hooks_.preview.empty();
    p.circle(m+p.s(4),status_bar.cy(),p.s(3),ui::rgb(preview?th::kAccent:connected?th::kGood:th::kText3));
    p.text(preview?"Offline preview \xC2\xB7 Sample device status":"Hikari",{m+p.s(16),status_bar.t,split,status_bar.b},p.s(12),ui::rgb(th::kText2));
    // Real lobby shortcuts, right-aligned.
    {
        float kx=right-p.text_width("Full screen",p.s(12));
        p.text("Full screen",{kx,status_bar.t,right,status_bar.b},p.s(12),ui::rgb(th::kText3));
        kx-=p.s(8)+std::max(p.s(20),p.text_width("F11",p.s(11.5f),Weight::SemiBold)+p.s(12));
        p.keycap(kx,status_bar.cy()-p.s(1),"F11");
    }
    // State changes can replace Play with Retry, Settings or Cancel.
    if (std::none_of(ui_.hits.begin(), ui_.hits.end(),
            [this](const auto& hit) { return hit.first == ui_.focus_id; })) {
        const int preferred = progress ? kActStop : conn == ConnState::Replaced ? kActTakeOver
            : conn == ConnState::Rejected || conn == ConnState::Idle
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
        // Centred in the space right of the stats panel when it is open.
        const float l = show_stats_ ? p.s(32) + std::min(r.w() - p.s(32), p.s(660)) : r.l;
        const float cy = r.cy() - p.s(24);
        p.text("Stream preview", {l, cy - p.s(38), r.r, cy}, p.s(30), ui::rgb(th::kText), Weight::SemiBold, Align::Center);
        p.text("No live video - connect a host to see your game", {l, cy + p.s(8), r.r, cy + p.s(38)}, p.s(15), ui::rgb(th::kText2), Weight::Regular, Align::Center);
    }
    std::string toast;
    double until;
    {
        std::lock_guard<std::mutex> lk(mu_);
        toast = toast_;
        until = toast_until_;
    }
    if (show_stats_) draw_stats(p, r);

    if (hooks_.preview.empty() && net_.state() != ConnState::Connected && net_.state() != ConnState::Replaced) {
        p.fill(r, ui::rgb(0x000000, 0.55f));
        p.spinner(r.cx(), r.cy() - p.s(20), p.s(22), ui::rgb(th::kAccent));
        p.text("Reconnecting...", Rect{r.l, r.cy() + p.s(16), r.r, r.cy() + p.s(46)}, p.s(18), ui::rgb(th::kText),
               Weight::SemiBold, Align::Center);
    }
    if (menu_open_) draw_stream_menu(p, r);
    if (now_s() < until && !toast.empty()) {
        const float alpha = static_cast<float>(std::min(1.0, (until - now_s()) * 2));
        const float w = p.text_width(toast, p.s(13), Weight::Medium) + p.s(36);
        const Rect t{r.cx() - w / 2, p.s(16), r.cx() + w / 2, p.s(50)};
        p.fill(t, ui::rgb(0x16171A, 0.92f * alpha), t.h() / 2);
        p.stroke(t, ui::rgb(0xFFFFFF, 0.08f * alpha), p.s(1), t.h() / 2);
        p.text(toast, t, p.s(13), ui::rgb(th::kText, alpha), Weight::Medium, Align::Center);
    }
}

void ClientApp::draw_stream_menu(ui::Painter& p, const Rect& r)
{
    std::lock_guard<std::mutex> lk(mu_);
    ui_.begin_frame();
    p.fill(r, ui::rgb(0x000000, 0.5f));
    const std::vector<int> items = menu_items();
    menu_sel_ = std::clamp(menu_sel_, 0, static_cast<int>(items.size()) - 1);
    const float w = p.s(420), row_h = p.s(44);
    const float h = menu_confirm_ ? p.s(176) : p.s(96) + row_h * items.size() + p.s(58);
    const Rect panel{r.cx() - w / 2, r.cy() - h / 2, r.cx() + w / 2, r.cy() + h / 2};
    p.shadow(panel, p.s(14), 0.6f);
    p.fill(panel, ui::rgb(th::kBg2), p.s(14));
    p.stroke(panel, ui::rgb(0xFFFFFF, 0.08f), p.s(1), p.s(14));
    const float in = panel.l + p.s(24), out = panel.r - p.s(24);

    // Keycaps right-aligned at x, returns the left edge.
    const auto keys = [&](float x, float cy, std::initializer_list<const char*> caps) {
        for (auto it = std::rbegin(caps); it != std::rend(caps); ++it) {
            const float kw = std::max(p.s(20), p.text_width(*it, p.s(11.5f), Weight::SemiBold) + p.s(12));
            x -= kw;
            p.keycap(x, cy, *it);
            x -= p.s(4);
        }
        return x;
    };

    if (menu_confirm_) {
        p.text("End the stream?", {in, panel.t + p.s(22), out, panel.t + p.s(48)}, p.s(17), ui::rgb(th::kText), Weight::SemiBold);
        p.text("Your console stays on and connected. Press Resume in the lobby, or A on your controller, to pick up where you left off.",
               {in, panel.t + p.s(54), out, panel.t + p.s(112)}, p.s(13), ui::rgb(th::kText2), Weight::Regular, Align::Leading, Align::Leading, true);
        const float by = panel.b - p.s(60);
        const Rect keep{out - p.s(268), by, out - p.s(136), by + p.s(38)}, end{out - p.s(124), by, out, by + p.s(38)};
        ui_.focus_id = items[static_cast<size_t>(menu_sel_)];
        if (p.button(ui_, kActEndCancel, keep, "Keep playing")) post_action(kActEndCancel);
        if (p.button(ui_, kActEndConfirm, end, "End stream", ButtonStyle::Danger)) post_action(kActEndConfirm);
    } else {
        p.text("Stream paused", {in, panel.t + p.s(22), out, panel.t + p.s(48)}, p.s(17), ui::rgb(th::kText), Weight::SemiBold);
        p.text("Nothing you press reaches the console while this menu is open.", {in, panel.t + p.s(50), out, panel.t + p.s(72)},
               p.s(12.5f), ui::rgb(th::kText2));
        ui_.focus_id = -1;
        float y = panel.t + p.s(88);
        for (size_t i = 0; i < items.size(); ++i) {
            const int id = items[i];
            const Rect row{panel.l + p.s(10), y, panel.r - p.s(10), y + row_h - p.s(4)};
            const bool selected = static_cast<int>(i) == menu_sel_;
            if (p.button(ui_, id, row, "", selected ? ButtonStyle::Secondary : ButtonStyle::Ghost)) post_action(id);
            const char* label = id == kActResume ? "Resume" : id == kActStats ? (show_stats_ ? "Hide stats" : "Show stats")
                : id == kActController ? "Controller settings" : id == kActFullscreen ? (fullscreen_ ? "Exit full screen" : "Full screen")
                : "End stream";
            p.text(label, {in, row.t, out - p.s(120), row.b}, p.s(14), ui::rgb(id == kActEndAsk ? 0xFF8A80 : th::kText), Weight::Medium);
            const float cy = row.cy() - p.s(1);
            if (id == kActResume) keys(out, cy, {"Esc"});
            else if (id == kActStats) keys(out, cy, {"F1"});
            else if (id == kActFullscreen) keys(out, cy, {"F11"});
            else if (id == kActEndAsk) keys(out, cy, {"Ctrl", "Alt", "Q"});
            y += row_h;
        }
        const float fy = panel.b - p.s(46);
        p.divider(panel.l + p.s(1), panel.r - p.s(1), fy);
        p.text("Arrows and Enter, or D-pad and A. Esc or B resumes.", {in, fy, out, panel.b}, p.s(12), ui::rgb(th::kText3));
    }
    ui_.take_click();
}

namespace {

// Everything the stats panel shows, filled from live counters or offline sample values.
struct StatsView {
    std::string resolution, codec, encoder, present_mode, device, bridge, route;
    double mbps = 0, rtt = 0, jitter = 0, loss = 0, queue = 0;
    bool e2e_valid = false;
    double e2e_p50 = 0, e2e_p95 = 0, cap_send_p50 = 0, cap_send_p95 = 0, enc_p50 = 0, enc_p95 = 0, dec_p50 = 0, dec_p95 = 0;
    double input_p50 = 0, input_p95 = 0, host_bridge = 0, bridge_switch_p50 = 0, bridge_switch_p99 = 0;
    double send_hz = 0, capture_fps = 0, encode_fps = 0, decode_fps = 0, present_fps = 0;
    unsigned reports = 0, capture_drops = 0, sender_drops = 0, audio_ms = 0;
    unsigned long long network_drops = 0, fec_fixed = 0, decoder_drops = 0, underruns = 0;
};

}  // namespace

void ClientApp::draw_stats(ui::Painter& p, const Rect& r)
{
    const uint64_t now = now_us();
    const bool sample = !hooks_.preview.empty();
    StatsView v;
    if (sample) {
        v = {"1920 x 1080 at 60", "H.264", "Hardware H.264 encoder", "Balanced", "Xbox Wireless Controller", "Connected"};
        v.mbps = 18.4; v.rtt = 9.8; v.jitter = 0.6; v.loss = 0.02; v.queue = 1.2;
        v.e2e_valid = true; v.e2e_p50 = 34.2; v.e2e_p95 = 41.8;
        v.cap_send_p50 = 6.1; v.cap_send_p95 = 7.9; v.enc_p50 = 3.2; v.enc_p95 = 4.0; v.dec_p50 = 1.9; v.dec_p95 = 2.6;
        v.input_p50 = 4.9; v.input_p95 = 7.2; v.host_bridge = 0.21; v.bridge_switch_p50 = 1.02; v.bridge_switch_p99 = 1.98;
        v.send_hz = 250; v.reports = 250; v.capture_fps = 60.0; v.encode_fps = 60.0; v.decode_fps = 59.9; v.present_fps = 60.0;
        v.network_drops = 2; v.fec_fixed = 2; v.audio_ms = 40;
        v.route = "Tailscale direct";
    } else {
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
        const auto ms = [](uint32_t us) { return us / 1000.0; };
        v.resolution = fmt("%u x %u at %u", hs.width, hs.height, hs.fps);
        v.codec = sc.codec == proto::VideoCodec::HEVC ? "HEVC" : "H.264";
        v.encoder = hs.encoder_name[0] ? hs.encoder_name : "-";
        v.present_mode = to_string(renderer_.mode());
        v.device = input_.active_device().empty() ? "None" : input_.active_device();
        v.bridge = hs.esp_connected ? (hs.esp_failsafe ? "Failsafe" : "Connected") : "Offline";
        v.mbps = ns.mbps; v.rtt = ns.rtt_ms; v.jitter = ns.jitter_ms; v.loss = ns.loss_pct; v.queue = ns.queue_delay_ms;
        v.e2e_valid = e2e.count > 0; v.e2e_p50 = ms(e2e.p50); v.e2e_p95 = ms(e2e.p95);
        v.cap_send_p50 = ms(hs.capture_to_send_p50); v.cap_send_p95 = ms(hs.capture_to_send_p95);
        v.enc_p50 = ms(hs.encode_p50); v.enc_p95 = ms(hs.encode_p95); v.dec_p50 = ms(dec.p50); v.dec_p95 = ms(dec.p95);
        v.input_p50 = hs.input_owd_p50 / 1000.0; v.input_p95 = hs.input_owd_p95 / 1000.0; v.host_bridge = hs.input_host_p50 / 1000.0;
        v.bridge_switch_p50 = hs.esp_rx_to_console_p50 / 1000.0; v.bridge_switch_p99 = hs.esp_rx_to_console_p99 / 1000.0;
        v.send_hz = input_.send_rate(); v.reports = hs.esp_reports_per_s;
        v.capture_fps = hs.capture_fps_x100 / 100.0; v.encode_fps = hs.encode_fps_x100 / 100.0;
        v.decode_fps = decode_fps_; v.present_fps = present_fps_;
        v.capture_drops = hs.capture_drops; v.sender_drops = hs.encode_drops;
        v.network_drops = ns.frames_dropped; v.fec_fixed = ns.frames_fec_recovered; v.decoder_drops = decoder_drops_;
        v.audio_ms = audio_.buffered_ms(); v.underruns = audio_.underruns();
        if (path_state_ != 0) {
            std::lock_guard<std::mutex> lk(mu_);
            v.route = path_state_ == 1 ? std::string("Tailscale direct") : "Tailscale relay " + path_relay_ + " (adds latency)";
        }
    }

    const float w = std::min(r.w() - p.s(32), p.s(660));
    const Rect panel{p.s(16), p.s(16), p.s(16) + w, std::min(r.b - p.s(16), p.s(16) + p.s(560))};
    p.shadow(panel, p.s(12), 0.4f);
    p.fill(panel, ui::rgb(0x0E0F11, 0.9f), p.s(12));
    p.stroke(panel, ui::rgb(0xFFFFFF, 0.08f), p.s(1), p.s(12));
    const float in = panel.l + p.s(18), out = panel.r - p.s(18);

    // Header.
    p.text("Stream stats", {in, panel.t + p.s(12), out, panel.t + p.s(40)}, p.s(14), ui::rgb(th::kText), Weight::SemiBold);
    if (sample) {
        p.pill(out, panel.t + p.s(14), "Sample data", th::kAccent, true);
    } else {
        const float hide = p.text_width("Hide", p.s(12));
        p.text("Hide", {out - hide, panel.t + p.s(12), out, panel.t + p.s(40)}, p.s(12), ui::rgb(th::kText3));
        p.keycap(out - hide - p.s(30), panel.t + p.s(26), "F1");
    }

    // Headline figures.
    struct Figure { std::string value; const char* unit; const char* label; };
    const Figure figures[] = {
        {v.e2e_valid ? fmt("%.1f", v.e2e_p50) : "-", "ms", "Glass to glass*"},
        {fmt("%.1f", v.present_fps), "fps", "Frame rate"},
        {fmt("%.1f", v.mbps), "Mbps", "Bitrate"},
        {fmt("%.2f", v.loss), "%", "Packet loss"},
    };
    const float fy = panel.t + p.s(50), fw = (out - in) / 4;
    for (int i = 0; i < 4; ++i) {
        const float fx = in + fw * i;
        if (i) p.line(fx - p.s(12), fy + p.s(4), fx - p.s(12), fy + p.s(44), ui::rgb(0xFFFFFF, 0.07f), p.s(1));
        const float vw = p.text_width(figures[i].value, p.s(22), Weight::SemiBold);
        p.text(figures[i].value, {fx, fy, fx + fw, fy + p.s(28)}, p.s(22), ui::rgb(th::kText), Weight::SemiBold);
        p.text(figures[i].unit, {fx + vw + p.s(4), fy + p.s(8), fx + fw, fy + p.s(28)}, p.s(12), ui::rgb(th::kText3));
        p.text(figures[i].label, {fx, fy + p.s(30), fx + fw - p.s(14), fy + p.s(48)}, p.s(11.5f), ui::rgb(th::kText2));
    }

    // Glass-to-glass trace, 0-100 ms.
    const Rect sp{in, fy + p.s(62), out, fy + p.s(118)};
    p.fill(sp, ui::rgb(0xFFFFFF, 0.03f), p.s(6));
    for (int g = 1; g < 4; ++g)
        p.line(sp.l + p.s(6), sp.b - sp.h() * g / 4, sp.r - p.s(6), sp.b - sp.h() * g / 4, ui::rgb(0xFFFFFF, 0.04f), p.s(1));
    for (int i = 1; i < 120; ++i) {
        const float a = e2e_spark_[(spark_pos_ + i - 1) % 120], b = e2e_spark_[(spark_pos_ + i) % 120];
        if (a <= 0 || b <= 0) continue;
        const float x0 = sp.l + sp.w() * (i - 1) / 119.0f, x1 = sp.l + sp.w() * i / 119.0f;
        p.line(x0, sp.b - sp.h() * std::min(a, 100.0f) / 100.0f, x1, sp.b - sp.h() * std::min(b, 100.0f) / 100.0f,
               ui::rgb(th::kAccent), p.s(1.5f));
    }
    p.text("100 ms", {sp.r - p.s(60), sp.t + p.s(2), sp.r - p.s(8), sp.t + p.s(16)}, p.s(10.5f), ui::rgb(th::kText3), Weight::Regular, Align::Trailing);

    // Detail sections in two columns.
    using Rows = std::vector<std::pair<std::string, std::string>>;
    const std::string arrow = " \xE2\x86\x92 ";
    const auto pair_ms = [](double a, double b) { return fmt("%.1f / %.1f ms", a, b); };
    Rows network = {{"Round trip", fmt("%.1f ms", v.rtt)}, {"Jitter", fmt("%.1f ms", v.jitter)}, {"Queue delay", fmt("%.1f ms", v.queue)}};
    if (!v.route.empty()) network.insert(network.begin(), {"Route", v.route});
    const std::pair<const char*, Rows> left[] = {
        {"Video", {{"Resolution", v.resolution}, {"Codec", v.codec}, {"Encoder", v.encoder}, {"Display mode", v.present_mode}}},
        {"Network", network},
        {"Latency  p50 / p95", {{"Glass to glass*", v.e2e_valid ? pair_ms(v.e2e_p50, v.e2e_p95) : "Syncing clocks"},
                                {"Capture" + arrow + "send", pair_ms(v.cap_send_p50, v.cap_send_p95)},
                                {"Encode", pair_ms(v.enc_p50, v.enc_p95)}, {"Decode", pair_ms(v.dec_p50, v.dec_p95)}}},
    };
    const std::pair<const char*, Rows> right[] = {
        {"Input", {{"PC" + arrow + "host", pair_ms(v.input_p50, v.input_p95)}, {"Host" + arrow + "bridge", fmt("%.2f ms", v.host_bridge)},
                   {"Bridge" + arrow + "Switch", fmt("%.2f / %.2f ms", v.bridge_switch_p50, v.bridge_switch_p99)}}},
        {"Controller", {{"Device", v.device}, {"Sending", fmt("%.0f Hz", v.send_hz)}, {"Bridge", fmt("%s, %u reports/s", v.bridge.c_str(), v.reports)}}},
        {"Frames", {{"Capture / encode", fmt("%.1f / %.1f fps", v.capture_fps, v.encode_fps)}, {"Decode / present", fmt("%.1f / %.1f fps", v.decode_fps, v.present_fps)},
                    {"Dropped", fmt("%u capture, %u sender", v.capture_drops, v.sender_drops)},
                    {"Network", fmt("%llu lost, %llu fixed by FEC", v.network_drops, v.fec_fixed)}}},
        {"Audio", {{"Buffer", fmt("%u ms, %llu underruns", v.audio_ms, v.underruns)}}},
    };
    const float col_gap = p.s(28), cw = (out - in - col_gap) / 2, top = sp.b + p.s(16), limit = panel.b - p.s(30);
    const auto column = [&](float cx, const auto& sections) {
        float y = top;
        for (const auto& [title, rows] : sections) {
            if (y + p.s(22) > limit) return;
            p.text(title, {cx, y, cx + cw, y + p.s(20)}, p.s(11.5f), ui::rgb(th::kText2), Weight::SemiBold);
            y += p.s(22);
            for (const auto& [k, val] : rows) {
                if (y + p.s(19) > limit) return;
                p.text(k, {cx, y, cx + cw * 0.42f, y + p.s(19)}, p.s(12), ui::rgb(th::kText3));
                p.text(val, {cx + cw * 0.42f, y, cx + cw, y + p.s(19)}, p.s(12), ui::rgb(th::kText), Weight::Regular, Align::Trailing);
                y += p.s(19);
            }
            y += p.s(10);
        }
    };
    column(in, left);
    column(in + cw + col_gap, right);
    p.text("* Capture card to this screen, excluding display scan-out.", {in, panel.b - p.s(26), out, panel.b - p.s(8)}, p.s(10.5f), ui::rgb(th::kText3));
}

}  // namespace hikari

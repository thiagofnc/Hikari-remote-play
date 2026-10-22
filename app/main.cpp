#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "client_app.h"
#include "controller_settings.h"
#include "d2d_window.h"
#include "host_app.h"
#include "hikari/crypto.h"
#include "hikari/log.h"
#include "hikari/media.h"
#include "hikari/system.h"

namespace hikari {
namespace {
using ui::Rect;
namespace th = ui::theme;

class RoleWindow final : public D2DWindow {
public:
    std::string role;
private:
    void draw(ui::Painter& p, const Rect& r) override
    {
        p.background(r);
        p.logo({r.cx() - p.s(42), p.s(24), r.cx() + p.s(42), p.s(108)});
        p.text("Your Switch, anywhere.", {0, p.s(120), r.r, p.s(164)}, p.s(26), ui::rgb(th::kText),
               ui::Weight::SemiBold, ui::Align::Center);
        p.text("What will this PC do?", {0, p.s(174), r.r, p.s(205)}, p.s(16), ui::rgb(th::kText2),
               ui::Weight::Regular, ui::Align::Center);
        p.button(ui_, 1, {p.s(32), p.s(232), r.r - p.s(32), p.s(284)}, "Host - the PC beside your Switch", ui::ButtonStyle::Primary);
        p.button(ui_, 2, {p.s(32), p.s(300), r.r - p.s(32), p.s(352)}, "Client - the PC where you play");
        p.text("You can change roles by launching Hikari with --setup.",
               {p.s(24), p.s(374), r.r - p.s(24), p.s(410)}, p.s(12), ui::rgb(th::kText2),
               ui::Weight::Regular, ui::Align::Center, ui::Align::Center, true);
    }
    void on_action(int id) override { role = id == 1 ? "host" : "client"; close(); }
};

class ClientSettings final : public D2DWindow {
public:
    explicit ClientSettings(Config& cfg) : cfg_(cfg) {}
    bool open(HWND owner, bool discover = true)
    {
        if (!create(L"Hikari - Connect to your host", 620, 560, owner)) return false;
        host_ = add_edit(cfg_.get("client", "host"));
        code_ = add_edit(cfg_.get("client", "pairing_code"), true, true);
        offline_ = !discover;
        if (discover) {
            ClientNetEvents events;
            discovery_.start(std::move(events));
            discovery_.set_discovery(true);
        }
        return true;
    }
    bool saved = false;
private:
    void draw(ui::Painter& p, const Rect& r) override
    {
        p.background(r);
        const float x = p.s(30), right = r.r - x;
        p.text("Connect to your Switch", {x, p.s(22), right, p.s(66)}, p.s(26), ui::rgb(th::kText), ui::Weight::SemiBold);
        p.text("Use the address and pairing code shown on your host PC.",
               {x, p.s(72), right, p.s(108)}, p.s(14), ui::rgb(th::kText2));
        p.text("Host address (LAN IP or Tailscale name)", {x, p.s(122), right, p.s(148)}, p.s(14), ui::rgb(th::kText));
        const Rect host_rect{x, p.s(152), right, p.s(196)};
        p.fill(host_rect, ui::rgb(th::kSurface2), p.s(6));
        place(host_, host_rect);
        p.text("Pairing code", {x, p.s(210), right, p.s(236)}, p.s(14), ui::rgb(th::kText));
        const Rect code_rect{x, p.s(240), right, p.s(284)};
        p.fill(code_rect, ui::rgb(th::kSurface2), p.s(6));
        place(code_, code_rect);
        p.text("Hosts on your network", {x, p.s(304), right, p.s(330)}, p.s(14), ui::rgb(th::kText2));
        hosts_ = discovery_.discovered();
        if (hosts_.empty())
            p.text(offline_ ? "Offline preview - discovery is disabled." : "Searching... You can also enter an address above.", {x, p.s(338), right, p.s(374)}, p.s(13), ui::rgb(th::kText2));
        for (size_t i = 0; i < std::min<size_t>(hosts_.size(), 2); ++i) {
            const float y = p.s(338 + 42 * float(i));
            p.button(ui_, 100 + int(i), {x, y, right, y + p.s(36)}, hosts_[i].name + "  " + hosts_[i].address);
        }
        p.text(error_, {x, p.s(428), right, p.s(462)}, p.s(13), ui::rgb(th::kBad));
        p.button(ui_, 1, {right - p.s(160), p.s(484), right, p.s(528)}, "Save and connect", ui::ButtonStyle::Primary);
        p.button(ui_, 2, {x, p.s(484), x + p.s(100), p.s(528)}, "Cancel");
    }
    void on_action(int id) override
    {
        if (id == 2) { close(); return; }
        if (id >= 100 && size_t(id - 100) < hosts_.size()) {
            set_text(host_, hosts_[size_t(id - 100)].address);
            return;
        }
        if (id != 1) return;
        const std::string host = text_of(host_);
        const std::string code = crypto::normalize_pairing_code(text_of(code_));
        if (host.empty() || code.size() != 12) {
            error_ = "Enter a host address and a 12-character pairing code.";
            return;
        }
        // Save through a copy so a failed write leaves the active settings intact.
        Config next = cfg_;
        next.set("client", "host", host);
        next.set("client", "pairing_code", code);
        if (!next.save()) { error_ = "Could not save settings. Check access to the Hikari data folder."; return; }
        cfg_ = std::move(next);
        saved = true;
        close();
    }
    void on_key(UINT key) override { if (key == VK_ESCAPE) close(); }
    Config& cfg_;
    HWND host_ = nullptr, code_ = nullptr;
    ClientNet discovery_;
    std::vector<DiscoveredHost> hosts_;
    std::string error_;
    bool offline_ = false;
};

bool client_settings(HWND owner, Config& cfg)
{
    ClientSettings settings(cfg);
    if (!settings.open(owner)) return false;
    settings.run_modal();
    return settings.saved;
}

class HostWindow final : public D2DWindow {
public:
    explicit HostWindow(HostApp& host, bool preview = false) : host_(host), preview_(preview) {}
    ~HostWindow() override { if (tray_.hWnd) Shell_NotifyIconW(NIM_DELETE, &tray_); }
    void add_tray()
    {
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = hwnd();
        tray_.uID = 1;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray_.uCallbackMessage = WM_APP + 20;
        tray_.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        lstrcpynW(tray_.szTip, L"Hikari Host", ARRAYSIZE(tray_.szTip));
        have_tray_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
    }
private:
    void draw(ui::Painter& p, const Rect& r) override
    {
        p.background(r);
        HostSnapshot s;
        if (preview_) {
            s.host_name = "Living room - Offline preview";
            s.pairing_code = "DEMO-CODE-ONLY";
            s.addresses = {{"192.0.2.10", "Sample LAN", false}};
            s.bridge_ok = s.capture_ok = s.audio_ok = s.encoder_ok = true;
            s.bridge_port = "ESP32-S3 controller bridge";
            s.capture_name = "HDMI capture - 1920 x 1080 at 60 fps";
            s.audio_name = "Capture card HDMI audio";
            s.encoder_name = "Hardware H.264 encoder";
            s.message = "Sample device status. No hardware or network connection is active.";
        } else s = host_.snapshot();
        const float x = p.s(30), right = r.r - x;
        p.text("Hikari Host", {x, p.s(22), right, p.s(62)}, p.s(28), ui::rgb(th::kText), ui::Weight::SemiBold);
        p.text(s.host_name, {x, p.s(66), right, p.s(94)}, p.s(15), ui::rgb(th::kText2));
        p.text("Pairing code:  " + s.pairing_code, {x, p.s(112), right, p.s(152)}, p.s(23), ui::rgb(th::kAccent), ui::Weight::SemiBold);
        float y = p.s(164);
        for (const auto& address : s.addresses) {
            if (y > p.s(224)) break;
            p.text((address.tailscale ? "Tailscale: " : "LAN: ") + address.ip + ":" + std::to_string(s.port),
                   {x, y, right, y + p.s(24)}, p.s(14), ui::rgb(th::kText2));
            y += p.s(26);
        }
        struct Check { const char* label; bool ok; std::string detail; };
        const Check checks[] = {
            {"Controller bridge", s.bridge_ok, s.bridge_ok ? s.bridge_port : "Connect the ESP32-S3 UART port"},
            {"Capture card", s.capture_ok, s.capture_ok ? s.capture_name : "Searching for a capture card..."},
            {"HDMI audio", s.audio_ok, s.audio_ok ? s.audio_name : "Searching for capture audio..."},
            {"Hardware encoder", s.encoder_ok, s.encoder_benchmarking ? "Benchmarking..." : s.encoder_name},
        };
        y = p.s(246);
        for (const auto& check : checks) {
            p.circle(x + p.s(6), y + p.s(14), p.s(4), ui::rgb(check.ok ? th::kGood : th::kWarn));
            p.text(check.label, {x + p.s(20), y, right, y + p.s(27)}, p.s(15), ui::rgb(th::kText), ui::Weight::SemiBold);
            p.text(check.detail, {x + p.s(20), y + p.s(27), right, y + p.s(51)}, p.s(13), ui::rgb(th::kText2));
            y += p.s(62);
        }
        p.text(s.client_connected ? "Connected: " + s.client_name : "Waiting for a client", {x, p.s(504), right, p.s(532)}, p.s(15), ui::rgb(s.client_connected ? th::kGood : th::kText2));
        p.text(s.message, {x, p.s(538), right, p.s(578)}, p.s(13), ui::rgb(th::kText2), ui::Weight::Regular, ui::Align::Leading, ui::Align::Center, true);
        p.button(ui_, 1, {x, p.s(600), x + p.s(130), p.s(642)}, "Wake console", ui::ButtonStyle::Primary);
        p.button(ui_, 2, {x + p.s(142), p.s(600), x + p.s(264), p.s(642)}, "Find devices");
        p.button(ui_, 3, {x + p.s(276), p.s(600), right, p.s(642)}, "New pairing code");
        p.text("Closing this window keeps the host in the notification area. Right-click its icon to quit.",
               {x, p.s(654), right, p.s(700)}, p.s(12), ui::rgb(th::kText2), ui::Weight::Regular, ui::Align::Leading, ui::Align::Center, true);
    }
    void on_action(int id) override
    {
        if (preview_) return;
        if (id == 1) host_.wake_console();
        if (id == 2) host_.redetect_devices();
        if (id == 3 && MessageBoxW(hwnd(), L"Replace the pairing code? Clients will need the new code to reconnect.", L"Hikari", MB_YESNO | MB_ICONQUESTION) == IDYES)
            host_.regenerate_pairing_code();
    }
    bool on_close() override
    {
        if (!have_tray_) return true;
        ShowWindow(hwnd(), SW_HIDE);
        return false;
    }
    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override
    {
        if (msg == WM_APP + 20) {
            handled = true;
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd(), SW_RESTORE);
                SetForegroundWindow(hwnd());
            }
            if (lp == WM_RBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Open Hikari");
                AppendMenuW(menu, MF_STRING, 2, L"Quit");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd());
                const int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd(), nullptr);
                DestroyMenu(menu);
                if (id == 1) ShowWindow(hwnd(), SW_RESTORE);
                if (id == 2) close();
                else PostMessageW(hwnd(), WM_NULL, 0, 0);
            }
            return 0;
        }
        return D2DWindow::on_message(msg, wp, lp, handled);
    }
    HostApp& host_;
    NOTIFYICONDATAW tray_{};
    bool have_tray_ = false;
    bool preview_ = false;
};

int run_preview(const std::string& screen)
{
    Config cfg;
    const std::string dir = app_data_dir() + "\\preview";
    ensure_directory(dir);
    cfg.load(dir + "\\hikari.ini");
    cfg.set("client", "host", "192.0.2.10");
    cfg.set("client", "host_name", "Living room");
    cfg.set("client", "pairing_code", "DEMO-CODE-ONLY");
    cfg.set_bool("client", "fullscreen", false);
    if (screen == "setup") {
        RoleWindow window;
        if (!window.create(L"Hikari - Setup preview", 520, 440)) return 1;
        window.run_modal();
        return 0;
    }
    if (screen == "connection") {
        ClientSettings window(cfg);
        if (!window.open(nullptr, false)) return 1;
        window.run_modal();
        return 0;
    }
    if (screen == "host") {
        HostApp host;
        HostWindow window(host, true);
        if (!window.create(L"Hikari - Host preview", 620, 720)) return 1;
        window.run_modal();
        return 0;
    }
    if (screen == "controller") {
        InputMapping mapping = InputMapping::defaults_positional();
        ControllerSettingsDeps deps;
        deps.poll_pad = [] {
            RawPadState pad;
            pad.connected = true;
            pad.device_name = "Xbox Wireless Controller (preview)";
            return pad;
        };
        deps.list_devices = [] { return std::vector<std::string>{"Xbox Wireless Controller (preview)"}; };
        show_controller_settings(nullptr, cfg, mapping, deps);
        return 0;
    }
    if (screen != "ready" && screen != "asleep" && screen != "waking" && screen != "failed" && screen != "stream")
        return 2;
    ClientApp client;
    ClientHooks hooks;
    hooks.preview = screen;
    hooks.open_settings = [&cfg](HWND owner) {
        ClientSettings settings(cfg);
        if (!settings.open(owner, false)) return false;
        settings.run_modal();
        return settings.saved;
    };
    return client.run(cfg, std::move(hooks));
}

int run(const std::vector<std::string>& args)
{
    MediaRuntime media;
    if (!media.ok()) return 1;
    apply_process_performance_policy();
    for (const auto& arg : args)
        if (arg.rfind("--preview=", 0) == 0) return run_preview(arg.substr(10));
    Config cfg;
    cfg.load(app_data_dir() + "\\hikari.ini");
    std::string role = cfg.get("general", "role");
    bool setup = false, selftest = false;
    for (const auto& arg : args) {
        if (arg == "--host") role = "host";
        if (arg == "--client") role = "client";
        if (arg == "--setup") setup = true;
        if (arg == "--selftest") selftest = true;
    }
    if (selftest) {
        RoleWindow window;
        if (!window.create(L"Hikari self-test", 520, 440)) return 1;
        // Exercise the real rendering and embedded resources without saving settings.
        SendMessageW(window.hwnd(), WM_PAINT, 0, 0);
        window.close();
        return FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA) ? 0 : 1;
    }
    if (setup || (role != "host" && role != "client")) {
        RoleWindow chooser;
        if (!chooser.create(L"Welcome to Hikari", 520, 440)) return 1;
        chooser.run_modal();
        if (chooser.role.empty()) return 0;
        role = chooser.role;
    }
    cfg.set("general", "role", role);
    cfg.save();
    log::init(role.c_str());
    HK_LOGI("Hikari %s starting as %s", HIKARI_VERSION, role.c_str());
    if (role == "host") {
        HostApp host;
        if (!host.start(cfg)) {
            MessageBoxW(nullptr, L"The host could not start. Check the Hikari logs for details.", L"Hikari", MB_ICONERROR);
            return 1;
        }
        HostWindow window(host);
        if (!window.create(L"Hikari Host", 620, 720)) return 1;
        window.add_tray();
        window.run_modal();
        host.stop();
        cfg.save();
        return 0;
    }
    if (cfg.get("client", "host").empty() || cfg.get("client", "pairing_code").empty())
        if (!client_settings(nullptr, cfg)) return 0;
    ClientApp client;
    ClientHooks hooks;
    hooks.open_settings = [&cfg](HWND owner) { return client_settings(owner, cfg); };
    return client.run(cfg, std::move(hooks));
}
}  // namespace
}  // namespace hikari

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (argv) {
        for (int i = 1; i < argc; ++i) args.push_back(hikari::utf8(argv[i]));
        LocalFree(argv);
    }
    return hikari::run(args);
}

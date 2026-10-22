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
#include "hikari/tailscale.h"

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
        const float x=p.s(24), right=r.r-x;
        p.logo({x,p.s(24),x+p.s(24),p.s(48)});
        p.text("Hikari",{x+p.s(34),p.s(22),right,p.s(50)},p.s(18),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.text("How will you use this PC?",{x,p.s(80),right,p.s(112)},p.s(22),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.text("Host your Switch here, or connect to another PC to play.",{x,p.s(116),right,p.s(142)},p.s(13),ui::rgb(th::kText2));
        const float middle=(x+right)/2, y=p.s(164), bottom=p.s(378);
        const auto choice=[&](int id,float left,float edge,const char* name,const char* description,const char* requirement) {
            const Rect box{left,y,edge,bottom};
            p.button(ui_,id,box,"",ui::ButtonStyle::Ghost);
            p.fill(box,ui::rgb(ui_.hovered(id,box)?th::kSurface:th::kBg2),p.s(12));
            const float in=left+p.s(22),end=edge-p.s(22);
            p.text(name,{in,y+p.s(22),end,y+p.s(52)},p.s(18),ui::rgb(th::kText),ui::Weight::SemiBold);
            p.text(description,{in,y+p.s(62),end,y+p.s(116)},p.s(13),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
            p.line(in,y+p.s(134),end,y+p.s(134),ui::rgb(th::kBorder),p.s(1));
            p.text(requirement,{in,y+p.s(148),end-p.s(18),bottom-p.s(18)},p.s(12),ui::rgb(th::kText3),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
            const float ax=end-p.s(5),ay=bottom-p.s(36);
            p.line(ax-p.s(10),ay,ax,ay,ui::rgb(th::kText2),p.s(1.5f));
            p.line(ax-p.s(4),ay-p.s(4),ax,ay,ui::rgb(th::kText2),p.s(1.5f));
            p.line(ax-p.s(4),ay+p.s(4),ax,ay,ui::rgb(th::kText2),p.s(1.5f));
        };
        choice(1,x,middle-p.s(8),"Host","Connect your Switch and capture card to this PC.","Requires capture hardware");
        choice(2,middle+p.s(8),right,"Client","Play remotely from a host you have already set up.","Requires a paired host PC");
        p.text("Your choice is saved. Use Change role on either screen to switch.",{x,p.s(402),right,p.s(430)},p.s(12),ui::rgb(th::kText3));

    }
    void on_action(int id) override { role = id == 1 ? "host" : "client"; close(); }
    void on_key(UINT key) override { if (key == VK_ESCAPE) close(); }
};

class ClientSettings final : public D2DWindow {
public:
    explicit ClientSettings(Config& cfg) : cfg_(cfg) {}
    bool open(HWND owner, bool discover = true)
    {
        if (!create(L"Hikari - Connect to your host", 760, 568, owner)) return false;
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
        if (!offline_ && GetTickCount64() >= next_tailscale_check_) {
            tailscale_ = tailscale_info();
            next_tailscale_check_ = GetTickCount64() + 2000;
        }
        p.background(r);
        const float x=p.s(24),right=r.r-x, split=right-p.s(236), in=x+p.s(22), edge=split-p.s(22);
        p.text("Connection settings",{x,p.s(20),right,p.s(52)},p.s(22),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.text("Pair this PC with the host connected to your Switch.",{x,p.s(56),right,p.s(80)},p.s(13),ui::rgb(th::kText2));
        p.fill({x,p.s(104),split,p.s(446)},ui::rgb(th::kBg2),p.s(12));
        p.fill({split+p.s(16),p.s(104),right,p.s(446)},ui::rgb(th::kBg2),p.s(12));
        p.text("Host details",{in,p.s(122),edge,p.s(150)},p.s(15),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.text("Host address",{in,p.s(174),edge,p.s(198)},p.s(13),ui::rgb(th::kText));
        const Rect host_rect{in,p.s(206),edge,p.s(246)};
        p.fill(host_rect,ui::rgb(th::kSurface),p.s(8));
        p.stroke(host_rect,ui::rgb(GetFocus()==host_?th::kAccent:th::kBorder),p.s(1),p.s(8));
        place(host_,host_rect);
        p.text("Use the host's Tailscale address for remote play.",{in,p.s(252),edge,p.s(286)},p.s(12),ui::rgb(th::kText3),ui::Weight::Regular,ui::Align::Leading,ui::Align::Leading,true);
        p.text("Pairing code",{in,p.s(296),edge,p.s(320)},p.s(13),ui::rgb(th::kText));
        const Rect code_rect{in,p.s(328),edge,p.s(368)};
        p.fill(code_rect,ui::rgb(th::kSurface),p.s(8));
        p.stroke(code_rect,ui::rgb(GetFocus()==code_?th::kAccent:th::kBorder),p.s(1),p.s(8));
        place(code_,code_rect);
        p.text("Find the 12-character code in Hikari on your host PC.",{in,p.s(376),edge,p.s(422)},p.s(12),ui::rgb(th::kText3),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
        const float nx=split+p.s(36),nr=right-p.s(20);
        p.text("Nearby hosts",{nx,p.s(122),nr,p.s(150)},p.s(15),ui::rgb(th::kText),ui::Weight::SemiBold);
        hosts_=discovery_.discovered();
        if(hosts_.empty()) {
            p.text(offline_?"Discovery paused":"Looking for hosts",{nx,p.s(178),nr,p.s(206)},p.s(13),ui::rgb(th::kText));
            p.text(offline_?"Offline preview. Enter a host address to review this form.":"Keep Hikari running on your host PC, or enter its address manually.",{nx,p.s(214),nr,p.s(302)},p.s(12),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Leading,true);
        }
        for(size_t i=0;i<std::min<size_t>(hosts_.size(),2);++i) {
            const float y=p.s(174+100*float(i));
            p.button(ui_,100+int(i),{nx-p.s(4),y,nr+p.s(4),y+p.s(86)},"",ui::ButtonStyle::Secondary);
            p.text(hosts_[i].name,{nx+p.s(8),y+p.s(8),nr-p.s(8),y+p.s(36)},p.s(13),ui::rgb(th::kText),ui::Weight::Medium);
            p.text((hosts_[i].tailscale?"Tailscale: ":"LAN only: ")+hosts_[i].address,{nx+p.s(8),y+p.s(38),nr-p.s(8),y+p.s(72)},p.s(12),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
        }
        p.text(error_,{x,p.s(452),right,p.s(490)},p.s(12),ui::rgb(th::kBad),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
        p.button(ui_,3,{x,p.s(506),x+p.s(148),p.s(544)},tailscale_.installed?"Open Tailscale":"Install Tailscale");
        p.text(offline_?"Offline preview":tailscale_.ipv4.empty()?"Sign in on both PCs with the same account.":"This PC: "+tailscale_.ipv4,
               {x+p.s(160),p.s(500),right-p.s(278),p.s(548)},p.s(12),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
        p.button(ui_,2,{right-p.s(262),p.s(506),right-p.s(166),p.s(544)},"Cancel");
        p.button(ui_,1,{right-p.s(154),p.s(506),right,p.s(544)},"Save and connect",ui::ButtonStyle::Primary);

    }
    void on_action(int id) override
    {
        if (id == 2) { close(); return; }
        if (id == 3) {
            if (!offline_ && !open_tailscale_setup()) error_ = "Could not open Tailscale. Visit tailscale.com/download/windows.";
            return;
        }
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
    TailscaleInfo tailscale_;
    ULONGLONG next_tailscale_check_ = 0;
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
    std::function<bool(HWND)> change_role;
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
        const float x=p.s(24),right=r.r-x,split=x+(right-x-p.s(16))*0.55f,side=split+p.s(16);
        p.text("Host settings",{x,p.s(20),right,p.s(52)},p.s(22),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.button(ui_,5,{right-p.s(144),p.s(24),right,p.s(60)},"Change role");
        p.text(s.host_name,{x,p.s(56),right,p.s(80)},p.s(13),ui::rgb(th::kText2));
        p.fill({x,p.s(104),split,p.s(546)},ui::rgb(th::kBg2),p.s(12));
        p.fill({side,p.s(104),right,p.s(546)},ui::rgb(th::kBg2),p.s(12));
        const float in=x+p.s(22),edge=split-p.s(22),sx=side+p.s(22),sr=right-p.s(22);
        p.text("Connected devices",{in,p.s(122),edge,p.s(150)},p.s(15),ui::rgb(th::kText),ui::Weight::SemiBold);
        struct Check { const char* label; bool ok; std::string detail; };
        const Check checks[]={
            {"Controller bridge",s.bridge_ok,s.bridge_ok?s.bridge_port:"Connect the ESP32-S3 UART port"},
            {"Capture card",s.capture_ok,s.capture_ok?s.capture_name:"Searching for a capture card..."},
            {"HDMI audio",s.audio_ok,s.audio_ok?s.audio_name:"Searching for capture audio..."},
            {"Hardware encoder",s.encoder_ok,s.encoder_benchmarking?"Benchmarking...":s.encoder_name},
        };
        float y=p.s(170);
        for(const auto& check:checks) {
            p.text(check.label,{in,y,edge-p.s(66),y+p.s(24)},p.s(13),ui::rgb(th::kText),ui::Weight::Medium);
            p.text(check.ok?"Ready":"Not ready",{edge-p.s(76),y,edge,y+p.s(24)},p.s(12),ui::rgb(check.ok?th::kGood:th::kWarn),ui::Weight::Regular,ui::Align::Trailing);
            p.text(check.detail,{in,y+p.s(28),edge,y+p.s(66)},p.s(12),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Leading,true);
            p.line(in,y+p.s(76),edge,y+p.s(76),ui::rgb(th::kBorder),p.s(1));
            y+=p.s(88);
        }
        p.text("Pair a client",{sx,p.s(122),sr,p.s(150)},p.s(15),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.text("Use Tailscale on both PCs, signed into the same account.",{sx,p.s(158),sr,p.s(204)},p.s(12),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Leading,true);
        p.text("Pairing code",{sx,p.s(214),sr,p.s(238)},p.s(12),ui::rgb(th::kText2));
        p.fill({sx,p.s(246),sr,p.s(292)},ui::rgb(th::kSurface),p.s(8));
        p.text(s.pairing_code,{sx+p.s(12),p.s(246),sr-p.s(12),p.s(292)},p.s(20),ui::rgb(th::kText),ui::Weight::SemiBold);
        float ay=p.s(314);
        for(const auto& address:s.addresses) {
            if(ay>p.s(396)) break;
            p.text(address.tailscale?"Tailscale address":"LAN address",{sx,ay,sr,ay+p.s(22)},p.s(12),ui::rgb(th::kText2));
            p.text(address.ip+":"+std::to_string(s.port),{sx,ay+p.s(24),sr,ay+p.s(50)},p.s(13),ui::rgb(th::kText));
            ay+=p.s(62);
        }
        p.line(sx,p.s(454),sr,p.s(454),ui::rgb(th::kBorder),p.s(1));
        p.circle(sx+p.s(4),p.s(482),p.s(3),ui::rgb(s.client_connected?th::kGood:th::kText3));
        p.text(s.client_connected?"Connected: "+s.client_name:"Waiting for a client",{sx+p.s(16),p.s(468),sr,p.s(496)},p.s(13),ui::rgb(th::kText));
        p.button(ui_,4,{sx,p.s(510),sx+p.s(164),p.s(544)},"Set up Tailscale");
        p.text(s.message,{x,p.s(558),right,p.s(604)},p.s(12),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
        p.button(ui_,2,{x,p.s(620),x+p.s(118),p.s(658)},"Find devices");
        p.button(ui_,3,{right-p.s(294),p.s(620),right-p.s(142),p.s(658)},"New pairing code");
        p.button(ui_,1,{right-p.s(130),p.s(620),right,p.s(658)},"Wake console",ui::ButtonStyle::Primary);
        p.text("Closing this window keeps the host in the notification area.",{x,p.s(674),right,p.s(700)},p.s(12),ui::rgb(th::kText3));

    }
    void on_action(int id) override
    {
        if (id == 5 && change_role && change_role(hwnd())) { close(); return; }
        if (preview_) return;
        if (id == 1) host_.wake_console();
        if (id == 2) host_.redetect_devices();
        if (id == 4 && !open_tailscale_setup())
            MessageBoxW(hwnd(), L"Open tailscale.com/download/windows to install Tailscale.", L"Hikari", MB_OK);
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
        if (!window.create(L"Hikari - Setup preview", 680, 454)) return 1;
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
        if (!window.create(L"Hikari - Host preview", 900, 724)) return 1;
        window.run_modal();
        return 0;
    }
    if (screen == "controller" || screen == "controller-compact" || screen == "controller-switch2" || screen == "controller-switch2-compact") {
        InputMapping mapping = InputMapping::defaults_positional();
        ControllerSettingsDeps deps;
        if (screen == "controller-switch2" || screen == "controller-switch2-compact") deps.model = joycon::Model::Switch2;
        if (screen == "controller-compact" || screen == "controller-switch2-compact") {
            deps.initial_width = 1040;
            deps.initial_height = 700;
        }
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
    if (screen != "switch2" && screen != "connection-needed" && screen != "ready" && screen != "ready-compact" && screen != "asleep" && screen != "waking" && screen != "failed" && screen != "stream")
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
        if (!window.create(L"Hikari self-test", 680, 454)) return 1;
        // Exercise the real rendering and embedded resources without saving settings.
        SendMessageW(window.hwnd(), WM_PAINT, 0, 0);
        window.close();
        return FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA) ? 0 : 1;
    }
    for (;;) {
        if (setup || (role != "host" && role != "client")) {
            RoleWindow chooser;
            if (!chooser.create(L"Welcome to Hikari", 680, 454)) return 1;
            chooser.run_modal();
            if (chooser.role.empty()) return 0;
            role = chooser.role;
        }
        cfg.set("general", "role", role);
        if (!cfg.save()) {
            MessageBoxW(nullptr, L"Could not save this PC's role. Check access to the Hikari data folder.", L"Hikari", MB_ICONERROR);
            return 1;
        }
        setup = false;
        std::string next_role;
        const auto change_role = [&](HWND owner) {
            RoleWindow chooser;
            if (!chooser.create(L"Hikari - Change role", 680, 454, owner)) return false;
            chooser.run_modal();
            if (chooser.role.empty() || chooser.role == role) return false;
            next_role = chooser.role;
            return true;
        };
        log::init(role.c_str());
        HK_LOGI("Hikari %s starting as %s", HIKARI_VERSION, role.c_str());
        int result = 0;
        if (role == "host") {
            HostApp host;
            if (!host.start(cfg)) {
                MessageBoxW(nullptr, L"The host could not start. Check the Hikari logs for details.", L"Hikari", MB_ICONERROR);
                return 1;
            }
            HostWindow window(host);
            window.change_role = change_role;
            if (!window.create(L"Hikari Host", 900, 724)) return 1;
            window.add_tray();
            window.run_modal();
            host.stop();
            cfg.save();
        } else {
            ClientApp client;
            ClientHooks hooks;
            hooks.open_settings = [&cfg](HWND owner) { return client_settings(owner, cfg); };
            hooks.change_role = change_role;
            result = client.run(cfg, std::move(hooks));
        }
        if (next_role.empty()) return result;
        role = next_role;
    }
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

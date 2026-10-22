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

void menu_header(ui::Painter& p, const Rect& r, const std::string& title, const std::string& subtitle)
{
    const float x=p.s(28), right=r.r-x;
    p.logo({x,p.s(30),x+p.s(28),p.s(58)});
    p.text(title,{x+p.s(42),p.s(24),right,p.s(54)},p.s(22),ui::rgb(th::kText),ui::Weight::SemiBold);
    p.text(subtitle,{x+p.s(42),p.s(54),right,p.s(76)},p.s(13),ui::rgb(th::kText2));
}

// Small line icons drawn into a 44 DIP icon tile.
void role_icon(ui::Painter& p, const Rect& tile, bool host, D2D1_COLOR_F c)
{
    const float w=p.s(1.5f);
    if (host) {
        // Stacked server units.
        const float ix=tile.cx()-p.s(11), iy=tile.cy()-p.s(10);
        for (int unit=0;unit<2;++unit) {
            const float sy=iy+p.s(11*float(unit));
            p.stroke({ix,sy,ix+p.s(22),sy+p.s(9)},c,w,p.s(2));
            p.circle(ix+p.s(4.5f),sy+p.s(4.5f),p.s(1.2f),c);
            p.line(ix+p.s(10),sy+p.s(4.5f),ix+p.s(17),sy+p.s(4.5f),c,w);
        }
    } else {
        // A display on its stand.
        const float ix=tile.cx()-p.s(11), iy=tile.cy()-p.s(10);
        p.stroke({ix,iy,ix+p.s(22),iy+p.s(15)},c,w,p.s(2));
        p.line(ix+p.s(11),iy+p.s(15),ix+p.s(11),iy+p.s(20),c,w);
        p.line(ix+p.s(6),iy+p.s(20.5f),ix+p.s(16),iy+p.s(20.5f),c,w);
    }
}

void network_icon(ui::Painter& p, const Rect& tile, D2D1_COLOR_F c)
{
    const float cx=tile.cx(), cy=tile.cy(), w=p.s(1.5f);
    const D2D1_POINT_2F a{cx,cy-p.s(7)}, b{cx-p.s(8),cy+p.s(6)}, d{cx+p.s(8),cy+p.s(6)};
    p.line(a.x,a.y,b.x,b.y,c,w);
    p.line(a.x,a.y,d.x,d.y,c,w);
    p.line(b.x,b.y,d.x,d.y,c,w);
    for (const auto& n : {a,b,d}) {
        p.circle(n.x,n.y,p.s(3.2f),ui::rgb(0x202226));
        p.circle(n.x,n.y,p.s(3.2f),c,false,w);
    }
}

class RoleWindow final : public D2DWindow {
public:
    std::string role;
private:
    void draw(ui::Painter& p, const Rect& r) override
    {
        p.background(r);
        const float x=p.s(28), right=r.r-x, pitch=p.s(92);
        menu_header(p,r,"How will you use this PC?","Choose a role for this computer. You can change it later.");
        const Rect group{x,p.s(108),right,p.s(108)+pitch*2};
        p.card(group);
        p.divider(group.l+p.s(1),group.r-p.s(1),group.t+pitch);
        const auto choice=[&](int id,float y,bool host,const char* title,const char* description) {
            const Rect row{group.l,y,group.r,y+pitch};
            p.button(ui_,id,row.inset(p.s(6)),"",ui::ButtonStyle::Ghost);
            const bool hover=ui_.hovered(id,row.inset(p.s(6)));
            const Rect tile{row.l+p.s(20),row.cy()-p.s(22),row.l+p.s(64),row.cy()+p.s(22)};
            p.icon_tile(tile);
            role_icon(p,tile,host,ui::rgb(hover?th::kAccent:th::kText));
            p.text(title,{row.l+p.s(82),row.cy()-p.s(22),row.r-p.s(56),row.cy()-p.s(1)},p.s(15),ui::rgb(th::kText),ui::Weight::SemiBold);
            p.text(description,{row.l+p.s(82),row.cy()+p.s(1),row.r-p.s(56),row.cy()+p.s(22)},p.s(13),ui::rgb(th::kText2));
            p.chevron_right(row.r-p.s(28),row.cy(),ui::rgb(hover?th::kText:th::kText3));
        };
        choice(1,group.t,true,"Host","Your Switch and capture hardware connect to this PC.");
        choice(2,group.t+pitch,false,"Client","Connect to your host PC to play remotely.");
        const Rect footer{r.l,r.b-p.s(56),r.r,r.b};
        p.footer_bar(footer);
        p.text("Saved on this PC. Switch anytime with Change role.",{x,footer.t,right,footer.b},p.s(12),ui::rgb(th::kText3));
    }
    void on_action(int id) override { role = id == 1 ? "host" : "client"; close(); }
    void on_key(UINT key) override { if (key == VK_ESCAPE) close(); }
};

class ClientSettings final : public D2DWindow {
public:
    explicit ClientSettings(Config& cfg) : cfg_(cfg) {}
    bool open(HWND owner, bool live = true)
    {
        if (!create(L"Hikari - Connect to your host", 600, 540, owner)) return false;
        host_ = add_edit(cfg_.get("client", "host"));
        code_ = add_edit(cfg_.get("client", "pairing_code"), true, true);
        offline_ = !live;
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
        const float x=p.s(28),right=r.r-x;
        menu_header(p,r,"Connect to your host","Enter the address and pairing code shown on your host PC.");
        const auto entry=[&](HWND edit,float y,const char* label,const char* help) {
            p.text(label,{x,y,right,y+p.s(20)},p.s(13),ui::rgb(th::kText),ui::Weight::Medium);
            const Rect box{x,y+p.s(26),right,y+p.s(66)};
            p.field(box,GetFocus()==edit);
            place(edit,box);
            p.text(help,{x,y+p.s(72),right,y+p.s(92)},p.s(12),ui::rgb(th::kText3));
        };
        entry(host_,p.s(108),"Host address","Use the host's Tailscale address for remote play.");
        entry(code_,p.s(214),"Pairing code","The 12-character code displayed on your host.");

        const Rect ts{x,p.s(334),right,p.s(410)};
        p.card(ts);
        const Rect tile{ts.l+p.s(16),ts.cy()-p.s(22),ts.l+p.s(60),ts.cy()+p.s(22)};
        p.icon_tile(tile);
        network_icon(p,tile,ui::rgb(th::kText));
        const float bw=p.s(148);
        p.text("Tailscale",{tile.r+p.s(14),ts.cy()-p.s(20),ts.r-bw-p.s(32),ts.cy()-p.s(1)},p.s(14),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.text(offline_?"Offline preview":tailscale_.ipv4.empty()?"Sign in on both PCs with the same account.":"This PC: "+tailscale_.ipv4,
               {tile.r+p.s(14),ts.cy()+p.s(1),ts.r-bw-p.s(32),ts.cy()+p.s(20)},p.s(12),ui::rgb(th::kText2));
        p.button(ui_,3,{ts.r-p.s(16)-bw,ts.cy()-p.s(17),ts.r-p.s(16),ts.cy()+p.s(17)},tailscale_.installed?"Open Tailscale":"Install Tailscale");
        p.text(error_,{x,p.s(416),right,p.s(460)},p.s(12.5f),ui::rgb(th::kBad),ui::Weight::Medium,ui::Align::Leading,ui::Align::Center,true);

        const Rect footer{r.l,r.b-p.s(72),r.r,r.b};
        p.footer_bar(footer);
        const float by=footer.t+p.s(18);
        p.text("Your connection is saved on this PC.",{x,footer.t,right-p.s(282),footer.b},p.s(12),ui::rgb(th::kText3));
        p.button(ui_,2,{right-p.s(262),by,right-p.s(166),by+p.s(36)},"Cancel",ui::ButtonStyle::Ghost);
        p.button(ui_,1,{right-p.s(154),by,right,by+p.s(36)},"Save and connect",ui::ButtonStyle::Primary);
    }
    void on_action(int id) override
    {
        if (id == 2) { close(); return; }
        if (id == 3) {
            if (!offline_ && !open_tailscale_setup()) error_ = "Could not open Tailscale. Visit tailscale.com/download/windows.";
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
        const float x=p.s(28),right=r.r-x,split=x+(right-x-p.s(16))*0.56f;
        menu_header(p,r,"Host",s.host_name);
        p.button(ui_,5,{right-p.s(120),p.s(28),right,p.s(64)},"Change role",ui::ButtonStyle::Ghost);

        const Rect devices{x,p.s(108),split,p.s(548)}, pair{split+p.s(16),p.s(108),right,p.s(548)};
        p.card(devices);
        p.card(pair);
        const float in=devices.l+p.s(20),edge=devices.r-p.s(20);
        struct Check { const char* label; bool ok; std::string detail; };
        const Check checks[]={
            {"Controller bridge",s.bridge_ok,s.bridge_ok?s.bridge_port:"Connect the ESP32-S3 UART port"},
            {"Capture card",s.capture_ok,s.capture_ok?s.capture_name:"Searching for a capture card..."},
            {"HDMI audio",s.audio_ok,s.audio_ok?s.audio_name:"Searching for capture audio..."},
            {"Hardware encoder",s.encoder_ok,s.encoder_benchmarking?"Benchmarking...":s.encoder_name},
        };
        p.text("Connected devices",{in,devices.t+p.s(18),edge,devices.t+p.s(44)},p.s(15),ui::rgb(th::kText),ui::Weight::SemiBold);
        const float pitch=p.s(78);
        float y=devices.t+p.s(62);
        for(int i=0;i<4;++i) {
            const auto& check=checks[i];
            if(i>0) p.divider(in,edge,y);
            const float cy=y+pitch/2;
            p.status_icon(in+p.s(10),cy,check.ok);
            p.text(check.label,{in+p.s(34),cy-p.s(20),edge-p.s(84),cy-p.s(1)},p.s(13.5f),ui::rgb(th::kText),ui::Weight::Medium);
            p.text(check.detail,{in+p.s(34),cy+p.s(1),edge,cy+p.s(20)},p.s(12),ui::rgb(th::kText2));
            p.text(check.ok?"Ready":"Not ready",{edge-p.s(84),cy-p.s(20),edge,cy-p.s(1)},p.s(12),ui::rgb(check.ok?th::kGood:th::kWarn),ui::Weight::Medium,ui::Align::Trailing);
            y+=pitch;
        }

        const float sx=pair.l+p.s(20),sr=pair.r-p.s(20);
        p.text("Pair a client",{sx,pair.t+p.s(18),sr,pair.t+p.s(44)},p.s(15),ui::rgb(th::kText),ui::Weight::SemiBold);
        p.text("Use Tailscale on both PCs, signed into the same account.",{sx,pair.t+p.s(46),sr,pair.t+p.s(84)},p.s(12.5f),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Leading,true);
        p.text("Pairing code",{sx,pair.t+p.s(94),sr,pair.t+p.s(112)},p.s(12),ui::rgb(th::kText2),ui::Weight::Medium);
        const Rect code{sx,pair.t+p.s(118),sr,pair.t+p.s(166)};
        p.fill(code,ui::rgb(th::kBg),p.s(8));
        p.stroke(code,ui::rgb(th::kBorderStrong),p.s(1),p.s(8));
        p.mono(s.pairing_code,code.inset(p.s(16),0),p.s(19),ui::rgb(th::kText));
        float ay=pair.t+p.s(184);
        for(const auto& address:s.addresses) {
            if(ay>pair.t+p.s(270)) break;
            p.text(address.tailscale?"Tailscale address":"LAN address",{sx,ay,sr,ay+p.s(32)},p.s(12.5f),ui::rgb(th::kText2));
            p.mono(address.ip+":"+std::to_string(s.port),{sx,ay,sr,ay+p.s(32)},p.s(12.5f),ui::rgb(th::kText),ui::Align::Trailing);
            p.divider(sx,sr,ay+p.s(32));
            ay+=p.s(36);
        }
        p.divider(sx,sr,pair.b-p.s(104));
        p.circle(sx+p.s(4),pair.b-p.s(74),p.s(3),ui::rgb(s.client_connected?th::kGood:th::kText3));
        p.text(s.client_connected?"Connected: "+s.client_name:"Waiting for a client",{sx+p.s(16),pair.b-p.s(88),sr,pair.b-p.s(60)},p.s(13),ui::rgb(th::kText));
        p.button(ui_,4,{sx,pair.b-p.s(52),sx+p.s(156),pair.b-p.s(16)},"Set up Tailscale");

        p.text(s.message,{x,p.s(560),right,p.s(600)},p.s(12),ui::rgb(th::kText2),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
        const Rect footer{r.l,r.b-p.s(76),r.r,r.b};
        p.footer_bar(footer);
        const float by=footer.t+p.s(20);
        p.button(ui_,2,{x,by,x+p.s(124),by+p.s(36)},"Find devices");
        p.text("Closing this window keeps the host in the notification area.",{x+p.s(140),footer.t,right-p.s(310),footer.b},p.s(12),ui::rgb(th::kText3),ui::Weight::Regular,ui::Align::Leading,ui::Align::Center,true);
        p.button(ui_,3,{right-p.s(298),by,right-p.s(146),by+p.s(36)},"New pairing code");
        p.button(ui_,1,{right-p.s(134),by,right,by+p.s(36)},"Wake console",ui::ButtonStyle::Primary);
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
        if (!window.create(L"Hikari - Setup preview", 680, 400)) return 1;
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
        if (!window.create(L"Hikari - Host preview", 900, 688)) return 1;
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
    if (screen != "switch2" && screen != "connection-needed" && screen != "ready" && screen != "ready-compact" && screen != "asleep" && screen != "waking" && screen != "failed" && screen != "stream" && screen != "stream-clean")
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
        if (!window.create(L"Hikari self-test", 680, 400)) return 1;
        // Exercise the real rendering and embedded resources without saving settings.
        SendMessageW(window.hwnd(), WM_PAINT, 0, 0);
        window.close();
        return FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA) ? 0 : 1;
    }
    for (;;) {
        if (setup || (role != "host" && role != "client")) {
            RoleWindow chooser;
            if (!chooser.create(L"Welcome to Hikari", 680, 400)) return 1;
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
            if (!chooser.create(L"Hikari - Change role", 680, 400, owner)) return false;
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
            if (!window.create(L"Hikari Host", 900, 688)) return 1;
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

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <string>
#include <vector>

#include "client_app.h"
#include "controller_settings.h"
#include "d2d_window.h"
#include "host_app.h"
#include "hikari/crypto.h"
#include "hikari/invite.h"
#include "host_probe.h"
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

bool copy_to_clipboard(HWND owner, const std::string& text)
{
    const std::wstring w = widen(text);
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

class ClientSettings final : public D2DWindow {
public:
    explicit ClientSettings(Config& cfg) : cfg_(cfg) {}
    ~ClientSettings() override
    {
        cancel_ = true;
        if (worker_.joinable()) worker_.join();
    }
    bool open(HWND owner, bool live = true)
    {
        if (!create(L"Hikari - Connect to your host", 600, 624, owner)) return false;
        invite_ = add_edit("");
        host_ = add_edit(cfg_.get("client", "host"));
        code_ = add_edit(cfg_.get("client", "pairing_code"), false, true);
        fallback_ = cfg_.get("client", "host_fallback");
        offline_ = !live;
        SetFocus(invite_);
        return true;
    }
    bool saved = false;

private:
    LRESULT on_message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override
    {
        if (msg == WM_COMMAND && HIWORD(wp) == EN_CHANGE && !filling_) {
            const HWND from = reinterpret_cast<HWND>(lp);
            if (from == invite_) {
                Invite inv;
                invite_ok_ = parse_invite(text_of(invite_), inv);
                if (invite_ok_) {
                    // Fill the manual fields so the user can see what will be used.
                    filling_ = true;
                    set_text(host_, inv.host);
                    set_text(code_, inv.code);
                    filling_ = false;
                    fallback_ = inv.fallback;
                    error_.clear();
                    save_anyway_ = false;
                }
            } else if (from == host_) {
                fallback_.clear();  // typed by hand: the invite's fallback no longer applies
                invite_ok_ = false;
            }
            handled = true;
            return 0;
        }
        return D2DWindow::on_message(msg, wp, lp, handled);
    }

    void field(ui::Painter& p, HWND edit, const Rect& rect)
    {
        p.fill(rect, ui::rgb(th::kSurface), p.s(6));
        p.stroke(rect, ui::rgb(GetFocus() == edit ? th::kAccent : th::kBorder), p.s(1), p.s(6));
        place(edit, rect);
    }

    void draw(ui::Painter& p, const Rect& r) override
    {
        if (!offline_ && GetTickCount64() >= next_tailscale_check_) {
            tailscale_ = tailscale_info();
            next_tailscale_check_ = GetTickCount64() + 2000;
        }
        p.background(r);
        const float x = p.s(28), right = r.r - x;
        menu_header(p, r, "Connect to your host", "Paste the invite from your host PC. Hikari checks it before saving.");

        p.text("Invite", {x, p.s(122), right, p.s(148)}, p.s(15), ui::rgb(th::kText), ui::Weight::SemiBold);
        field(p, invite_, {x, p.s(152), right, p.s(192)});
        if (invite_ok_)
            p.text("Invite recognised - host and pairing code filled in below.", {x, p.s(198), right, p.s(222)}, p.s(12),
                   ui::rgb(th::kGood));
        else
            p.text("On the host PC, click Copy invite, then paste it here (Ctrl+V).", {x, p.s(198), right, p.s(222)},
                   p.s(12), ui::rgb(th::kText3));

        p.text("Or enter the details yourself", {x, p.s(240), right, p.s(262)}, p.s(13), ui::rgb(th::kText2));
        p.text("Host address", {x, p.s(270), right, p.s(292)}, p.s(13), ui::rgb(th::kText));
        field(p, host_, {x, p.s(296), right, p.s(336)});
        p.text(fallback_.empty() ? "The host's Tailscale name or address."
                                 : "If this name can't be found, Hikari uses " + fallback_ + ".",
               {x, p.s(342), right, p.s(364)}, p.s(12), ui::rgb(th::kText3));
        p.text("Pairing code", {x, p.s(378), right, p.s(400)}, p.s(13), ui::rgb(th::kText));
        field(p, code_, {x, p.s(404), right, p.s(444)});

        p.button(ui_, 3, {x, p.s(458), x + p.s(148), p.s(494)}, tailscale_.installed ? "Open Tailscale" : "Install Tailscale");
        p.text(offline_ ? "Offline preview"
               : tailscale_.ipv4.empty() ? "Tailscale is not connected on this PC. Sign in with the same account as the host."
                                         : "Tailscale connected (this PC: " + tailscale_.ipv4 + ").",
               {x + p.s(160), p.s(452), right, p.s(500)}, p.s(12),
               ui::rgb(offline_ || !tailscale_.ipv4.empty() ? th::kText2 : th::kWarn), ui::Weight::Regular,
               ui::Align::Leading, ui::Align::Center, true);

        const Rect status{x, p.s(506), right, p.s(550)};
        if (checking_) {
            p.spinner(x + p.s(10), status.cy(), p.s(8), ui::rgb(th::kAccent));
            p.text("Contacting " + checking_host_ + "...", {x + p.s(28), status.t, right, status.b}, p.s(13),
                   ui::rgb(th::kText2));
        } else if (!error_.empty()) {
            p.text(error_, status, p.s(12), ui::rgb(th::kBad), ui::Weight::Regular, ui::Align::Leading, ui::Align::Center,
                   true);
        }

        p.line(x, p.s(558), right, p.s(558), ui::rgb(th::kBorder, 0.5f), p.s(1));
        if (save_anyway_ && !checking_)
            p.button(ui_, 4, {x, p.s(572), x + p.s(132), p.s(610)}, "Save anyway", ui::ButtonStyle::Ghost);
        p.button(ui_, 2, {right - p.s(262), p.s(572), right - p.s(166), p.s(610)}, "Cancel", ui::ButtonStyle::Ghost);
        p.button(ui_, 1, {right - p.s(154), p.s(572), right, p.s(610)}, checking_ ? "Checking..." : "Save and connect",
                 ui::ButtonStyle::Primary, !checking_);
    }

    bool read_fields(std::string& host, std::string& code)
    {
        host = text_of(host_);
        code = crypto::normalize_pairing_code(text_of(code_));
        while (!host.empty() && host.back() == ' ') host.pop_back();
        while (!host.empty() && host.front() == ' ') host.erase(host.begin());
        if (host.empty() || code.size() != 12) {
            error_ = "Paste an invite, or enter a host address and the 12-character pairing code.";
            return false;
        }
        return true;
    }

    void save(const std::string& host, const std::string& code, const std::string& host_name)
    {
        // Save through a copy so a failed write leaves the active settings intact.
        Config next = cfg_;
        next.set("client", "host", host);
        next.set("client", "host_fallback", fallback_);
        next.set("client", "pairing_code", code);
        if (!host_name.empty()) next.set("client", "host_name", host_name);
        if (!next.save()) {
            error_ = "Could not save settings. Check access to the Hikari data folder.";
            return;
        }
        cfg_ = std::move(next);
        saved = true;
        close();
    }

    void on_action(int id) override
    {
        if (id == 2) {
            close();
            return;
        }
        if (id == 3) {
            if (!offline_ && !open_tailscale_setup()) error_ = "Could not open Tailscale. Visit tailscale.com/download/windows.";
            return;
        }
        std::string host, code;
        if (id == 4) {
            if (read_fields(host, code)) save(host, code, "");
            return;
        }
        if (id != 1 || checking_ || !read_fields(host, code)) return;
        if (offline_) {
            save(host, code, "");
            return;
        }
        // Verify before saving: a typo should show up here, not later as a
        // mysterious "can't connect" in the lobby.
        error_.clear();
        save_anyway_ = false;
        checking_ = true;
        checking_host_ = host;
        if (worker_.joinable()) worker_.join();
        done_ = false;
        worker_ = std::thread([this, host, code, fallback = fallback_] {
            outcome_ = probe_host({host, fallback}, code, &cancel_);
            done_ = true;
        });
    }

    void on_timer() override
    {
        if (!checking_ || !done_) return;
        worker_.join();
        checking_ = false;
        std::string host, code;
        read_fields(host, code);
        const std::string where = checking_host_;
        switch (outcome_.result) {
        case ProbeResult::Ok:
            save(host, code, outcome_.host_name);
            return;
        case ProbeResult::WrongCode:
            error_ = "The host rejected this pairing code. Copy a fresh invite on the host PC (codes change when you "
                     "press New pairing code).";
            break;
        case ProbeResult::VersionMismatch:
            error_ = "The host is running a different version of Hikari. Install the same version on both PCs.";
            break;
        case ProbeResult::CannotResolve:
            error_ = "Couldn't find \"" + where + "\". Check the address, make sure Tailscale is connected on this PC, "
                     "or use the host's 100.x.x.x address.";
            save_anyway_ = true;
            break;
        case ProbeResult::NoAnswer:
            error_ = "No answer from " + where + ". Check that Hikari is running on the host PC, both PCs are signed "
                     "in to Tailscale with the same account, and the host allowed Hikari through Windows Firewall.";
            save_anyway_ = true;  // the host may simply be off right now
            break;
        case ProbeResult::Cancelled:
            break;
        }
    }

    void on_key(UINT key) override
    {
        if (key == VK_ESCAPE) close();
    }

    Config& cfg_;
    HWND invite_ = nullptr, host_ = nullptr, code_ = nullptr;
    std::string fallback_;
    bool invite_ok_ = false;
    bool filling_ = false;
    std::string error_;
    bool save_anyway_ = false;
    bool offline_ = false;
    TailscaleInfo tailscale_;
    ULONGLONG next_tailscale_check_ = 0;

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> done_{false};
    bool checking_ = false;
    std::string checking_host_;
    ProbeOutcome outcome_;
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
    HostWindow(HostApp& host, Config* cfg, bool preview = false) : host_(host), cfg_(cfg), preview_(preview) {}
    std::function<bool(HWND)> change_role;
    ~HostWindow() override
    {
        if (tray_.hWnd) Shell_NotifyIconW(NIM_DELETE, &tray_);
        if (fix_.joinable()) fix_.join();
    }
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
    HostSnapshot sample() const
    {
        HostSnapshot s;
        s.host_name = "Living room - Offline preview";
        s.pairing_code = "DEMO-CODE-ONLY";
        s.addresses = {{"192.0.2.10", "Sample LAN", false}};
        s.tailscale_name = "living-room.example.ts.net";
        s.invite = "hikari://living-room.example.ts.net/DEMO-CODE-ONLY";
        s.bridge_ok = s.capture_ok = s.audio_ok = s.encoder_ok = true;
        s.bridge_port = "ESP32-S3 controller bridge";
        s.capture_name = "HDMI capture - 1920 x 1080 at 60 fps";
        s.audio_name = "Capture card HDMI audio";
        s.encoder_name = "Hardware H.264 encoder";
        s.firewall.checked = true;
        s.firewall.enabled = true;
        s.firewall.allowed = true;
        s.lid_action = 0;
        s.autostart = true;
        s.message = "Sample device status. No hardware or network connection is active.";
        return s;
    }

    // Runs a fix that may show a UAC prompt without freezing the window.
    void run_fix(std::function<bool()> fn, const char* failed)
    {
        if (fixing_ || preview_) return;
        if (fix_.joinable()) fix_.join();
        fixing_ = true;
        fix_ = std::thread([this, fn = std::move(fn), failed] {
            if (!fn()) fix_error_ = failed;
            fixing_ = false;
        });
    }

    void draw(ui::Painter& p, const Rect& r) override
    {
        p.background(r);
        const HostSnapshot s = preview_ ? sample() : host_.snapshot();
        last_ = s;

        // First run: ask once to let clients through the firewall, before the
        // user ever sees a "can't reach host" on the other PC.
        if (!preview_ && cfg_ && s.firewall.checked && !s.firewall.ok() && !cfg_->get_bool("host", "firewall_asked", false)) {
            cfg_->set_bool("host", "firewall_asked", true);
            cfg_->save();
            run_fix([this] { return host_.allow_through_firewall(); }, "Windows Firewall was not changed.");
        }

        const float x = p.s(24), right = r.r - x, split = x + (right - x - p.s(16)) * 0.55f, side = split + p.s(16);
        menu_header(p, r, "Host", s.host_name);
        p.button(ui_, 5, {right - p.s(144), p.s(24), right, p.s(60)}, "Change role", ui::ButtonStyle::Ghost);
        p.line(split + p.s(8), p.s(124), split + p.s(8), p.s(652), ui::rgb(th::kBorder, 0.5f), p.s(1));
        const float in = x + p.s(4), edge = split - p.s(24), sx = side + p.s(24), sr = right - p.s(4);

        // Left: hardware.
        p.text("Connected devices", {in, p.s(122), edge, p.s(150)}, p.s(15), ui::rgb(th::kText), ui::Weight::SemiBold);
        struct Check {
            const char* label;
            bool ok;
            std::string detail;
        };
        const Check checks[] = {
            {"Controller bridge", s.bridge_ok, s.bridge_ok ? s.bridge_port : "Connect the ESP32-S3 UART port"},
            {"Capture card", s.capture_ok, s.capture_ok ? s.capture_name : "Searching for a capture card..."},
            {"HDMI audio", s.audio_ok, s.audio_ok ? s.audio_name : "Searching for capture audio..."},
            {"Hardware encoder", s.encoder_ok, s.encoder_benchmarking ? "Benchmarking..." : s.encoder_name},
        };
        float y = p.s(166);
        for (const auto& check : checks) {
            p.text(check.label, {in, y, edge - p.s(66), y + p.s(24)}, p.s(13), ui::rgb(th::kText), ui::Weight::Medium);
            p.text(check.ok ? "Ready" : "Not ready", {edge - p.s(76), y, edge, y + p.s(24)}, p.s(12),
                   ui::rgb(check.ok ? th::kGood : th::kWarn), ui::Weight::Regular, ui::Align::Trailing);
            p.text(check.detail, {in, y + p.s(26), edge, y + p.s(52)}, p.s(12), ui::rgb(th::kText2), ui::Weight::Regular,
                   ui::Align::Leading, ui::Align::Leading, true);
            p.line(in, y + p.s(60), edge, y + p.s(60), ui::rgb(th::kBorder, 0.5f), p.s(1));
            y += p.s(70);
        }

        // Left: keeping this PC reachable.
        p.text("Always reachable", {in, p.s(456), edge, p.s(484)}, p.s(15), ui::rgb(th::kText), ui::Weight::SemiBold);
        const auto row = [&](float ry, const char* label, const std::string& status, uint32_t color) {
            p.text(label, {in, ry, edge - p.s(190), ry + p.s(34)}, p.s(13), ui::rgb(th::kText), ui::Weight::Medium);
            p.text(status, {edge - p.s(190), ry, edge - p.s(96), ry + p.s(34)}, p.s(12), ui::rgb(color),
                   ui::Weight::Regular, ui::Align::Trailing);
        };
        const Rect action1{edge - p.s(86), p.s(494), edge, p.s(526)};
        if (!s.firewall.checked) {
            row(p.s(494), "Windows Firewall", "Checking...", th::kText3);
        } else if (s.firewall.ok()) {
            row(p.s(494), "Windows Firewall", "Allowed", th::kGood);
        } else {
            row(p.s(494), "Windows Firewall", s.firewall.blocked ? "Blocked" : "May block clients", th::kWarn);
            if (p.button(ui_, 10, action1, fixing_ ? "..." : "Allow", ui::ButtonStyle::Primary, !fixing_)) {}
        }
        row(p.s(536), "Start with Windows", s.autostart ? "On" : "Off", s.autostart ? th::kGood : th::kWarn);
        p.toggle(ui_, 11, {edge - p.s(58), p.s(542), edge - p.s(14), p.s(564)}, s.autostart);
        const Rect action3{edge - p.s(86), p.s(578), edge, p.s(610)};
        if (s.lid_action < 0) {
            row(p.s(578), "Lid closed", "No lid", th::kText3);
        } else if (s.lid_action == 0) {
            row(p.s(578), "Lid closed", "Stays awake", th::kGood);
        } else {
            row(p.s(578), "Lid closed", s.lid_action == 1 ? "PC sleeps" : s.lid_action == 2 ? "PC hibernates" : "PC shuts down",
                th::kWarn);
            if (p.button(ui_, 12, action3, fixing_ ? "..." : "Keep awake", ui::ButtonStyle::Primary, !fixing_)) {}
        }
        const char* fix_error = fix_error_.load();
        p.text(fix_error ? std::string(fix_error)
               : s.autostart ? "Hikari starts when you sign in to Windows, so a reboot doesn't take your Switch offline."
                             : "Without auto-start, a reboot leaves your Switch unreachable until Hikari is opened.",
               {in, p.s(616), edge, p.s(652)}, p.s(11.5f), ui::rgb(fix_error ? th::kBad : th::kText3),
               ui::Weight::Regular, ui::Align::Leading, ui::Align::Leading, true);

        // Right: pairing.
        p.text("Pair a client", {sx, p.s(122), sr, p.s(150)}, p.s(15), ui::rgb(th::kText), ui::Weight::SemiBold);
        p.text("Copy the invite and paste it into Hikari on the PC you'll play from. Both PCs need Tailscale, signed in "
               "to the same account.",
               {sx, p.s(154), sr, p.s(200)}, p.s(12), ui::rgb(th::kText2), ui::Weight::Regular, ui::Align::Leading,
               ui::Align::Leading, true);
        const Rect invite_box{sx, p.s(208), sr, p.s(248)};
        p.fill(invite_box, ui::rgb(th::kSurface), p.s(6));
        p.text(s.invite.empty() ? "Waiting for a network address..." : s.invite, invite_box.inset(p.s(10), 0), p.s(12.5f),
               ui::rgb(s.invite.empty() ? th::kText3 : th::kText));
        const bool copied = GetTickCount64() < copied_until_;
        p.button(ui_, 6, {sx, p.s(256), sx + p.s(118), p.s(290)}, copied ? "Copied" : "Copy invite",
                 ui::ButtonStyle::Primary, !s.invite.empty());
        p.button(ui_, 4, {sx + p.s(126), p.s(256), sx + p.s(276), p.s(290)}, "Set up Tailscale");

        p.text("Pairing code", {sx, p.s(310), sr, p.s(332)}, p.s(12), ui::rgb(th::kText2));
        p.text(s.pairing_code, {sx, p.s(332), sr, p.s(364)}, p.s(19), ui::rgb(th::kText), ui::Weight::SemiBold);
        p.text("This PC on Tailscale", {sx, p.s(378), sr, p.s(400)}, p.s(12), ui::rgb(th::kText2));
        std::string ts = !s.tailscale_name.empty() ? s.tailscale_name : s.tailscale_ip;
        if (!s.tailscale_name.empty() && !s.tailscale_ip.empty()) ts += "  (" + s.tailscale_ip + ")";
        p.text(ts.empty() ? "Not connected - open Tailscale and sign in" : ts, {sx, p.s(400), sr, p.s(424)}, p.s(13),
               ui::rgb(ts.empty() ? th::kWarn : th::kText));
        std::string lan;
        for (const auto& a : s.addresses)
            if (!a.tailscale) {
                lan = a.ip;
                break;
            }
        if (!lan.empty())
            p.text("On your home network: " + lan + (s.port == proto::kDefaultPort ? "" : ":" + std::to_string(s.port)),
                   {sx, p.s(428), sr, p.s(450)}, p.s(12), ui::rgb(th::kText2));

        p.line(sx, p.s(468), sr, p.s(468), ui::rgb(th::kBorder), p.s(1));
        p.circle(sx + p.s(4), p.s(496), p.s(3), ui::rgb(s.client_connected ? th::kGood : th::kText3));
        p.text(s.client_connected ? "Connected: " + s.client_name : "Waiting for a client", {sx + p.s(16), p.s(482), sr, p.s(510)},
               p.s(13), ui::rgb(th::kText));

        p.line(x, p.s(668), right, p.s(668), ui::rgb(th::kBorder, 0.5f), p.s(1));
        p.text(s.message, {x, p.s(674), right, p.s(702)}, p.s(12), ui::rgb(th::kText2), ui::Weight::Regular,
               ui::Align::Leading, ui::Align::Center, true);
        p.button(ui_, 2, {x, p.s(714), x + p.s(118), p.s(752)}, "Find devices");
        p.button(ui_, 3, {right - p.s(294), p.s(714), right - p.s(142), p.s(752)}, "New pairing code");
        p.button(ui_, 1, {right - p.s(130), p.s(714), right, p.s(752)}, "Wake console", ui::ButtonStyle::Primary);
        p.text("Closing this window keeps the host running in the notification area.", {x, p.s(760), right, p.s(788)},
               p.s(12), ui::rgb(th::kText3));
    }

    void on_action(int id) override
    {
        if (id == 5 && change_role && change_role(hwnd())) {
            close();
            return;
        }
        if (id == 6 && !last_.invite.empty() && copy_to_clipboard(hwnd(), last_.invite)) copied_until_ = GetTickCount64() + 2000;
        if (preview_) return;
        fix_error_ = nullptr;
        if (id == 1) host_.wake_console();
        if (id == 2) host_.redetect_devices();
        if (id == 4 && !open_tailscale_setup())
            MessageBoxW(hwnd(), L"Open tailscale.com/download/windows to install Tailscale.", L"Hikari", MB_OK);
        if (id == 3 && MessageBoxW(hwnd(), L"Replace the pairing code? Other PCs will need the new invite to connect.",
                                   L"Hikari", MB_YESNO | MB_ICONQUESTION) == IDYES)
            host_.regenerate_pairing_code();
        if (id == 10) run_fix([this] { return host_.allow_through_firewall(); }, "Windows Firewall was not changed.");
        if (id == 11 && !host_.set_start_with_windows(!last_.autostart)) fix_error_ = "Could not change auto-start.";
        if (id == 12)
            run_fix([this] { return host_.keep_awake_with_lid_closed(); }, "The power setting could not be changed.");
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
                AppendMenuW(menu, MF_STRING, 3, L"Copy invite");
                AppendMenuW(menu, MF_STRING, 2, L"Quit");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd());
                const int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd(), nullptr);
                DestroyMenu(menu);
                if (id == 1) ShowWindow(hwnd(), SW_RESTORE);
                if (id == 3) copy_to_clipboard(hwnd(), (preview_ ? sample() : host_.snapshot()).invite);
                if (id == 2)
                    close();
                else
                    PostMessageW(hwnd(), WM_NULL, 0, 0);
            }
            return 0;
        }
        return D2DWindow::on_message(msg, wp, lp, handled);
    }

    HostApp& host_;
    Config* cfg_ = nullptr;
    NOTIFYICONDATAW tray_{};
    bool have_tray_ = false;
    bool preview_ = false;
    HostSnapshot last_;
    ULONGLONG copied_until_ = 0;
    std::thread fix_;
    std::atomic<bool> fixing_{false};
    std::atomic<const char*> fix_error_{nullptr};  // string literals only (set by the fix thread)
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
    cfg.set("client", "console_model", "switch");  // previews start from Switch; switch2 previews select it explicitly
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
        HostWindow window(host, nullptr, true);
        if (!window.create(L"Hikari - Host preview", 900, 800)) return 1;
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
    if (screen != "switch2" && screen != "connection-needed" && screen != "ready" && screen != "ready-compact" && screen != "asleep" && screen != "waking" && screen != "failed" && screen != "stream" && screen != "stream-menu" && screen != "stream-end" && screen != "ended")
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
    bool setup = false, selftest = false, minimized = false;
    for (const auto& arg : args) {
        // Elevated helper (firewall rule / power plan), started by the host
        // behind a single UAC prompt. Does its job and exits; no UI.
        if (arg.rfind("--elevated-setup=", 0) == 0) {
            uint16_t port = proto::kDefaultPort;
            for (const auto& a : args)
                if (a.rfind("--port=", 0) == 0) port = static_cast<uint16_t>(std::atoi(a.c_str() + 7));
            return elevated_setup_main(arg.substr(17), port);
        }
        if (arg == "--minimized") minimized = true;
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
        const auto start_log = [&] {
            log::init(role.c_str());
            HK_LOGI("Hikari %s starting as %s", HIKARI_VERSION, role.c_str());
        };
        int result = 0;
        if (role == "host") {
            // Closing the window only hides the host to the tray, so a second launch would
            // fail to bind the port. Bring the running host back instead.
            HANDLE instance = CreateMutexW(nullptr, TRUE, L"Local\\HikariHost");
            if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(instance);
                if (HWND existing = FindWindowW(L"HikariWindow", L"Hikari Host"); existing && !minimized) {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                }
                return 0;
            }
            struct InstanceGuard {
                HANDLE h;
                ~InstanceGuard()
                {
                    if (h) CloseHandle(h);
                }
            } guard{instance};
            start_log();  // only now: a second launch must not rotate the running host's log
            HostApp host;
            if (!host.start(cfg)) {
                MessageBoxW(nullptr, L"The host could not start. Check the Hikari logs for details.", L"Hikari", MB_ICONERROR);
                return 1;
            }
            HostWindow window(host, &cfg);
            window.change_role = change_role;
            if (!window.create(L"Hikari Host", 900, 800)) return 1;
            window.add_tray();
            window.run_modal(!minimized);
            host.stop();
            cfg.save();
        } else {
            start_log();
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

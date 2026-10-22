// hikari-pad: drive the console from the host through the controller bridge
// (press buttons, move sticks, wake it) and inspect the bridge's state.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bridge.h"
#include "bridge_util.h"
#include "hikari/clock.h"
#include "hikari/system.h"
#include "tool_common.h"

using namespace hikari;
using namespace hikari::tool;

namespace {

const char* kHelp = R"(hikari-pad - control the Switch through the Hikari controller bridge

Usage:
  hikari-pad press BUTTON[+BUTTON...]       tap (--ms, default 100)
  hikari-pad hold BUTTON[+...] MS           hold for MS milliseconds
  hikari-pad stick L|R X Y                  tilt a stick, X/Y in -1..1 (+Y up),
                                            held for --ms (default 300)
  hikari-pad seq "A,200;B,200;HOME,100"     run steps BUTTONS,MS in order;
                                            "wait,MS" pauses, "L:x:y,MS" /
                                            "R:x:y,MS" tilt a stick
  hikari-pad wake [--methods usb,home,power] [--press-ms 150]
  hikari-pad status                         bridge, console USB state, telemetry
  hikari-pad interactive                    live keyboard control

Buttons: A B X Y L R ZL ZR PLUS MINUS HOME CAPTURE LS RS UP DOWN LEFT RIGHT
(case-insensitive; combine with '+', e.g. L+R or ZL+ZR+A).

Options:
  --ms N           press/tilt duration for press/stick (ms)
  --gap N          release time between seq steps (default 80 ms)
  --methods LIST   wake methods: usb (USB remote wakeup), home (press HOME
                   once the bus resumes), power (power-button GPIO), cec
                   (default usb,home,power)
  --press-ms N     power-button pulse / HOME hold for wake (default 150)
  --wait N         seconds wake waits for the console to come up (default 8)
  --port COMx      try this serial port first
  --baud N         link baud rate (default 2000000)
  --verbose        show library log output
  --help           this text

The neutral state is always published before exit (also on Ctrl+C).
Exit codes: 0 ok, 1 command failed, 2 no bridge found, 64 bad arguments.
)";

std::atomic<bool> g_stop{false};
Bridge* g_bridge = nullptr;

BOOL WINAPI on_ctrl(DWORD)
{
    g_stop = true;
    if (g_bridge) g_bridge->publish_neutral();
    return TRUE;
}

// Interruptible sleep (Ctrl+C ends holds early).
void wait_ms(double ms)
{
    PreciseTimer timer;
    const uint64_t end = now_us() + static_cast<uint64_t>(std::max(0.0, ms) * 1000);
    while (!g_stop && now_us() < end) timer.sleep_until_us(std::min(end, now_us() + 20'000));
}

int16_t axis(double v) { return static_cast<int16_t>(std::lround(std::clamp(v, -1.0, 1.0) * 32767)); }

bool parse_stick(const std::string& which, const std::string& xs, const std::string& ys, proto::PadState& p)
{
    double x = 0, y = 0;
    if (!parse_double(xs, x) || !parse_double(ys, y) || x < -1 || x > 1 || y < -1 || y > 1) return false;
    const std::string w = upper(which);
    if (w == "L") {
        p.lx = axis(x);
        p.ly = axis(y);
    } else if (w == "R") {
        p.rx = axis(x);
        p.ry = axis(y);
    } else {
        return false;
    }
    return true;
}

void print_status(Bridge& bridge)
{
    const BridgeStatus st = bridge.status();
    std::printf("Bridge        %s on %s, firmware %u.%u\n", st.connected ? "connected" : "disconnected",
                st.port.c_str(), st.fw_version >> 8, st.fw_version & 0xFF);
    std::printf("Console USB   %s\n", usb_state_name(st.usb_state));
    std::string wm;
    if (st.wake_methods & HK_WAKE_USB_REMOTE_WAKEUP) wm += " usb";
    if (st.wake_methods & HK_WAKE_HOME_PRESS) wm += " home";
    if (st.wake_methods & HK_WAKE_POWER_BUTTON) wm += " power";
    if (st.wake_methods & HK_WAKE_HDMI_CEC) wm += " cec";
    std::printf("Wake methods %s  (remote wakeup %s by console)\n", wm.empty() ? " none" : wm.c_str(),
                st.remote_wakeup_enabled ? "enabled" : "NOT enabled");
    std::printf("Failsafe      %s\n", st.failsafe ? "active (neutral output)" : "inactive (host in control)");
    std::printf("Link RTT      %s\n", st.link_rtt.count ? st.link_rtt.to_ms_string().c_str() : "(no samples yet)");
    std::printf("Host write    %s\n",
                st.host_write.count ? st.host_write.to_ms_string().c_str() : "(no pad writes in the last 2 s)");
    if (!st.telemetry_valid) {
        std::printf("Telemetry     (none received yet)\n");
        return;
    }
    const hk_telemetry& t = st.telemetry;
    std::printf("Telemetry     uptime %.1f s, session %08x\n", t.uptime_ms / 1000.0, t.session_id);
    std::printf("  rx packets  %u total, %u/s, invalid %u, seq gaps %u, stale %u\n", t.rx_packets,
                t.rx_packets_per_s, t.rx_invalid, t.rx_seq_gaps, t.rx_stale);
    std::printf("  USB reports %u total, %u/s, failed %u\n", t.usb_reports, t.usb_reports_per_s, t.usb_fail);
    std::printf("  rx->submit    p50/p95/p99/max %s\n", lat4(t.rx_to_submit).c_str());
    std::printf("  rx->consumed  p50/p95/p99/max %s\n", lat4(t.rx_to_consumed).c_str());
    std::printf("  poll interval p50/p95/p99/max %s\n", lat4(t.poll_interval).c_str());
    if (st.last_wake_event_us)
        std::printf("Last wake     method 0x%02x result %u, %.1f s ago\n", (st.last_wake_event >> 8) & 0xFF,
                    st.last_wake_event & 0xFF, double(now_us() - st.last_wake_event_us) / 1e6);
}

const char* wake_result_name(uint32_t r)
{
    switch (r) {
    case HK_WAKE_RESULT_SENT: return "signal sent";
    case HK_WAKE_RESULT_UNSUPPORTED: return "unsupported on this board/state";
    case HK_WAKE_RESULT_NOT_PERMITTED: return "console did not enable USB remote wakeup";
    case HK_WAKE_RESULT_ALREADY_AWAKE: return "console already awake";
    default: return "unknown";
    }
}

int do_wake(Bridge& bridge, const std::string& methods_s, int press_ms, double wait_s)
{
    uint8_t methods = 0;
    for (const auto& m : split(lower(methods_s), ',')) {
        if (m == "usb") methods |= HK_WAKE_USB_REMOTE_WAKEUP;
        else if (m == "home") methods |= HK_WAKE_HOME_PRESS;
        else if (m == "power") methods |= HK_WAKE_POWER_BUTTON;
        else if (m == "cec") methods |= HK_WAKE_HDMI_CEC;
        else {
            std::fprintf(stderr, "error: unknown wake method '%s' (usb, home, power, cec)\n", m.c_str());
            return 64;
        }
    }
    const BridgeStatus before = bridge.status();
    std::printf("Console USB before: %s\n", usb_state_name(before.usb_state));
    bridge.request_wake(methods, static_cast<uint16_t>(press_ms));
    std::printf("Wake requested (methods 0x%02x, press %d ms). Waiting up to %.0f s...\n", methods, press_ms, wait_s);
    uint64_t seen_event = before.last_wake_event_us;
    const uint64_t t0 = now_us();
    while (!g_stop && double(now_us() - t0) < wait_s * 1e6) {
        const BridgeStatus st = bridge.status();
        if (st.last_wake_event_us != seen_event) {
            seen_event = st.last_wake_event_us;
            std::printf("  +%.2f s  method 0x%02x: %s\n", double(st.last_wake_event_us - t0) / 1e6,
                        (st.last_wake_event >> 8) & 0xFF, wake_result_name(st.last_wake_event & 0xFF));
        }
        if (st.usb_state == HK_USB_ACTIVE) {
            std::printf("  +%.2f s  console USB active: the console is awake.\n", double(now_us() - t0) / 1e6);
            return 0;
        }
        wait_ms(20);
    }
    std::printf("Console did not report USB active within %.0f s (now: %s).\n", wait_s,
                usb_state_name(bridge.status().usb_state));
    return 1;
}

// ---- interactive -------------------------------------------------------------

struct KeyBind {
    WORD vk;
    const char* key;
    uint32_t button;
};

// Face buttons on I/J/K/L in Nintendo diamond positions (X top, Y left,
// B bottom, A right); left stick WASD; right stick TFGH or numpad 8/4/2/6.
const KeyBind kKeyButtons[] = {
    {'L', "L", HK_BTN_A},          {'K', "K", HK_BTN_B},           {'I', "I", HK_BTN_X},
    {'J', "J", HK_BTN_Y},          {'Q', "Q", HK_BTN_L},           {'E', "E", HK_BTN_ZL},
    {'U', "U", HK_BTN_R},          {'O', "O", HK_BTN_ZR},          {VK_OEM_PLUS, "=", HK_BTN_PLUS},
    {VK_OEM_MINUS, "-", HK_BTN_MINUS}, {VK_HOME, "Home", HK_BTN_HOME}, {'0', "0", HK_BTN_HOME},
    {VK_END, "End", HK_BTN_CAPTURE}, {'9', "9", HK_BTN_CAPTURE},     {'C', "C", HK_BTN_LSTICK},
    {'M', "M", HK_BTN_RSTICK},     {VK_UP, "Up", HK_BTN_DPAD_UP},  {VK_DOWN, "Down", HK_BTN_DPAD_DOWN},
    {VK_LEFT, "Left", HK_BTN_DPAD_LEFT}, {VK_RIGHT, "Right", HK_BTN_DPAD_RIGHT},
};

enum StickDir { kLUp, kLDown, kLLeft, kLRight, kRUp, kRDown, kRLeft, kRRight, kStickDirs };
struct StickBind {
    WORD vk;
    StickDir dir;
};
const StickBind kKeySticks[] = {
    {'W', kLUp},       {'S', kLDown},      {'A', kLLeft},      {'D', kLRight},
    {'T', kRUp},       {'G', kRDown},      {'F', kRLeft},      {'H', kRRight},
    {VK_NUMPAD8, kRUp}, {VK_NUMPAD2, kRDown}, {VK_NUMPAD4, kRLeft}, {VK_NUMPAD6, kRRight},
};

void print_keymap()
{
    std::printf("Key map:\n"
                "  A=L  B=K  X=I  Y=J            (diamond on I/J/K/L)\n"
                "  L=Q  ZL=E  R=U  ZR=O\n"
                "  PLUS='='  MINUS='-'  HOME=Home/0  CAPTURE=End/9\n"
                "  Left stick W/A/S/D (LS click C)   Right stick T/F/G/H or numpad 8/4/2/6 (RS click M)\n"
                "  D-pad: arrow keys     Hold Shift: half stick tilt\n"
                "  Esc or Ctrl+C: quit (neutral state is sent)\n\n");
}

int interactive(Bridge& bridge)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD old_mode = 0;
    if (!GetConsoleMode(in, &old_mode)) {
        std::fprintf(stderr, "interactive mode needs a console (stdin is redirected).\n");
        return 1;
    }
    SetConsoleMode(in, ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT);
    print_keymap();

    uint32_t buttons = 0;
    bool stick[kStickDirs] = {};
    bool shift = false;
    proto::PadState last;
    bool dirty = true;
    uint64_t next_status = 0;
    std::string usb = "?";

    auto build = [&]() {
        proto::PadState p;
        p.buttons = buttons;
        const double mag = shift ? 0.5 : 1.0;
        auto ax = [&](bool neg, bool pos) { return axis(((pos ? 1.0 : 0.0) - (neg ? 1.0 : 0.0)) * mag); };
        p.lx = ax(stick[kLLeft], stick[kLRight]);
        p.ly = ax(stick[kLDown], stick[kLUp]);
        p.rx = ax(stick[kRLeft], stick[kRRight]);
        p.ry = ax(stick[kRDown], stick[kRUp]);
        return p;
    };

    while (!g_stop) {
        const uint64_t now = now_us();
        if (now >= next_status) {
            next_status = now + 250'000;
            const BridgeStatus st = bridge.status();
            usb = !st.connected ? "bridge lost" : st.usb_state == HK_USB_ACTIVE ? "awake"
                                              : st.usb_state == HK_USB_SUSPENDED ? "asleep"
                                                                                 : "detached";
            dirty = true;
        }
        if (dirty) {
            const proto::PadState p = build();
            if (p != last) {
                bridge.publish(p, false);
                last = p;
            }
            std::printf("\r  [%s] buttons %-28s L(%+.1f,%+.1f) R(%+.1f,%+.1f)        ", usb.c_str(),
                        button_names(p.buttons).c_str(), p.lx / 32767.0, p.ly / 32767.0, p.rx / 32767.0,
                        p.ry / 32767.0);
            std::fflush(stdout);
            dirty = false;
        }
        if (WaitForSingleObject(in, 50) != WAIT_OBJECT_0) continue;
        INPUT_RECORD recs[32];
        DWORD n = 0;
        if (!ReadConsoleInputW(in, recs, 32, &n)) break;
        for (DWORD i = 0; i < n; ++i) {
            if (recs[i].EventType == FOCUS_EVENT && !recs[i].Event.FocusEvent.bSetFocus) {
                // Key-up events are lost while unfocused: release everything.
                buttons = 0;
                for (auto& s : stick) s = false;
                dirty = true;
                continue;
            }
            if (recs[i].EventType != KEY_EVENT) continue;
            const KEY_EVENT_RECORD& k = recs[i].Event.KeyEvent;
            shift = (k.dwControlKeyState & SHIFT_PRESSED) != 0;
            dirty = true;
            if (k.wVirtualKeyCode == VK_ESCAPE && k.bKeyDown) g_stop = true;
            for (const auto& b : kKeyButtons)
                if (b.vk == k.wVirtualKeyCode) {
                    if (k.bKeyDown)
                        buttons |= b.button;
                    else
                        buttons &= ~b.button;
                }
            for (const auto& s : kKeySticks)
                if (s.vk == k.wVirtualKeyCode) stick[s.dir] = k.bKeyDown != 0;
        }
    }
    std::printf("\n");
    SetConsoleMode(in, old_mode);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args(argc, argv, {"ms", "gap", "methods", "press-ms", "wait", "port", "baud"}, {"verbose"});
    if (args.has("help") || args.positional().empty()) {
        std::fputs(kHelp, stdout);
        return args.has("help") ? 0 : 64;
    }
    init_console(args.has("verbose"));
    const auto& pos = args.positional();
    const std::string cmd = lower(pos[0]);
    auto usage_error = [](const std::string& msg) {
        std::fprintf(stderr, "error: %s\nRun with --help for usage.\n", msg.c_str());
        return 64;
    };

    // Validate the whole command before touching the bridge.
    struct Step {
        proto::PadState pad;
        double ms;
    };
    std::vector<Step> steps;
    const double gap = args.num("gap", 80);
    if (cmd == "press" || cmd == "hold") {
        if (pos.size() < (cmd == "hold" ? 3u : 2u)) return usage_error(cmd + " needs a button" +
                                                                   (cmd == "hold" ? " and a duration" : ""));
        Step s;
        std::string bad;
        if (!parse_buttons(pos[1], s.pad.buttons, &bad)) return usage_error("unknown button '" + bad + "'");
        s.ms = cmd == "hold" ? 0 : args.num("ms", 100);
        if (cmd == "hold" && (!parse_double(pos[2], s.ms) || s.ms < 0)) return usage_error("bad duration " + pos[2]);
        steps.push_back(s);
    } else if (cmd == "stick") {
        if (pos.size() < 4) return usage_error("stick needs L|R X Y");
        Step s;
        if (!parse_stick(pos[1], pos[2], pos[3], s.pad)) return usage_error("stick expects L|R and X Y in -1..1");
        s.ms = args.num("ms", 300);
        steps.push_back(s);
    } else if (cmd == "seq") {
        if (pos.size() < 2) return usage_error("seq needs a step list");
        for (const auto& item : split(pos[1], ';')) {
            const auto parts = split(item, ',');
            Step s;
            if (parts.size() != 2 || !parse_double(parts[1], s.ms) || s.ms < 0)
                return usage_error("bad step '" + item + "' (expected NAME,MS)");
            const std::string what = parts[0];
            if (lower(what) == "wait") {
                s.ms = -s.ms;  // negative = pause without a release gap
            } else if (what.size() > 2 && (what[1] == ':')) {
                const auto xy = split(what.substr(2), ':');
                if (xy.size() != 2 || !parse_stick(what.substr(0, 1), xy[0], xy[1], s.pad))
                    return usage_error("bad stick step '" + what + "' (expected L:x:y)");
            } else {
                std::string bad;
                if (!parse_buttons(what, s.pad.buttons, &bad)) return usage_error("unknown button '" + bad + "'");
            }
            steps.push_back(s);
        }
        if (steps.empty()) return usage_error("empty sequence");
    } else if (cmd != "wake" && cmd != "status" && cmd != "interactive") {
        return usage_error("unknown command '" + pos[0] + "'");
    }
    if (!args.error().empty()) return usage_error(args.error());

    apply_process_performance_policy();
    set_thread_role(ThreadRole::Input, "hikari-pad");
    Bridge bridge;
    if (!connect_bridge(bridge, args.get("port"), static_cast<uint32_t>(args.num("baud", HK_LINK_DEFAULT_BAUD)), 5.0))
        return 2;
    g_bridge = &bridge;
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    int rc = 0;
    if (cmd == "status") {
        // Telemetry arrives at 1 Hz and the first ping after ~1 s.
        wait_ms(1300);
        print_status(bridge);
    } else if (cmd == "wake") {
        rc = do_wake(bridge, args.get("methods", "usb,home,power"), static_cast<int>(args.num("press-ms", 150)),
                     args.num("wait", 8));
    } else if (cmd == "interactive") {
        rc = interactive(bridge);
    } else {
        const BridgeStatus st = bridge.status();
        if (st.usb_state != HK_USB_ACTIVE)
            std::printf("Note: console USB is %s; input is only seen while it is awake.\n",
                        usb_state_name(st.usb_state));
        proto::PadState neutral;
        for (size_t i = 0; i < steps.size() && !g_stop; ++i) {
            const Step& s = steps[i];
            if (s.ms < 0) {
                wait_ms(-s.ms);
                continue;
            }
            // The bridge re-sends a held state every 40 ms, so one publish
            // holds it for as long as needed.
            bridge.publish(s.pad, false);
            std::printf("  %s%s for %.0f ms\n", button_names(s.pad.buttons).c_str(),
                        (s.pad.lx || s.pad.ly || s.pad.rx || s.pad.ry)
                            ? strf(" sticks L(%+.2f,%+.2f) R(%+.2f,%+.2f)", s.pad.lx / 32767.0, s.pad.ly / 32767.0,
                                   s.pad.rx / 32767.0, s.pad.ry / 32767.0)
                                  .c_str()
                            : "",
                        s.ms);
            wait_ms(s.ms);
            bridge.publish(neutral, false);
            if (i + 1 < steps.size()) wait_ms(gap);
        }
        flush_bridge(bridge);
    }

    bridge.publish_neutral();
    flush_bridge(bridge);
    g_bridge = nullptr;
    bridge.stop();
    return rc;
}

#pragma once
// Helpers shared by the tools that drive the ESP32 controller bridge.

#include <cstdio>
#include <string>

#include "bridge.h"
#include "hikari/clock.h"
#include "tool_common.h"

namespace hikari::tool {

struct ButtonName {
    const char* name;
    uint32_t bit;
};

inline constexpr ButtonName kButtons[] = {
    {"A", HK_BTN_A},          {"B", HK_BTN_B},         {"X", HK_BTN_X},           {"Y", HK_BTN_Y},
    {"L", HK_BTN_L},          {"R", HK_BTN_R},         {"ZL", HK_BTN_ZL},         {"ZR", HK_BTN_ZR},
    {"PLUS", HK_BTN_PLUS},    {"MINUS", HK_BTN_MINUS}, {"HOME", HK_BTN_HOME},     {"CAPTURE", HK_BTN_CAPTURE},
    {"LS", HK_BTN_LSTICK},    {"RS", HK_BTN_RSTICK},   {"UP", HK_BTN_DPAD_UP},    {"DOWN", HK_BTN_DPAD_DOWN},
    {"LEFT", HK_BTN_DPAD_LEFT}, {"RIGHT", HK_BTN_DPAD_RIGHT},
};

// "A", "a", "A+B", "ZL+ZR" -> mask. Returns false on an unknown name.
inline bool parse_buttons(const std::string& text, uint32_t& mask, std::string* bad = nullptr)
{
    mask = 0;
    for (const auto& part : split(text, '+')) {
        const std::string u = upper(part);
        bool found = false;
        for (const auto& b : kButtons)
            if (u == b.name) {
                mask |= b.bit;
                found = true;
            }
        if (!found) {
            if (bad) *bad = part;
            return false;
        }
    }
    return mask != 0;
}

inline std::string button_names(uint32_t mask)
{
    std::string s;
    for (const auto& b : kButtons)
        if (mask & b.bit) s += (s.empty() ? "" : "+") + std::string(b.name);
    return s.empty() ? "-" : s;
}

inline const char* usb_state_name(uint8_t s)
{
    return s == HK_USB_ACTIVE ? "active (console awake)" : s == HK_USB_SUSPENDED ? "suspended (console asleep)"
                                                                                 : "detached (no console / off)";
}

inline std::string lat4(const hk_lat4& l)
{
    return strf("%.2f/%.2f/%.2f/%.2f ms", l.p50 / 1e3, l.p95 / 1e3, l.p99 / 1e3, l.max / 1e3);
}

// Starts the bridge and waits for the HELLO handshake. On failure prints what
// serial ports exist so the user can tell "not plugged in" from "wrong board".
inline bool connect_bridge(Bridge& bridge, const std::string& port, uint32_t baud, double timeout_s)
{
    bridge.start(port, baud);
    const uint64_t deadline = now_us() + static_cast<uint64_t>(timeout_s * 1e6);
    while (now_us() < deadline) {
        if (bridge.connected()) return true;
        Sleep(20);
    }
    if (bridge.connected()) return true;
    std::fprintf(stderr, "No Hikari controller bridge answered within %.0f s%s.\n", timeout_s,
                 port.empty() ? "" : (" (tried " + port + " first)").c_str());
    const auto ports = enumerate_serial_ports();
    if (ports.empty()) {
        std::fprintf(stderr, "No serial (COM) ports are present. Plug the ESP32-S3 in with a data-capable USB cable;\n"
                             "on a DevKitC use the UART (CP210x/CH343) port, not the native USB port.\n");
    } else {
        std::fprintf(stderr, "Serial ports seen:\n");
        for (const auto& p : ports)
            std::fprintf(stderr, "  %-6s VID %04X PID %04X  %s%s\n", p.port.c_str(), p.vid, p.pid, p.name.c_str(),
                         p.score > 0 ? "  (looks like an ESP32 bridge: no HELLO reply - firmware flashed? "
                                       "port busy in another app?)"
                                     : "");
        std::fprintf(stderr, "Use --port COMx to force a port, --baud to change the link speed.\n");
    }
    return false;
}

// Gives the bridge thread time to put the last published state on the wire
// (publish() only fills a mailbox) before the caller stops it.
inline void flush_bridge(Bridge& bridge, int timeout_ms = 200)
{
    const uint64_t sent = bridge.status().pad_frames_sent;
    const uint64_t deadline = now_us() + uint64_t(timeout_ms) * 1000;
    while (bridge.connected() && now_us() < deadline && bridge.status().pad_frames_sent == sent) Sleep(2);
    Sleep(5);
}

}  // namespace hikari::tool

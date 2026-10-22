#include "input_mapping.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "hk_link.h"

namespace hikari {

namespace {

constexpr size_t kPadCount = static_cast<size_t>(PhysicalInput::PadCount);

constexpr size_t idx(SwitchInput s) { return static_cast<size_t>(s); }
constexpr PhysicalInput pin(int i) { return static_cast<PhysicalInput>(i); }

// Win32 VK codes used below (spelled out so this file needs no <windows.h>).
constexpr unsigned kVkShift = 0x10, kVkControl = 0x11, kVkMenu = 0x12;
constexpr unsigned kVkHome = 0x24, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28;
constexpr unsigned kVkF12 = 0x7B, kVkOemPlus = 0xBB, kVkOemMinus = 0xBD;

struct PadName {
    const char* token;    // stable, written to config
    const char* display;  // shown in the UI
};

// Indexed by PhysicalInput (None .. PadCount-1).
constexpr PadName kPadNames[kPadCount] = {
    {"none", "None"},
    {"pad:south", "South (A)"},
    {"pad:east", "East (B)"},
    {"pad:west", "West (X)"},
    {"pad:north", "North (Y)"},
    {"pad:lb", "LB"},
    {"pad:rb", "RB"},
    {"pad:lt", "LT"},
    {"pad:rt", "RT"},
    {"pad:back", "Back (View)"},
    {"pad:start", "Start (Menu)"},
    {"pad:guide", "Guide"},
    {"pad:share", "Share"},
    {"pad:ls", "LS Click"},
    {"pad:rs", "RS Click"},
    {"pad:dpad_up", "D-Pad Up"},
    {"pad:dpad_down", "D-Pad Down"},
    {"pad:dpad_left", "D-Pad Left"},
    {"pad:dpad_right", "D-Pad Right"},
    {"pad:lstick_up", "LS Up"},
    {"pad:lstick_down", "LS Down"},
    {"pad:lstick_left", "LS Left"},
    {"pad:lstick_right", "LS Right"},
    {"pad:rstick_up", "RS Up"},
    {"pad:rstick_down", "RS Down"},
    {"pad:rstick_left", "RS Left"},
    {"pad:rstick_right", "RS Right"},
};

struct SwitchInfo {
    const char* display;
    const char* key;
    uint32_t bit;  // HK_BTN_*, 0 for stick directions
};

constexpr SwitchInfo kSwitch[kSwitchInputCount] = {
    {"A", "a", HK_BTN_A},
    {"B", "b", HK_BTN_B},
    {"X", "x", HK_BTN_X},
    {"Y", "y", HK_BTN_Y},
    {"L", "l", HK_BTN_L},
    {"R", "r", HK_BTN_R},
    {"ZL", "zl", HK_BTN_ZL},
    {"ZR", "zr", HK_BTN_ZR},
    {"Plus", "plus", HK_BTN_PLUS},
    {"Minus", "minus", HK_BTN_MINUS},
    {"Home", "home", HK_BTN_HOME},
    {"Capture", "capture", HK_BTN_CAPTURE},
    {"L Stick Click", "lstick", HK_BTN_LSTICK},
    {"R Stick Click", "rstick", HK_BTN_RSTICK},
    {"D-Pad Up", "dpad_up", HK_BTN_DPAD_UP},
    {"D-Pad Down", "dpad_down", HK_BTN_DPAD_DOWN},
    {"D-Pad Left", "dpad_left", HK_BTN_DPAD_LEFT},
    {"D-Pad Right", "dpad_right", HK_BTN_DPAD_RIGHT},
    {"L Stick Up", "lstick_up", 0},
    {"L Stick Down", "lstick_down", 0},
    {"L Stick Left", "lstick_left", 0},
    {"L Stick Right", "lstick_right", 0},
    {"R Stick Up", "rstick_up", 0},
    {"R Stick Down", "rstick_down", 0},
    {"R Stick Left", "rstick_left", 0},
    {"R Stick Right", "rstick_right", 0},
};

const char* vk_name(unsigned vk)
{
    static const char* const kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static const char* const kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                           "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char* const kNum[] = {"Num 0", "Num 1", "Num 2", "Num 3", "Num 4",
                                       "Num 5", "Num 6", "Num 7", "Num 8", "Num 9"};
    static const char* const kF[] = {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",
                                     "F9",  "F10", "F11", "F12", "F13", "F14", "F15", "F16",
                                     "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
    if (vk >= '0' && vk <= '9') return kDigits[vk - '0'];
    if (vk >= 'A' && vk <= 'Z') return kLetters[vk - 'A'];
    if (vk >= 0x60 && vk <= 0x69) return kNum[vk - 0x60];
    if (vk >= 0x70 && vk <= 0x87) return kF[vk - 0x70];
    switch (vk) {
    case 0x01: return "Mouse Left";
    case 0x02: return "Mouse Right";
    case 0x04: return "Mouse Middle";
    case 0x05: return "Mouse X1";
    case 0x06: return "Mouse X2";
    case 0x08: return "Backspace";
    case kVkWheelUp: return "Wheel Up";
    case kVkWheelDown: return "Wheel Down";
    case 0x09: return "Tab";
    case 0x0D: return "Enter";
    case 0x10: return "Shift";
    case 0x11: return "Ctrl";
    case 0x12: return "Alt";
    case 0x13: return "Pause";
    case 0x14: return "Caps Lock";
    case 0x1B: return "Esc";
    case 0x20: return "Space";
    case 0x21: return "Page Up";
    case 0x22: return "Page Down";
    case 0x23: return "End";
    case 0x24: return "Home";
    case 0x25: return "Left";
    case 0x26: return "Up";
    case 0x27: return "Right";
    case 0x28: return "Down";
    case 0x2C: return "Print Screen";
    case 0x2D: return "Insert";
    case 0x2E: return "Delete";
    case 0x5B: return "Left Win";
    case 0x5C: return "Right Win";
    case 0x5D: return "Menu";
    case 0x6A: return "Num *";
    case 0x6B: return "Num +";
    case 0x6D: return "Num -";
    case 0x6E: return "Num .";
    case 0x6F: return "Num /";
    case 0x90: return "Num Lock";
    case 0x91: return "Scroll Lock";
    case 0xA0: return "Left Shift";
    case 0xA1: return "Right Shift";
    case 0xA2: return "Left Ctrl";
    case 0xA3: return "Right Ctrl";
    case 0xA4: return "Left Alt";
    case 0xA5: return "Right Alt";
    case 0xBA: return ";";
    case 0xBB: return "=";
    case 0xBC: return ",";
    case 0xBD: return "-";
    case 0xBE: return ".";
    case 0xBF: return "/";
    case 0xC0: return "`";
    case 0xDB: return "[";
    case 0xDC: return "\\";
    case 0xDD: return "]";
    case 0xDE: return "'";
    case 0xE2: return "OEM 102";
    default: return nullptr;
    }
}

bool iequals(const std::string& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && b[i] == 0;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Generic modifiers are what WM_KEYDOWN reports; the input thread may fill
// either form, so a generic binding matches both sides.
bool key_down(const KeyboardState& kb, unsigned vk)
{
    if (kb.down.test(vk)) return true;
    switch (vk) {
    case kVkShift: return kb.down.test(0xA0) || kb.down.test(0xA1);
    case kVkControl: return kb.down.test(0xA2) || kb.down.test(0xA3);
    case kVkMenu: return kb.down.test(0xA4) || kb.down.test(0xA5);
    default: return false;
    }
}

constexpr int16_t kHalf = 16384;

bool pad_down(const RawPadState& p, PhysicalInput in)
{
    const auto i = static_cast<size_t>(in);
    if (i == 0 || i >= kPadCount) return false;
    if (p.pressed.test(i)) return true;
    // Derive from analog values too, so a source that only fills axes works.
    switch (in) {
    case PhysicalInput::PadLT: return p.lt > 64;
    case PhysicalInput::PadRT: return p.rt > 64;
    case PhysicalInput::PadLStickUp: return p.ly > kHalf;
    case PhysicalInput::PadLStickDown: return p.ly < -kHalf;
    case PhysicalInput::PadLStickLeft: return p.lx < -kHalf;
    case PhysicalInput::PadLStickRight: return p.lx > kHalf;
    case PhysicalInput::PadRStickUp: return p.ry > kHalf;
    case PhysicalInput::PadRStickDown: return p.ry < -kHalf;
    case PhysicalInput::PadRStickLeft: return p.rx < -kHalf;
    case PhysicalInput::PadRStickRight: return p.rx > kHalf;
    default: return false;
    }
}

bool input_down(const RawPadState& pad, const KeyboardState& kb, PhysicalInput in)
{
    if (in == PhysicalInput::None) return false;
    if (is_key(in)) return key_down(kb, key_vk(in));
    return pad.connected && pad_down(pad, in);
}

// Stick-direction bindings for stick s (0 = left, 1 = right) are the identity.
bool stick_identity(const std::array<PhysicalInput, kSwitchInputCount>& b, int s)
{
    const size_t base = s == 0 ? idx(SwitchInput::LStickUp) : idx(SwitchInput::RStickUp);
    const int phys = s == 0 ? static_cast<int>(PhysicalInput::PadLStickUp) : static_cast<int>(PhysicalInput::PadRStickUp);
    for (int d = 0; d < 4; ++d)
        if (b[base + static_cast<size_t>(d)] != pin(phys + d)) return false;
    return true;
}

void apply_deadzone(int16_t& x, int16_t& y, float dz)
{
    if (dz <= 0.0f) return;
    const float fx = x / 32767.0f, fy = y / 32767.0f;
    const float mag = std::sqrt(fx * fx + fy * fy);
    if (mag <= dz) {
        x = y = 0;
        return;
    }
    // Rescale so the output starts at 0 just outside the deadzone instead of jumping.
    const float scale = std::min(1.0f, (mag - dz) / (1.0f - dz)) / mag;
    x = static_cast<int16_t>(std::clamp(fx * scale * 32767.0f, -32767.0f, 32767.0f));
    y = static_cast<int16_t>(std::clamp(fy * scale * 32767.0f, -32767.0f, 32767.0f));
}

std::array<PhysicalInput, kSwitchInputCount> pad_common()
{
    std::array<PhysicalInput, kSwitchInputCount> b{};
    b[idx(SwitchInput::L)] = PhysicalInput::PadLB;
    b[idx(SwitchInput::R)] = PhysicalInput::PadRB;
    b[idx(SwitchInput::ZL)] = PhysicalInput::PadLT;
    b[idx(SwitchInput::ZR)] = PhysicalInput::PadRT;
    b[idx(SwitchInput::Minus)] = PhysicalInput::PadBack;
    b[idx(SwitchInput::Plus)] = PhysicalInput::PadStart;
    b[idx(SwitchInput::Home)] = PhysicalInput::PadGuide;
    b[idx(SwitchInput::Capture)] = PhysicalInput::PadShare;
    b[idx(SwitchInput::LStickClick)] = PhysicalInput::PadLS;
    b[idx(SwitchInput::RStickClick)] = PhysicalInput::PadRS;
    for (int d = 0; d < 4; ++d) {
        b[idx(SwitchInput::DpadUp) + static_cast<size_t>(d)] = pin(static_cast<int>(PhysicalInput::PadDpadUp) + d);
        b[idx(SwitchInput::LStickUp) + static_cast<size_t>(d)] = pin(static_cast<int>(PhysicalInput::PadLStickUp) + d);
        b[idx(SwitchInput::RStickUp) + static_cast<size_t>(d)] = pin(static_cast<int>(PhysicalInput::PadRStickUp) + d);
    }
    return b;
}

}  // namespace

// Keyboard defaults (both layouts), modelled on the common emulator scheme so
// players coming from Ryujinx/yuzu feel at home:
//   Left stick  W/A/S/D, click F        Right stick I/J/K/L, click H
//   D-pad       arrow keys              A/B/X/Y     Z/X/C/V
//   L/R         E/U                     ZL/ZR       Q/O
//   Plus/Minus  = / -                   Home        Home     Capture  F12
// Esc, Delete and Backspace are deliberately unbound: the settings window uses
// them to cancel/clear a rebind.
std::array<PhysicalInput, kSwitchInputCount> default_keyboard_bindings()
{
    std::array<PhysicalInput, kSwitchInputCount> b{};
    auto set = [&](SwitchInput s, unsigned vk) { b[idx(s)] = key_input(vk); };
    set(SwitchInput::LStickUp, 'W');
    set(SwitchInput::LStickLeft, 'A');
    set(SwitchInput::LStickDown, 'S');
    set(SwitchInput::LStickRight, 'D');
    set(SwitchInput::LStickClick, 'F');
    set(SwitchInput::RStickUp, 'I');
    set(SwitchInput::RStickLeft, 'J');
    set(SwitchInput::RStickDown, 'K');
    set(SwitchInput::RStickRight, 'L');
    set(SwitchInput::RStickClick, 'H');
    set(SwitchInput::DpadUp, kVkUp);
    set(SwitchInput::DpadDown, kVkDown);
    set(SwitchInput::DpadLeft, kVkLeft);
    set(SwitchInput::DpadRight, kVkRight);
    set(SwitchInput::A, 'Z');
    set(SwitchInput::B, 'X');
    set(SwitchInput::X, 'C');
    set(SwitchInput::Y, 'V');
    set(SwitchInput::L, 'E');
    set(SwitchInput::R, 'U');
    set(SwitchInput::ZL, 'Q');
    set(SwitchInput::ZR, 'O');
    set(SwitchInput::Plus, kVkOemPlus);
    set(SwitchInput::Minus, kVkOemMinus);
    set(SwitchInput::Home, kVkHome);
    set(SwitchInput::Capture, kVkF12);
    return b;
}

InputMapping InputMapping::defaults_positional()
{
    InputMapping m;
    m.pad = pad_common();
    m.pad[idx(SwitchInput::B)] = PhysicalInput::PadSouth;
    m.pad[idx(SwitchInput::A)] = PhysicalInput::PadEast;
    m.pad[idx(SwitchInput::Y)] = PhysicalInput::PadWest;
    m.pad[idx(SwitchInput::X)] = PhysicalInput::PadNorth;
    m.keyboard = default_keyboard_bindings();
    return m;
}

InputMapping InputMapping::defaults_by_label()
{
    InputMapping m;
    m.pad = pad_common();
    m.pad[idx(SwitchInput::A)] = PhysicalInput::PadSouth;
    m.pad[idx(SwitchInput::B)] = PhysicalInput::PadEast;
    m.pad[idx(SwitchInput::X)] = PhysicalInput::PadWest;
    m.pad[idx(SwitchInput::Y)] = PhysicalInput::PadNorth;
    m.keyboard = default_keyboard_bindings();
    return m;
}

proto::PadState InputMapping::apply(const RawPadState& raw, const KeyboardState& kb) const
{
    proto::PadState out;
    // Physical sticks pass through only while their direction bindings are
    // untouched; a remapped stick is composed digitally from its bindings.
    const bool pass[2] = {analog_sticks && raw.connected && stick_identity(pad, 0),
                          analog_sticks && raw.connected && stick_identity(pad, 1)};
    const size_t stick_base[2] = {idx(SwitchInput::LStickUp), idx(SwitchInput::RStickUp)};
    uint8_t dirs[2] = {0, 0};  // bit d = direction d (up, down, left, right) held
    bool kb_zl = false, kb_zr = false;

    for (size_t i = 0; i < kSwitchInputCount; ++i) {
        const int stick = i >= stick_base[1] ? 1 : (i >= stick_base[0] ? 0 : -1);
        bool on = false;
        if (!(stick >= 0 && pass[stick])) on = input_down(raw, kb, pad[i]);
        if (keyboard_enabled && input_down(raw, kb, keyboard[i])) {
            on = true;
            if (i == idx(SwitchInput::ZL)) kb_zl = true;
            if (i == idx(SwitchInput::ZR)) kb_zr = true;
        }
        if (!on) continue;
        if (stick >= 0)
            dirs[stick] |= static_cast<uint8_t>(1u << (i - stick_base[stick]));
        else
            out.buttons |= kSwitch[i].bit;
    }

    const float dz = std::clamp(stick_deadzone, 0.0f, 0.3f);
    const int16_t axes_in[2][2] = {{raw.lx, raw.ly}, {raw.rx, raw.ry}};
    int16_t* axes_out[2][2] = {{&out.lx, &out.ly}, {&out.rx, &out.ry}};
    for (int s = 0; s < 2; ++s) {
        const int dy = ((dirs[s] & 1) ? 1 : 0) - ((dirs[s] & 2) ? 1 : 0);
        const int dx = ((dirs[s] & 8) ? 1 : 0) - ((dirs[s] & 4) ? 1 : 0);
        if (dx != 0 || dy != 0) {
            // Digital directions: full deflection, diagonals on the unit circle.
            const int16_t full = (dx != 0 && dy != 0) ? 23170 : 32767;
            *axes_out[s][0] = static_cast<int16_t>(dx * full);
            *axes_out[s][1] = static_cast<int16_t>(dy * full);
        } else if (pass[s]) {
            int16_t x = axes_in[s][0], y = axes_in[s][1];
            apply_deadzone(x, y, dz);
            *axes_out[s][0] = x;
            *axes_out[s][1] = y;
        }
    }

    // Analog triggers: forward the raw value of whichever trigger ZL/ZR is bound to.
    bool analog_zl = false, analog_zr = false;
    if (analog_triggers && raw.connected) {
        const PhysicalInput zl = pad[idx(SwitchInput::ZL)], zr = pad[idx(SwitchInput::ZR)];
        if (zl == PhysicalInput::PadLT || zl == PhysicalInput::PadRT) {
            out.lt = zl == PhysicalInput::PadLT ? raw.lt : raw.rt;
            analog_zl = true;
        }
        if (zr == PhysicalInput::PadLT || zr == PhysicalInput::PadRT) {
            out.rt = zr == PhysicalInput::PadLT ? raw.lt : raw.rt;
            analog_zr = true;
        }
    }
    if ((out.buttons & HK_BTN_ZL) && (!analog_zl || kb_zl)) out.lt = 255;
    if ((out.buttons & HK_BTN_ZR) && (!analog_zr || kb_zr)) out.rt = 255;
    return out;
}

bool MouseAim::step(uint64_t dt_us, const InputMapping& m, int16_t& x, int16_t& y)
{
    // Mouse speed (counts/s) that tilts the stick fully at sensitivity 1:
    // a brisk flick on an 800 DPI mouse.
    constexpr float kFullSpeed = 2500.0f;
    // Small movements start past the console's own stick deadzone, so slow,
    // precise aiming still turns the camera.
    constexpr float kMinTilt = 0.12f;
    constexpr float kSmoothS = 0.010f;  // evens out 125..1000 Hz mouse reports

    const float dt = std::clamp(static_cast<float>(dt_us) / 1e6f, 0.0005f, 0.05f);
    const float a = 1.0f - std::exp(-dt / kSmoothS);
    vx_ += (static_cast<float>(acc_x_) / dt - vx_) * a;
    vy_ += (static_cast<float>(acc_y_) / dt - vy_) * a;
    acc_x_ = acc_y_ = 0;

    const float full = kFullSpeed / std::clamp(m.mouse_sensitivity, 0.2f, 3.0f);
    const float fx = vx_ / full;
    const float fy = (m.mouse_invert_y ? vy_ : -vy_) / full;  // mouse +Y is down, stick +Y is up
    const float mag = std::sqrt(fx * fx + fy * fy);
    if (mag < 0.01f) {
        vx_ = vy_ = 0;
        return false;
    }
    const float scale = (kMinTilt + (1.0f - kMinTilt) * std::min(1.0f, mag)) / mag;
    x = static_cast<int16_t>(std::clamp(fx * scale * 32767.0f, -32767.0f, 32767.0f));
    y = static_cast<int16_t>(std::clamp(fy * scale * 32767.0f, -32767.0f, 32767.0f));
    return true;
}

void InputMapping::load(const Config& cfg, const std::string& section)
{
    if (section.rfind("mapping.", 0) == 0) profile = section.substr(8);
    for (size_t i = 0; i < kSwitchInputCount; ++i) {
        const std::string key = kSwitch[i].key;
        const std::string pv = cfg.get(section, "pad." + key);
        if (!pv.empty()) pad[i] = physical_from_string(pv);
        const std::string kv = cfg.get(section, "kb." + key);
        if (!kv.empty()) keyboard[i] = physical_from_string(kv);
    }
    analog_sticks = cfg.get_bool(section, "analog_sticks", analog_sticks);
    analog_triggers = cfg.get_bool(section, "analog_triggers", analog_triggers);
    keyboard_enabled = cfg.get_bool(section, "keyboard_enabled", keyboard_enabled);
    mouse_aim = cfg.get_bool(section, "mouse_aim", mouse_aim);
    mouse_invert_y = cfg.get_bool(section, "mouse_invert_y", mouse_invert_y);
    mouse_sensitivity =
        std::clamp(static_cast<float>(cfg.get_double(section, "mouse_sensitivity", mouse_sensitivity)), 0.2f, 3.0f);
    stick_deadzone = std::clamp(static_cast<float>(cfg.get_double(section, "stick_deadzone", stick_deadzone)), 0.0f, 0.3f);
}

void InputMapping::save(Config& cfg, const std::string& section) const
{
    for (size_t i = 0; i < kSwitchInputCount; ++i) {
        const std::string key = kSwitch[i].key;
        cfg.set(section, "pad." + key, to_token(pad[i]));
        cfg.set(section, "kb." + key, to_token(keyboard[i]));
    }
    cfg.set_bool(section, "analog_sticks", analog_sticks);
    cfg.set_bool(section, "analog_triggers", analog_triggers);
    cfg.set_bool(section, "keyboard_enabled", keyboard_enabled);
    cfg.set_bool(section, "mouse_aim", mouse_aim);
    cfg.set_bool(section, "mouse_invert_y", mouse_invert_y);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(stick_deadzone));
    cfg.set(section, "stick_deadzone", buf);
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(mouse_sensitivity));
    cfg.set(section, "mouse_sensitivity", buf);
}

const char* to_string(SwitchInput s)
{
    const size_t i = idx(s);
    return i < kSwitchInputCount ? kSwitch[i].display : "?";
}

const char* config_key(SwitchInput s)
{
    const size_t i = idx(s);
    return i < kSwitchInputCount ? kSwitch[i].key : "?";
}

std::string to_string(PhysicalInput p)
{
    if (is_key(p)) {
        const unsigned vk = key_vk(p);
        if (const char* n = vk_name(vk)) return std::string("Key ") + n;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
        return buf;
    }
    const auto i = static_cast<size_t>(p);
    return i < kPadCount ? kPadNames[i].display : "None";
}

std::string to_token(PhysicalInput p)
{
    if (is_key(p)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "key:0x%02X", key_vk(p));
        return buf;
    }
    const auto i = static_cast<size_t>(p);
    return i < kPadCount ? kPadNames[i].token : "none";
}

PhysicalInput physical_from_string(const std::string& in)
{
    const std::string s = trim(in);
    if (s.empty()) return PhysicalInput::None;
    if (s.size() > 4 && iequals(s.substr(0, 4), "key:")) {
        char* end = nullptr;
        const unsigned long vk = std::strtoul(s.c_str() + 4, &end, 0);
        if (end && *end == 0 && vk >= 1 && vk <= 0xFE) return key_input(static_cast<unsigned>(vk));
        return PhysicalInput::None;
    }
    for (size_t i = 0; i < kPadCount; ++i)
        if (iequals(s, kPadNames[i].token) || iequals(s, kPadNames[i].display)) return pin(static_cast<int>(i));
    // Display names ("Key W"); only reached by hand-edited files or the UI.
    for (unsigned vk = 1; vk <= 0xFE; ++vk)
        if (iequals(s, to_string(key_input(vk)).c_str())) return key_input(vk);
    return PhysicalInput::None;
}

std::vector<std::string> list_mapping_profiles(const Config& cfg)
{
    std::vector<std::string> out;
    for (const auto& s : cfg.sections_with_prefix("mapping."))
        if (s.size() > 8) out.push_back(s.substr(8));
    return out;
}

bool mapping_equal(const InputMapping& a, const InputMapping& b)
{
    return a.profile == b.profile && a.pad == b.pad && a.keyboard == b.keyboard && a.analog_sticks == b.analog_sticks &&
           a.analog_triggers == b.analog_triggers && a.keyboard_enabled == b.keyboard_enabled &&
           a.mouse_aim == b.mouse_aim && a.mouse_invert_y == b.mouse_invert_y &&
           std::fabs(a.mouse_sensitivity - b.mouse_sensitivity) < 0.005f &&
           std::fabs(a.stick_deadzone - b.stick_deadzone) < 0.0005f;
}

}  // namespace hikari

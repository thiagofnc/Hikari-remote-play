#pragma once
// Controller mapping: PC-side physical inputs -> Switch controller inputs.
//
// The client's input thread produces a source-agnostic RawPadState (from
// XInput, Windows.Gaming.Input or raw HID) plus the keyboard state, and
// InputMapping::apply() turns them into the network PadState. Mappings are
// edited in the Controller Settings window and persisted per profile.

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

#include "hikari/config.h"
#include "hikari/net_protocol.h"

namespace hikari {

// Physical inputs. Gamepad inputs use *positional* Xbox naming (South = the
// bottom face button on any pad: Xbox A, PlayStation Cross, Switch B).
enum class PhysicalInput : uint16_t {
    None = 0,
    PadSouth,
    PadEast,
    PadWest,
    PadNorth,
    PadLB,
    PadRB,
    PadLT,  // analog trigger, treated as pressed past 25%
    PadRT,
    PadBack,   // View / Share(PS4) / Create(PS5) / Minus
    PadStart,  // Menu / Options / Plus
    PadGuide,  // Xbox / PS / Home
    PadShare,  // Xbox Series Share / PS touchpad click / Capture
    PadLS,     // left stick click
    PadRS,
    PadDpadUp,
    PadDpadDown,
    PadDpadLeft,
    PadDpadRight,
    PadLStickUp,
    PadLStickDown,
    PadLStickLeft,
    PadLStickRight,
    PadRStickUp,
    PadRStickDown,
    PadRStickLeft,
    PadRStickRight,
    PadCount,
    // Keyboard: KeyBase + Win32 virtual-key code (0x01..0xFE).
    KeyBase = 0x100,
};

inline PhysicalInput key_input(unsigned vk) { return static_cast<PhysicalInput>(0x100 + (vk & 0xFF)); }
inline bool is_key(PhysicalInput p) { return static_cast<uint16_t>(p) >= 0x100; }
inline unsigned key_vk(PhysicalInput p) { return static_cast<uint16_t>(p) - 0x100; }
// Mouse buttons bind as their own VK codes (VK_LBUTTON ...). Wheel notches use
// two codes Windows leaves reserved, so they bind like keys too.
constexpr unsigned kVkWheelUp = 0x0A, kVkWheelDown = 0x0B;

// Switch-side inputs that can be bound.
enum class SwitchInput : uint8_t {
    A, B, X, Y,
    L, R, ZL, ZR,
    Plus, Minus, Home, Capture,
    LStickClick, RStickClick,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    LStickUp, LStickDown, LStickLeft, LStickRight,
    RStickUp, RStickDown, RStickLeft, RStickRight,
    Count
};
constexpr size_t kSwitchInputCount = static_cast<size_t>(SwitchInput::Count);

// What the input thread sees from the active physical controller.
struct RawPadState {
    bool connected = false;
    std::string device_name;  // e.g. "Xbox Wireless Controller", "DualSense"
    std::bitset<static_cast<size_t>(PhysicalInput::PadCount)> pressed;  // digital inputs (incl. stick directions past 50%)
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;  // +Y = up
    uint8_t lt = 0, rt = 0;
};

// Keys currently held (Win32 VK codes), only while the client window has focus.
struct KeyboardState {
    std::bitset<256> down;
};

struct InputMapping {
    std::string profile = "Default";
    // binding[i] = physical input that drives SwitchInput i (None = unbound).
    std::array<PhysicalInput, kSwitchInputCount> pad{};       // gamepad bindings
    std::array<PhysicalInput, kSwitchInputCount> keyboard{};  // keyboard bindings
    // When true, the physical sticks drive the Switch sticks directly with full
    // analog precision (stick-direction bindings above are then only used for
    // keyboard / remapped buttons).
    bool analog_sticks = true;
    bool analog_triggers = true;  // pass LT/RT through as analog ZL/ZR values
    float stick_deadzone = 0.0f;  // 0..0.3; the console applies its own deadzone
    bool keyboard_enabled = true;  // keyboard and mouse
    // Mouse aim: while the pointer is captured in the stream, mouse motion
    // drives the right stick.
    bool mouse_aim = true;
    float mouse_sensitivity = 1.0f;  // 0.2..3.0
    bool mouse_invert_y = false;

    // Positional layout (Xbox A -> Switch B, Xbox B -> Switch A, ...): buttons
    // stay where Switch muscle memory expects them.
    static InputMapping defaults_positional();
    // Label layout (Xbox A -> Switch A, ...).
    static InputMapping defaults_by_label();

    proto::PadState apply(const RawPadState& pad, const KeyboardState& kb) const;

    void load(const Config& cfg, const std::string& section);
    void save(Config& cfg, const std::string& section) const;
};

// Mouse motion -> right-stick deflection. The mouse reports distance, a stick
// reports turn speed, so counts are turned into a smoothed velocity first.
class MouseAim {
public:
    void add(int dx, int dy)
    {
        acc_x_ += dx;
        acc_y_ += dy;
    }
    // Advances the filter by dt_us. Returns false while the mouse is at rest,
    // so the other right-stick sources keep control.
    bool step(uint64_t dt_us, const InputMapping& m, int16_t& x, int16_t& y);
    void reset() { *this = MouseAim{}; }

private:
    float vx_ = 0, vy_ = 0;  // counts per second
    int acc_x_ = 0, acc_y_ = 0;
};

const char* to_string(SwitchInput s);
std::string to_string(PhysicalInput p);  // "South (A)", "Key W", ...
PhysicalInput physical_from_string(const std::string& s);

// Profiles are stored in client.ini as [mapping.<name>].
std::vector<std::string> list_mapping_profiles(const Config& cfg);

// Stable machine token written to config files: "pad:south", "key:0x57",
// "none". physical_from_string() accepts these and the to_string() display
// names, so both round-trip.
std::string to_token(PhysicalInput p);
// Config key for a Switch input inside a [mapping.<name>] section ("a",
// "zl", "lstick_up", ...). Pad bindings are stored as "pad.<key>",
// keyboard bindings as "kb.<key>".
const char* config_key(SwitchInput s);
// Keyboard defaults shared by both pad layouts (see input_mapping.cpp).
std::array<PhysicalInput, kSwitchInputCount> default_keyboard_bindings();
// True when every binding, option and the profile name match.
bool mapping_equal(const InputMapping& a, const InputMapping& b);

}  // namespace hikari

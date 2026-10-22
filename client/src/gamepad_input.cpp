#include "gamepad_input.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/system.h"
#include "hk_link.h"

namespace hikari {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinput_get = nullptr;
constexpr WORD kGuideButton = 0x0400;  // only reported by XInputGetStateEx (ordinal 100)

void load_xinput()
{
    if (g_xinput_get) return;
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput1_3.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    if (!m) return;
    g_xinput_get = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(100))));
    if (!g_xinput_get)
        g_xinput_get = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
}

using P = PhysicalInput;
inline void setb(RawPadState& s, P p, bool on) { s.pressed.set(static_cast<size_t>(p), on); }

int16_t hid_axis(uint8_t v, bool invert)
{
    int32_t x = (static_cast<int32_t>(v) - 128) * 256;
    if (invert) x = -x - 1;
    return static_cast<int16_t>(std::clamp(x, -32768, 32767));
}

void apply_hat(RawPadState& s, uint8_t hat)
{
    const bool up = hat == 0 || hat == 1 || hat == 7;
    const bool right = hat == 1 || hat == 2 || hat == 3;
    const bool down = hat == 3 || hat == 4 || hat == 5;
    const bool left = hat == 5 || hat == 6 || hat == 7;
    setb(s, P::PadDpadUp, up);
    setb(s, P::PadDpadRight, right);
    setb(s, P::PadDpadDown, down);
    setb(s, P::PadDpadLeft, left);
}

// DualShock 4 (USB 0x01, BT short 0x01, BT full 0x11) and DualSense (USB
// 0x01, BT 0x31, BT simple 0x01) input reports -> RawPadState.
bool parse_sony(const uint8_t* r, size_t n, bool dualsense, RawPadState& s)
{
    if (n < 10) return false;
    size_t o;  // offset of LX
    bool ds_layout;
    if (r[0] == 0x01 && dualsense && n >= 64) {
        o = 1;
        ds_layout = true;  // DualSense USB
    } else if (r[0] == 0x31 && dualsense && n >= 12) {
        o = 2;
        ds_layout = true;  // DualSense BT full
    } else if (r[0] == 0x11 && n >= 12) {
        o = 3;
        ds_layout = false;  // DS4 BT full
    } else if (r[0] == 0x01) {
        o = 1;
        ds_layout = false;  // DS4 USB / BT simple (both pads)
    } else {
        return false;
    }
    if (n < o + 10) return false;
    s.lx = hid_axis(r[o], false);
    s.ly = hid_axis(r[o + 1], true);
    s.rx = hid_axis(r[o + 2], false);
    s.ry = hid_axis(r[o + 3], true);
    uint8_t b1, b2, b3;
    if (ds_layout) {
        s.lt = r[o + 4];
        s.rt = r[o + 5];
        b1 = r[o + 7];
        b2 = r[o + 8];
        b3 = r[o + 9];
    } else {
        b1 = r[o + 4];
        b2 = r[o + 5];
        b3 = r[o + 6];
        s.lt = r[o + 7];
        s.rt = r[o + 8];
    }
    apply_hat(s, b1 & 0x0F);
    setb(s, P::PadWest, b1 & 0x10);   // Square
    setb(s, P::PadSouth, b1 & 0x20);  // Cross
    setb(s, P::PadEast, b1 & 0x40);   // Circle
    setb(s, P::PadNorth, b1 & 0x80);  // Triangle
    setb(s, P::PadLB, b2 & 0x01);
    setb(s, P::PadRB, b2 & 0x02);
    setb(s, P::PadBack, b2 & 0x10);   // Share / Create
    setb(s, P::PadStart, b2 & 0x20);  // Options
    setb(s, P::PadLS, b2 & 0x40);
    setb(s, P::PadRS, b2 & 0x80);
    setb(s, P::PadGuide, b3 & 0x01);  // PS
    setb(s, P::PadShare, b3 & 0x02);  // touchpad click -> Capture
    return true;
}

LRESULT CALLBACK input_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        auto* self = reinterpret_cast<GamepadInput*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) self->on_raw_input(reinterpret_cast<void*>(lp), now_us());
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

GamepadInput::~GamepadInput() { stop(); }

bool GamepadInput::start(Sink sink, NavSink nav)
{
    stop();
    sink_ = std::move(sink);
    nav_ = std::move(nav);
    running_ = true;
    thread_ = std::thread(&GamepadInput::run, this);
    return true;
}

void GamepadInput::stop()
{
    if (!running_.exchange(false)) return;
    if (hwnd_) PostMessageW(static_cast<HWND>(hwnd_), WM_NULL, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void GamepadInput::set_mapping(const InputMapping& m)
{
    std::lock_guard<std::mutex> lk(mu_);
    mapping_ = m;
}

void GamepadInput::set_sending(bool on)
{
    if (sending_.exchange(on) == on) return;
    if (!on) {
        // Leaving the stream (or losing focus): release everything on the console.
        std::lock_guard<std::mutex> lk(mu_);
        resync_nav_ = true;  // buttons held during play are not new menu presses
        proto::Input in;
        in.seq = ++seq_;
        in.client_time_us = now_us();
        last_sent_ = proto::PadState{};
        in.pad = last_sent_;
        if (sink_) sink_(in);
    }
}

void GamepadInput::set_keyboard(const KeyboardState& kb)
{
    std::lock_guard<std::mutex> lk(mu_);
    keyboard_ = kb;
    kb_dirty_ = true;
}

void GamepadInput::pin_device(const std::string& name)
{
    std::lock_guard<std::mutex> lk(mu_);
    pinned_ = name == "Keyboard" ? "" : name;
}

void GamepadInput::mark_next_latency() { mark_next_ = true; }

GamepadInput::Device* GamepadInput::active()
{
    Device* best = nullptr;
    for (auto& d : devices_) {
        if (!d.connected) continue;
        if (!pinned_.empty() && d.name == pinned_) return &d;
        if (!best || d.last_activity_us > best->last_activity_us) best = &d;
    }
    return best;
}

RawPadState GamepadInput::raw()
{
    std::lock_guard<std::mutex> lk(mu_);
    Device* d = active();
    return d ? d->state : RawPadState{};
}

std::vector<std::string> GamepadInput::devices()
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    for (auto& d : devices_)
        if (d.connected) out.push_back(d.name);
    return out;
}

std::string GamepadInput::active_device()
{
    std::lock_guard<std::mutex> lk(mu_);
    Device* d = active();
    return d ? d->name : std::string();
}

bool GamepadInput::any_connected()
{
    std::lock_guard<std::mutex> lk(mu_);
    return active() != nullptr;
}

double GamepadInput::send_rate()
{
    std::lock_guard<std::mutex> lk(mu_);
    return last_rate_;
}

void GamepadInput::classify_sticks(RawPadState& s)
{
    constexpr int kT = 16384;  // 50% deflection counts as a digital direction
    setb(s, P::PadLStickUp, s.ly > kT);
    setb(s, P::PadLStickDown, s.ly < -kT);
    setb(s, P::PadLStickLeft, s.lx < -kT);
    setb(s, P::PadLStickRight, s.lx > kT);
    setb(s, P::PadRStickUp, s.ry > kT);
    setb(s, P::PadRStickDown, s.ry < -kT);
    setb(s, P::PadRStickLeft, s.rx < -kT);
    setb(s, P::PadRStickRight, s.rx > kT);
    setb(s, P::PadLT, s.lt > 64);
    setb(s, P::PadRT, s.rt > 64);
}

void GamepadInput::poll_xinput(uint64_t now, bool full_scan)
{
    if (!g_xinput_get) return;
    for (DWORD slot = 0; slot < 4; ++slot) {
        const std::string id = "xinput:" + std::to_string(slot);
        auto it = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.id == id; });
        const bool known = it != devices_.end() && it->connected;
        // XInputGetState on an empty slot is slow (device enumeration in the
        // driver), so empty slots are only rescanned once a second.
        if (!known && !full_scan) continue;
        XINPUT_STATE xs{};
        const bool ok = g_xinput_get(slot, &xs) == ERROR_SUCCESS;
        if (!ok) {
            if (known) {
                it->connected = false;
                HK_LOGI("Controller disconnected: %s", it->name.c_str());
            }
            continue;
        }
        if (it == devices_.end()) {
            Device d;
            d.id = id;
            d.name = "Xbox Controller " + std::to_string(slot + 1);
            d.xinput_slot = static_cast<int>(slot);
            devices_.push_back(d);
            it = devices_.end() - 1;
        }
        if (!it->connected) {
            it->connected = true;
            it->last_activity_us = now;
            HK_LOGI("Controller connected: %s", it->name.c_str());
        }
        it->last_seen_us = now;
        RawPadState s;
        s.connected = true;
        s.device_name = it->name;
        const WORD b = xs.Gamepad.wButtons;
        setb(s, P::PadSouth, b & XINPUT_GAMEPAD_A);
        setb(s, P::PadEast, b & XINPUT_GAMEPAD_B);
        setb(s, P::PadWest, b & XINPUT_GAMEPAD_X);
        setb(s, P::PadNorth, b & XINPUT_GAMEPAD_Y);
        setb(s, P::PadLB, b & XINPUT_GAMEPAD_LEFT_SHOULDER);
        setb(s, P::PadRB, b & XINPUT_GAMEPAD_RIGHT_SHOULDER);
        setb(s, P::PadBack, b & XINPUT_GAMEPAD_BACK);
        setb(s, P::PadStart, b & XINPUT_GAMEPAD_START);
        setb(s, P::PadGuide, b & kGuideButton);
        setb(s, P::PadLS, b & XINPUT_GAMEPAD_LEFT_THUMB);
        setb(s, P::PadRS, b & XINPUT_GAMEPAD_RIGHT_THUMB);
        setb(s, P::PadDpadUp, b & XINPUT_GAMEPAD_DPAD_UP);
        setb(s, P::PadDpadDown, b & XINPUT_GAMEPAD_DPAD_DOWN);
        setb(s, P::PadDpadLeft, b & XINPUT_GAMEPAD_DPAD_LEFT);
        setb(s, P::PadDpadRight, b & XINPUT_GAMEPAD_DPAD_RIGHT);
        s.lx = xs.Gamepad.sThumbLX;
        s.ly = xs.Gamepad.sThumbLY;
        s.rx = xs.Gamepad.sThumbRX;
        s.ry = xs.Gamepad.sThumbRY;
        s.lt = xs.Gamepad.bLeftTrigger;
        s.rt = xs.Gamepad.bRightTrigger;
        classify_sticks(s);
        const bool changed = s.pressed != it->state.pressed || s.lx != it->state.lx || s.ly != it->state.ly ||
                             s.rx != it->state.rx || s.ry != it->state.ry || s.lt != it->state.lt ||
                             s.rt != it->state.rt;
        // "Activity" = a deliberate input (button or stick past 30%), so a
        // drifting idle stick does not steal focus from the pad in use.
        if (s.pressed.any() || std::abs(s.lx) > 10000 || std::abs(s.ly) > 10000) it->last_activity_us = now;
        if (changed) it->state = s;
    }
}

void GamepadInput::on_raw_input(void* h, uint64_t now)
{
    UINT size = 0;
    GetRawInputData(static_cast<HRAWINPUT>(h), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 4096) return;
    alignas(8) uint8_t buf[4096];
    if (GetRawInputData(static_cast<HRAWINPUT>(h), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) return;
    auto* ri = reinterpret_cast<RAWINPUT*>(buf);
    if (ri->header.dwType != RIM_TYPEHID) return;

    std::lock_guard<std::mutex> lk(mu_);
    void* dev_handle = ri->header.hDevice;
    auto it = std::find_if(devices_.begin(), devices_.end(), [&](const Device& d) { return d.hid == dev_handle; });
    if (it == devices_.end()) {
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT isz = sizeof(info);
        if (GetRawInputDeviceInfoW(static_cast<HANDLE>(dev_handle), RIDI_DEVICEINFO, &info, &isz) == UINT(-1)) return;
        if (info.hid.dwVendorId != 0x054C) return;  // Sony only: everything else comes via XInput
        const DWORD pid = info.hid.dwProductId;
        const bool ds = pid == 0x0CE6 || pid == 0x0DF2;
        const bool ds4 = pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0;
        if (!ds && !ds4) return;
        Device d;
        d.hid = dev_handle;
        d.id = "hid:" + std::to_string(reinterpret_cast<uintptr_t>(dev_handle));
        d.name = ds ? (pid == 0x0DF2 ? "DualSense Edge" : "DualSense") : "DualShock 4";
        d.connected = true;
        d.last_activity_us = now;
        HK_LOGI("Controller connected: %s", d.name.c_str());
        devices_.push_back(d);
        it = devices_.end() - 1;
    }
    it->connected = true;
    it->last_seen_us = now;
    const bool dualsense = it->name.rfind("DualSense", 0) == 0;
    const uint8_t* data = ri->data.hid.bRawData;
    for (DWORD i = 0; i < ri->data.hid.dwCount; ++i) {
        RawPadState s;
        s.connected = true;
        s.device_name = it->name;
        if (!parse_sony(data + i * ri->data.hid.dwSizeHid, ri->data.hid.dwSizeHid, dualsense, s)) continue;
        classify_sticks(s);
        if (s.pressed.any() || std::abs(s.lx) > 10000 || std::abs(s.ly) > 10000) it->last_activity_us = now;
        it->state = s;
    }
    // Event-driven source: produce right away instead of waiting for the tick.
    produce(now, false);
}

void GamepadInput::produce(uint64_t now, bool force)
{
    // Caller holds mu_.
    Device* d = active();
    RawPadState raw = d ? d->state : RawPadState{};
    if (d && d->id != active_id_) {
        active_id_ = d->id;
        HK_LOGI("Active controller: %s", d->name.c_str());
    }
    proto::PadState pad = mapping_.apply(raw, keyboard_);
    pad.buttons |= injected_.load(std::memory_order_relaxed);

    // Holding Minus + Plus for a second opens or closes the stream menu, also while streaming.
    constexpr uint32_t kChord = HK_BTN_MINUS | HK_BTN_PLUS;
    if ((pad.buttons & kChord) != kChord) {
        chord_since_ = 0;
        chord_fired_ = false;
    } else if (!chord_since_) {
        chord_since_ = now;
    } else if (!chord_fired_ && now - chord_since_ >= 1'000'000) {
        chord_fired_ = true;
        if (menu_chord_) menu_chord_();
    }

    if (!sending_) {
        if (resync_nav_) {
            resync_nav_ = false;
            last_nav_ = pad;
        }
        // Lobby: report button-down edges for UI navigation (A = Play, ...).
        if (nav_) {
            static constexpr SwitchInput kNav[] = {SwitchInput::A,        SwitchInput::B,         SwitchInput::Plus,
                                                   SwitchInput::DpadUp,   SwitchInput::DpadDown,  SwitchInput::DpadLeft,
                                                   SwitchInput::DpadRight, SwitchInput::X,        SwitchInput::Y};
            static constexpr uint32_t kBits[] = {HK_BTN_A,         HK_BTN_B,          HK_BTN_PLUS,
                                                 HK_BTN_DPAD_UP,   HK_BTN_DPAD_DOWN,  HK_BTN_DPAD_LEFT,
                                                 HK_BTN_DPAD_RIGHT, HK_BTN_X,         HK_BTN_Y};
            for (size_t i = 0; i < std::size(kNav); ++i)
                if ((pad.buttons & kBits[i]) && !(last_nav_.buttons & kBits[i])) nav_(kNav[i]);
        }
        last_nav_ = pad;
        return;
    }

    const bool changed = pad != last_sent_;
    const bool refresh = now - last_send_us_ >= uint64_t(refresh_ms_.load()) * 1000;
    if (changed || refresh || force) {
        proto::Input in;
        in.seq = ++seq_;
        in.client_time_us = now;
        in.flags = d ? proto::kInputConnected : 0;
        if (mark_next_.exchange(false)) in.flags |= proto::kInputLatencyMark;
        in.pad = pad;
        last_sent_ = pad;
        last_send_us_ = now;
        last_input_ = in;
        redundant_at_ = changed ? now + 2000 : 0;
        rate_.add();
        if (sink_) sink_(in);
    } else if (redundant_at_ && now >= redundant_at_) {
        // Same sequence number: the host ignores it if the original arrived.
        proto::Input dup = last_input_;
        dup.flags |= proto::kInputRedundant;
        redundant_at_ = 0;
        if (sink_) sink_(dup);
    }
}

void GamepadInput::run()
{
    set_thread_role(ThreadRole::Input, "hikari-input");
    load_xinput();

    WNDCLASSW wc{};
    wc.lpfnWndProc = input_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HikariInputSink";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    hwnd_ = hwnd;
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x05;  // gamepad
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1] = rid[0];
    rid[1].usUsage = 0x04;  // joystick (DS4 reports itself as this)
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

    PreciseTimer timer;
    uint64_t next_scan = 0;
    uint64_t next_rate = now_us() + 1'000'000;
    while (running_) {
        timer.arm_relative_us(1000);  // 1 kHz
        HANDLE h = static_cast<HANDLE>(timer.handle());
        MsgWaitForMultipleObjectsEx(1, &h, 5, QS_RAWINPUT | QS_POSTMESSAGE, MWMO_INPUTAVAILABLE);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);

        const uint64_t now = now_us();
        std::lock_guard<std::mutex> lk(mu_);
        const bool full = now >= next_scan;
        if (full) next_scan = now + 1'000'000;
        poll_xinput(now, full);
        for (auto& d : devices_)  // Sony pads report continuously; silence = unplugged
            if (d.hid && d.connected && now - d.last_seen_us > 1'500'000) {
                d.connected = false;
                HK_LOGI("Controller disconnected: %s", d.name.c_str());
            }
        produce(now, kb_dirty_);
        kb_dirty_ = false;
        if (now >= next_rate) {
            last_rate_ = rate_.sample(now);
            next_rate = now + 1'000'000;
        }
    }
    DestroyWindow(hwnd);
    hwnd_ = nullptr;
}

}  // namespace hikari

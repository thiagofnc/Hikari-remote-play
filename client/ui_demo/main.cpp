// hikari_controller_demo: runs the Controller Settings window against a live
// XInput controller, or (with --selftest) checks InputMapping and exits.

#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "controller_settings.h"
#include "hikari/config.h"
#include "hikari/log.h"
#include "hikari/system.h"
#include "hk_link.h"

using namespace hikari;

namespace {

// ---- XInput poller ---------------------------------------------------------------

// XInputGetStateEx (ordinal 100) also reports the Guide button.
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
constexpr WORD kGuide = 0x0400;

class XInputPoller {
public:
    XInputPoller()
    {
        const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const wchar_t* d : dlls) {
            if ((dll_ = LoadLibraryW(d)) != nullptr) break;
        }
        if (dll_) {
            get_ = reinterpret_cast<GetStateFn>(
                reinterpret_cast<void*>(GetProcAddress(dll_, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(100)))));
            if (!get_)
                get_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(dll_, "XInputGetState")));
        }
        thread_ = std::thread([this] { loop(); });
    }
    ~XInputPoller()
    {
        stop_ = true;
        thread_.join();
        if (dll_) FreeLibrary(dll_);
    }

    RawPadState state()
    {
        std::lock_guard<std::mutex> lk(mu_);
        return state_;
    }
    std::vector<std::string> devices()
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        for (int i = 0; i < 4; ++i)
            if (connected_[i]) out.push_back(name(i));
        return out;
    }
    void select(const std::string& n)
    {
        for (int i = 0; i < 4; ++i)
            if (n == name(i)) active_ = i;
    }

private:
    static std::string name(int slot) { return "Xbox Controller (XInput " + std::to_string(slot + 1) + ")"; }

    void loop()
    {
        set_thread_role(ThreadRole::Input, "xinput-poll");
        uint64_t n = 0;
        while (!stop_) {
            // Probing empty slots is slow, so only rescan them once a second.
            if (get_ && n % 500 == 0) {
                for (int i = 0; i < 4; ++i) {
                    XINPUT_STATE s{};
                    const bool c = get_(static_cast<DWORD>(i), &s) == ERROR_SUCCESS;
                    std::lock_guard<std::mutex> lk(mu_);
                    connected_[i] = c;
                }
                if (!connected_[active_.load()])
                    for (int i = 0; i < 4; ++i)
                        if (connected_[i]) {
                            active_ = i;
                            break;
                        }
            }
            RawPadState r;
            XINPUT_STATE s{};
            const int slot = active_;
            if (get_ && connected_[slot] && get_(static_cast<DWORD>(slot), &s) == ERROR_SUCCESS) fill(r, s.Gamepad, slot);
            {
                std::lock_guard<std::mutex> lk(mu_);
                state_ = r;
            }
            ++n;
            Sleep(2);  // ~500 Hz (with 1 ms timer resolution)
        }
    }

    static void fill(RawPadState& r, const XINPUT_GAMEPAD& g, int slot)
    {
        r.connected = true;
        r.device_name = name(slot);
        const auto set = [&](PhysicalInput p, bool on) {
            if (on) r.pressed.set(static_cast<size_t>(p));
        };
        const WORD b = g.wButtons;
        set(PhysicalInput::PadSouth, b & XINPUT_GAMEPAD_A);
        set(PhysicalInput::PadEast, b & XINPUT_GAMEPAD_B);
        set(PhysicalInput::PadWest, b & XINPUT_GAMEPAD_X);
        set(PhysicalInput::PadNorth, b & XINPUT_GAMEPAD_Y);
        set(PhysicalInput::PadLB, b & XINPUT_GAMEPAD_LEFT_SHOULDER);
        set(PhysicalInput::PadRB, b & XINPUT_GAMEPAD_RIGHT_SHOULDER);
        set(PhysicalInput::PadBack, b & XINPUT_GAMEPAD_BACK);
        set(PhysicalInput::PadStart, b & XINPUT_GAMEPAD_START);
        set(PhysicalInput::PadGuide, b & kGuide);
        set(PhysicalInput::PadLS, b & XINPUT_GAMEPAD_LEFT_THUMB);
        set(PhysicalInput::PadRS, b & XINPUT_GAMEPAD_RIGHT_THUMB);
        set(PhysicalInput::PadDpadUp, b & XINPUT_GAMEPAD_DPAD_UP);
        set(PhysicalInput::PadDpadDown, b & XINPUT_GAMEPAD_DPAD_DOWN);
        set(PhysicalInput::PadDpadLeft, b & XINPUT_GAMEPAD_DPAD_LEFT);
        set(PhysicalInput::PadDpadRight, b & XINPUT_GAMEPAD_DPAD_RIGHT);
        r.lx = g.sThumbLX;
        r.ly = g.sThumbLY;
        r.rx = g.sThumbRX;
        r.ry = g.sThumbRY;
        r.lt = g.bLeftTrigger;
        r.rt = g.bRightTrigger;
        set(PhysicalInput::PadLT, r.lt > 64);
        set(PhysicalInput::PadRT, r.rt > 64);
        const int h = 16384;
        set(PhysicalInput::PadLStickUp, r.ly > h);
        set(PhysicalInput::PadLStickDown, r.ly < -h);
        set(PhysicalInput::PadLStickLeft, r.lx < -h);
        set(PhysicalInput::PadLStickRight, r.lx > h);
        set(PhysicalInput::PadRStickUp, r.ry > h);
        set(PhysicalInput::PadRStickDown, r.ry < -h);
        set(PhysicalInput::PadRStickLeft, r.rx < -h);
        set(PhysicalInput::PadRStickRight, r.rx > h);
    }

    HMODULE dll_ = nullptr;
    GetStateFn get_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> active_{0};
    std::mutex mu_;
    bool connected_[4] = {};
    RawPadState state_;
};

// ---- selftest -----------------------------------------------------------------------

int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

RawPadState pad_with(PhysicalInput p)
{
    RawPadState r;
    r.connected = true;
    r.pressed.set(static_cast<size_t>(p));
    return r;
}

int selftest()
{
    const KeyboardState no_keys;
    {  // Layout defaults.
        const InputMapping pos = InputMapping::defaults_positional();
        const InputMapping lab = InputMapping::defaults_by_label();
        CHECK(pos.apply(pad_with(PhysicalInput::PadSouth), no_keys).buttons == HK_BTN_B);
        CHECK(pos.apply(pad_with(PhysicalInput::PadEast), no_keys).buttons == HK_BTN_A);
        CHECK(pos.apply(pad_with(PhysicalInput::PadWest), no_keys).buttons == HK_BTN_Y);
        CHECK(pos.apply(pad_with(PhysicalInput::PadNorth), no_keys).buttons == HK_BTN_X);
        CHECK(lab.apply(pad_with(PhysicalInput::PadSouth), no_keys).buttons == HK_BTN_A);
        CHECK(lab.apply(pad_with(PhysicalInput::PadNorth), no_keys).buttons == HK_BTN_Y);
        CHECK(pos.apply(pad_with(PhysicalInput::PadGuide), no_keys).buttons == HK_BTN_HOME);
        CHECK(pos.apply(pad_with(PhysicalInput::PadShare), no_keys).buttons == HK_BTN_CAPTURE);
        CHECK(pos.apply(pad_with(PhysicalInput::PadBack), no_keys).buttons == HK_BTN_MINUS);
        CHECK(pos.apply(pad_with(PhysicalInput::PadDpadLeft), no_keys).buttons == HK_BTN_DPAD_LEFT);
        RawPadState off = pad_with(PhysicalInput::PadSouth);
        off.connected = false;
        CHECK(pos.apply(off, no_keys).buttons == 0);
    }
    {  // Analog pass-through + deadzone.
        InputMapping m = InputMapping::defaults_positional();
        RawPadState r;
        r.connected = true;
        r.lx = 20000;
        r.ry = -12000;
        proto::PadState o = m.apply(r, no_keys);
        CHECK(o.lx == 20000 && o.ly == 0 && o.ry == -12000);
        m.stick_deadzone = 0.2f;
        r.lx = 5000;  // ~15%: inside the deadzone
        o = m.apply(r, no_keys);
        CHECK(o.lx == 0);
        r.lx = 32767;
        o = m.apply(r, no_keys);
        CHECK(o.lx > 32000);
        r.lx = 16384;  // 50% -> (0.5-0.2)/0.8 = 37.5%
        o = m.apply(r, no_keys);
        CHECK(std::abs(o.lx - 12287) < 200);
        m.analog_sticks = false;
        r.lx = 20000;  // past 50%: digital right
        o = m.apply(r, no_keys);
        CHECK(o.lx == 32767);
    }
    {  // Remapped stick direction disables pass-through and composes digitally.
        InputMapping m = InputMapping::defaults_positional();
        m.pad[static_cast<size_t>(SwitchInput::LStickUp)] = PhysicalInput::PadDpadUp;
        RawPadState r = pad_with(PhysicalInput::PadDpadUp);
        r.lx = 10000;  // ignored: left stick is no longer passed through
        const proto::PadState o = m.apply(r, no_keys);
        CHECK(o.ly == 32767 && o.lx == 0);
        CHECK((o.buttons & HK_BTN_DPAD_UP) != 0);
    }
    {  // Keyboard: diagonals, opposing directions, enable flag.
        InputMapping m = InputMapping::defaults_positional();
        RawPadState none;
        KeyboardState kb;
        kb.down.set('W');
        kb.down.set('D');
        proto::PadState o = m.apply(none, kb);
        CHECK(o.lx == 23170 && o.ly == 23170);
        kb.down.set('S');
        o = m.apply(none, kb);
        CHECK(o.ly == 0 && o.lx == 32767);
        kb.down.reset();
        kb.down.set('Z');
        kb.down.set('Q');
        o = m.apply(none, kb);
        CHECK(o.buttons == (HK_BTN_A | HK_BTN_ZL) && o.lt == 255);
        m.keyboard_enabled = false;
        o = m.apply(none, kb);
        CHECK(o.buttons == 0 && o.lt == 0);
    }
    {  // Analog triggers.
        InputMapping m = InputMapping::defaults_positional();
        RawPadState r;
        r.connected = true;
        r.lt = 100;
        r.rt = 30;
        r.pressed.set(static_cast<size_t>(PhysicalInput::PadLT));
        proto::PadState o = m.apply(r, no_keys);
        CHECK(o.lt == 100 && o.rt == 30 && (o.buttons & HK_BTN_ZL) && !(o.buttons & HK_BTN_ZR));
        m.analog_triggers = false;
        o = m.apply(r, no_keys);
        CHECK(o.lt == 255 && o.rt == 0);
    }
    {  // Keyboard defaults: no duplicates, Esc/Delete/Backspace free.
        const auto kb = default_keyboard_bindings();
        for (size_t i = 0; i < kb.size(); ++i) {
            CHECK(kb[i] != PhysicalInput::None);
            CHECK(kb[i] != key_input(VK_ESCAPE) && kb[i] != key_input(VK_DELETE) && kb[i] != key_input(VK_BACK));
            for (size_t j = i + 1; j < kb.size(); ++j) CHECK(kb[i] != kb[j]);
        }
    }
    {  // String round trips.
        for (int i = 0; i < static_cast<int>(PhysicalInput::PadCount); ++i) {
            const auto p = static_cast<PhysicalInput>(i);
            CHECK(physical_from_string(to_token(p)) == p);
            CHECK(physical_from_string(to_string(p)) == p);
        }
        for (unsigned vk = 1; vk <= 0xFE; ++vk) {
            CHECK(physical_from_string(to_token(key_input(vk))) == key_input(vk));
            CHECK(physical_from_string(to_string(key_input(vk))) == key_input(vk));
        }
        CHECK(physical_from_string("garbage") == PhysicalInput::None);
        CHECK(to_string(key_input('W')) == "Key W");
        CHECK(to_token(PhysicalInput::PadSouth) == "pad:south");
    }
    {  // Config round trip, in memory and through a file.
        InputMapping m = InputMapping::defaults_by_label();
        m.profile = "Zelda";
        m.pad[static_cast<size_t>(SwitchInput::A)] = PhysicalInput::PadRS;
        m.pad[static_cast<size_t>(SwitchInput::Home)] = PhysicalInput::None;
        m.keyboard[static_cast<size_t>(SwitchInput::B)] = key_input(VK_SPACE);
        m.analog_triggers = false;
        m.keyboard_enabled = false;
        m.stick_deadzone = 0.12f;
        Config cfg;
        m.save(cfg, "mapping.Zelda");
        InputMapping::defaults_positional().save(cfg, "mapping.Default");
        InputMapping back = InputMapping::defaults_positional();
        back.load(cfg, "mapping.Zelda");
        CHECK(mapping_equal(m, back));

        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        const std::string path = std::string(tmp) + "hikari_mapping_selftest.ini";
        CHECK(cfg.save_as(path));
        Config cfg2;
        CHECK(cfg2.load(path));
        InputMapping back2 = InputMapping::defaults_positional();
        back2.load(cfg2, "mapping.Zelda");
        CHECK(mapping_equal(m, back2));
        const auto profiles = list_mapping_profiles(cfg2);
        CHECK(profiles.size() == 2);
        std::remove(path.c_str());
    }
    std::printf("selftest: %s (%d failure%s)\n", g_failures ? "FAILED" : "passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0) return selftest();

    log::init("controller_demo");
    timeBeginPeriod(1);  // lets the poller run at ~500 Hz and the preview timer at ~120 Hz

    Config cfg;
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    cfg.load(std::string(tmp) + "hikari_controller_demo.ini");
    const std::string profile = cfg.get("client", "mapping_profile", "Default");
    InputMapping mapping = InputMapping::defaults_positional();
    mapping.load(cfg, "mapping." + profile);
    mapping.profile = profile;

    XInputPoller poller;
    ControllerSettingsDeps deps;
    deps.poll_pad = [&] { return poller.state(); };
    deps.list_devices = [&] { return poller.devices(); };
    deps.select_device = [&](const std::string& n) { poller.select(n); };

    const bool saved = show_controller_settings(nullptr, cfg, mapping, deps);
    std::printf("closed: %s, active profile '%s', settings file %s\n", saved ? "saved" : "not saved",
                mapping.profile.c_str(), cfg.path().c_str());
    timeEndPeriod(1);
    return 0;
}

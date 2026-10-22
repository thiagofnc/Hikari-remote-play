#pragma once
// Controller capture on its own thread, independent of UI and video.
//
// Sources:
//   * XInput (Xbox and anything Steam/DS4Windows exposes as XInput), polled
//     at 1 kHz with a high-resolution timer. The Guide button comes from the
//     XInputGetStateEx export so it can be Switch HOME.
//   * Raw Input HID for Sony DualShock 4 / DualSense (USB + Bluetooth):
//     event driven, so a report is translated and sent the moment it lands.
//   * Keyboard (state pushed from the window thread).
//   * Mouse, via Raw Input while the stream window has captured the pointer:
//     buttons and wheel bind like keys, motion can drive the right stick.
//
// The active device is chosen automatically: whichever controller was used
// most recently drives the console (unless one is pinned in settings).
//
// Sending: a changed state goes out immediately, a redundant copy follows
// 2 ms later (survives a single lost packet without waiting for a refresh),
// and an unchanged state is refreshed every `refresh_ms`.

#include <atomic>
#include <bitset>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hikari/histogram.h"
#include "input_mapping.h"

namespace hikari {

class GamepadInput {
public:
    using Sink = std::function<void(const proto::Input&)>;
    using NavSink = std::function<void(SwitchInput)>;  // button-down edges while not streaming

    ~GamepadInput();

    bool start(Sink sink, NavSink nav);
    void stop();

    void set_mapping(const InputMapping& m);
    void set_sending(bool on);         // streaming and window focused
    void set_refresh_ms(uint32_t ms) { refresh_ms_ = ms; }
    void set_keyboard(const KeyboardState& kb);
    void set_mouse_captured(bool on);  // pointer locked to the stream window
    void pin_device(const std::string& name);  // "" = automatic
    void mark_next_latency();          // tag the next sent state for glass-to-glass tests
    void set_injected(uint32_t buttons) { injected_ = buttons; }  // hotkeys (F8 = HOME, F9 = Capture)

    RawPadState raw();                 // active device (for the settings preview)
    std::vector<std::string> devices();
    std::string active_device();
    bool any_connected();
    double send_rate();

    // Raw Input dispatch from the input thread's message-only window.
    void on_raw_input(void* hrawinput, uint64_t now);

private:
    struct Device {
        std::string id;
        std::string name;
        int xinput_slot = -1;
        void* hid = nullptr;  // HANDLE from raw input
        bool connected = false;
        RawPadState state;
        uint64_t last_activity_us = 0;
        uint64_t last_seen_us = 0;
    };

    void run();
    void on_mouse(const void* rawmouse, uint64_t now);
    void poll_xinput(uint64_t now, bool full_scan);
    void produce(uint64_t now, bool force);
    Device* active();
    static void classify_sticks(RawPadState& s);

    std::thread thread_;
    std::atomic<bool> running_{false};
    void* hwnd_ = nullptr;
    Sink sink_;
    NavSink nav_;

    std::mutex mu_;
    std::vector<Device> devices_;
    std::string pinned_;
    std::string active_id_;
    InputMapping mapping_;
    KeyboardState keyboard_;
    bool kb_dirty_ = false;

    std::atomic<bool> mouse_captured_{false};
    MouseAim mouse_;
    std::bitset<256> mouse_down_;      // mouse buttons and wheel notches, as VK codes
    uint64_t wheel_release_us_[2]{};   // wheel up / down
    bool mouse_aiming_ = false;
    int16_t mouse_rx_ = 0, mouse_ry_ = 0;
    uint64_t last_tick_us_ = 0;

    std::atomic<bool> sending_{false};
    std::atomic<uint32_t> refresh_ms_{16};
    std::atomic<bool> mark_next_{false};
    std::atomic<uint32_t> injected_{0};

    proto::PadState last_sent_;
    proto::PadState last_nav_;
    uint32_t seq_ = 0;
    uint64_t last_send_us_ = 0;
    uint64_t redundant_at_ = 0;
    proto::Input last_input_;
    RateCounter rate_;
    double last_rate_ = 0;
};

}  // namespace hikari

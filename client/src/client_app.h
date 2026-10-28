#pragma once
// The Hikari client window: lobby ("your Switch" + Play) and the stream.
//
// Threads: the UI thread only pumps window messages; a single render thread
// owns D3D/D2D and does decode + present (one hop from network to screen);
// input, network and audio run on their own threads.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "audio_player.h"
#include "gamepad_input.h"
#include "hikari/config.h"
#include "net_client.h"
#include "renderer.h"
#include "video_decoder.h"

namespace hikari {

struct ClientHooks {
    // Offline UI preview; empty during normal remote play.
    std::string preview;
    // Opens the app's Settings window (modal). Returns true if settings changed.
    std::function<bool(HWND parent)> open_settings;
    // Returns true after a different role was selected; close before switching.
    std::function<bool(HWND parent)> change_role;
};

class ClientApp {
public:
    ClientApp();
    ~ClientApp();

    // Runs until the window closes. Returns the process exit code.
    int run(Config& cfg, ClientHooks hooks);

    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    enum class Screen { Lobby, Stream };
    enum Action : int {
        kActNone = 0,
        kActPlay = 1,
        kActStop,
        kActSettings,
        kActController,
        kActWake,
        kActSleep,
        kActPresetLatency,
        kActPresetBalanced,
        kActPresetQuality,
        kActRetry,
        kActFullscreen,
        kActConnection,
        kActChangeRole,
        kActTakeOver,
        kActStats,
        kActScreenshot,
        kActMenu,        // open or close the stream menu
        kActResume,      // close the stream menu
        kActEndAsk,      // show the end-stream confirmation
        kActEndCancel,   // back from the confirmation
        kActEndConfirm,  // end the stream; the session stays ready to resume
        kActStretch,     // fill the whole window instead of keeping 16:9
    };

    void load_settings();
    void connect();
    void preview_state(const std::string& state);
    void render_thread();
    void path_thread();  // Tailscale direct-vs-relayed watcher
    void draw_lobby(ui::Painter& p, const ui::Rect& r);
    void draw_stream_overlay(ui::Painter& p, const ui::Rect& r);
    void draw_stats(ui::Painter& p, const ui::Rect& r);
    void draw_stream_menu(ui::Painter& p, const ui::Rect& r);
    void open_menu(bool confirm_end);
    void close_menu();
    void menu_key(UINT vk);           // UI thread
    std::vector<int> menu_items();    // caller holds mu_
    void post_action(int action);
    void do_action(int action);  // UI thread
    void set_screen(Screen s);
    void toggle_fullscreen();
    void update_sending();
    void toast(const std::string& msg, double seconds = 3.0);
    void on_key(UINT vk, bool down);
    bool mouse_aim_enabled();
    void set_mouse_captured(bool on);  // UI thread
    void clip_cursor();
    void open_controller_settings();
    double now_s() const;

    Config* cfg_ = nullptr;
    ClientHooks hooks_;
    HWND hwnd_ = nullptr;
    GpuDevice gpu_;
    Renderer renderer_;
    VideoDecoder decoder_;
    ClientNet net_;
    GamepadInput input_;
    AudioPlayer audio_;
    InputMapping mapping_;

    std::thread render_;
    std::thread path_;
    Event path_wake_;
    // Route to the host over Tailscale: 0 unknown/not Tailscale, 1 direct, 2 relayed.
    std::atomic<int> path_state_{0};
    std::string path_relay_;  // guarded by mu_
    bool relay_toast_shown_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> resize_pending_{false};
    std::atomic<Screen> screen_{Screen::Lobby};
    std::atomic<bool> focused_{true};
    std::atomic<bool> show_stats_{false};
    std::atomic<bool> menu_open_{false};      // stream menu; console input is paused while open
    std::atomic<bool> ended_by_user_{false};
    std::atomic<bool> switch2_{false};  // client.console_model, chosen in controller settings  // lobby offers Resume after a deliberate end
    std::atomic<bool> fullscreen_{false};
    bool mouse_captured_ = false;  // UI thread
    std::atomic<bool> screenshot_{false};
    std::atomic<bool> need_keyframe_{false};
    RECT windowed_rect_{};
    LONG windowed_style_ = 0;
    uint64_t start_qpc_us_ = 0;

    // Shared UI state (guarded by mu_).
    std::mutex mu_;
    ui::Interaction ui_;
    proto::HostStatus status_{};
    proto::HostStats host_stats_{};
    proto::StreamConfig stream_{};
    bool have_status_ = false;
    int menu_sel_ = 0;
    bool menu_confirm_ = false;
    std::string toast_;
    double toast_until_ = 0;
    std::string host_label_;
    std::atomic<proto::Preset> preset_{proto::Preset::Balanced};
    std::atomic<uint64_t> last_mouse_move_us_{0};
    KeyboardState keys_;

    // Render-thread statistics.
    RollingHistogram e2e_{2'000'000};
    RollingHistogram decode_hist_{2'000'000};
    RateCounter present_rate_, decode_rate_;
    double present_fps_ = 0, decode_fps_ = 0;
    uint64_t decoder_drops_ = 0;
    float e2e_spark_[120]{};
    int spark_pos_ = 0;
};

}  // namespace hikari

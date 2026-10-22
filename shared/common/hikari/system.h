#pragma once
// Windows scheduling, power and timing helpers.
//
// Scheduling policy (see docs/profiling.md): only the threads whose wake-up
// latency directly adds to input/video latency get MMCSS registration; nothing
// runs at REALTIME_PRIORITY_CLASS, which measurably *increases* jitter by
// starving the drivers we depend on (USB, network, GPU).

#include <cstdint>
#include <string>

namespace hikari {

enum class ThreadRole {
    Input,     // controller sampling / input RX / serial TX   -> MMCSS "Games", highest
    Video,     // capture, encode, decode, present            -> MMCSS "Games", high
    Network,   // socket RX / paced TX                         -> MMCSS "Games", high
    Audio,     // WASAPI capture/render                        -> MMCSS "Pro Audio"
    Background // telemetry, UI, discovery
};

// Names the thread (visible in WPA/debuggers) and applies the role's
// priority/MMCSS policy. Safe to call once per thread.
void set_thread_role(ThreadRole role, const char* name);

// Keeps the machine and display awake while streaming, opts the process out
// of EcoQoS/power throttling (laptops otherwise park our threads on E-cores),
// and asks for the high-performance GPU on hybrid laptops.
void apply_process_performance_policy();
void keep_system_awake(bool on);

// High-resolution waitable timer (sub-millisecond on Windows 10 1803+).
class PreciseTimer {
public:
    PreciseTimer();
    ~PreciseTimer();
    PreciseTimer(const PreciseTimer&) = delete;
    PreciseTimer& operator=(const PreciseTimer&) = delete;

    // Sleeps until the absolute QPC-microsecond deadline. Uses the waitable
    // timer for the bulk and a short spin for the last ~200us.
    void sleep_until_us(uint64_t deadline_us);
    void* handle() const { return handle_; }
    // Arms the timer to fire after `us` without blocking (for multi-wait loops).
    void arm_relative_us(uint64_t us);
    bool high_resolution() const { return high_res_; }

private:
    void* handle_ = nullptr;
    bool high_res_ = false;
};

std::string app_data_dir();  // %LOCALAPPDATA%\Hikari (created on demand)
bool ensure_directory(const std::string& path);
std::string executable_dir();
std::string computer_name();
std::string utf8(const wchar_t* w);
std::wstring widen(const std::string& s);
std::string hresult_string(long hr);

}  // namespace hikari

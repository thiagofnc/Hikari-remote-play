#include "hikari/system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <shlobj.h>
#include <timeapi.h>

#include <cstdio>

#include "hikari/clock.h"
#include "hikari/log.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Ask hybrid-GPU laptops (Optimus / AMD switchable) to run us on the
// discrete GPU, which is where the better hardware encoder usually lives.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace hikari {

void set_thread_role(ThreadRole role, const char* name)
{
    using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto set_desc = reinterpret_cast<SetDesc>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set_desc && name) set_desc(GetCurrentThread(), widen(name).c_str());

    DWORD task_index = 0;
    HANDLE mmcss = nullptr;
    switch (role) {
    case ThreadRole::Input:
        mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task_index);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        break;
    case ThreadRole::Video:
    case ThreadRole::Network:
        mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task_index);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        break;
    case ThreadRole::Audio:
        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        break;
    case ThreadRole::Background:
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        break;
    }
    // Per-thread opt-out of EcoQoS so the scheduler does not park us on
    // efficiency cores while waiting on I/O.
    THREAD_POWER_THROTTLING_STATE tp{};
    tp.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    tp.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    tp.StateMask = role == ThreadRole::Background ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tp, sizeof(tp));
}

void apply_process_performance_policy()
{
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4 /*IGNORE_TIMER_RESOLUTION*/;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
}

void keep_system_awake(bool on)
{
    SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
}

PreciseTimer::PreciseTimer()
{
    handle_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    high_res_ = handle_ != nullptr;
    if (!handle_) {
        // Pre-1803 fallback: classic timer + 1 ms system timer resolution.
        handle_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        timeBeginPeriod(1);
    }
}

PreciseTimer::~PreciseTimer()
{
    if (!high_res_) timeEndPeriod(1);
    if (handle_) CloseHandle(handle_);
}

void PreciseTimer::arm_relative_us(uint64_t us)
{
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(us * 10);
    SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE);
}

void PreciseTimer::sleep_until_us(uint64_t deadline_us)
{
    constexpr uint64_t kSpin = 200;
    uint64_t now = now_us();
    if (deadline_us > now + kSpin) {
        arm_relative_us(deadline_us - now - kSpin);
        WaitForSingleObject(handle_, INFINITE);
    }
    while (now_us() < deadline_us) YieldProcessor();
}

std::string utf8(const wchar_t* w)
{
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool ensure_directory(const std::string& path)
{
    const int r = SHCreateDirectoryExW(nullptr, widen(path).c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

std::string app_data_dir()
{
    wchar_t* p = nullptr;
    std::string dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = utf8(p) + "\\Hikari";
    CoTaskMemFree(p);
    if (dir.empty()) dir = executable_dir();
    ensure_directory(dir);
    return dir;
}

std::string executable_dir()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf, n);
    const auto pos = s.find_last_of(L"\\/");
    return utf8(pos == std::wstring::npos ? s.c_str() : s.substr(0, pos).c_str());
}

std::string computer_name()
{
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n)) return "Hikari Host";
    return utf8(buf);
}

std::string hresult_string(long hr)
{
    char buf[256] = {};
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string m = msg ? utf8(msg) : std::string();
    LocalFree(msg);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
    std::snprintf(buf, sizeof(buf), "0x%08lX%s%s", static_cast<unsigned long>(hr), m.empty() ? "" : " ", m.c_str());
    return buf;
}

}  // namespace hikari

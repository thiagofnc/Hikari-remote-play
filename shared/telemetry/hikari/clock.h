#pragma once
// Monotonic high-resolution clock used for every latency timestamp.
// QueryPerformanceCounter is invariant-TSC backed on all supported Windows
// versions; conversion to microseconds is a single multiply/divide.

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <chrono>
#endif

namespace hikari {

#ifdef _WIN32
inline int64_t qpc_frequency()
{
    static const int64_t f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return li.QuadPart;
    }();
    return f;
}

inline int64_t qpc_now()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

inline uint64_t qpc_to_us(int64_t ticks)
{
    const int64_t f = qpc_frequency();
    return static_cast<uint64_t>((ticks / f) * 1000000 + (ticks % f) * 1000000 / f);
}

// Microseconds since boot on the QPC timeline.
inline uint64_t now_us() { return qpc_to_us(qpc_now()); }

// Media Foundation sample times are 100 ns units; MF capture sources stamp
// samples on the QPC timeline (MFGetSystemTime), so this converts directly.
inline uint64_t hns_to_us(int64_t hns) { return static_cast<uint64_t>(hns / 10); }
#else
inline uint64_t now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

}  // namespace hikari

#pragma once
// Lock-free latency histograms.
//
// Averages hide exactly the stalls that make remote play feel bad, so every
// latency in Hikari is recorded into a log-linear histogram (HDR-style,
// <= 6.25% relative error) from which p50/p95/p99/max are read. Recording is
// a handful of relaxed atomic ops and never allocates, so it is safe to call
// from capture callbacks, the input path and the render thread.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace hikari {

struct LatencySummary {
    uint32_t count = 0;
    uint32_t p50 = 0;
    uint32_t p95 = 0;
    uint32_t p99 = 0;
    uint32_t max = 0;
    double mean = 0.0;

    std::string to_string(const char* unit = "us") const;
    // p50/p95/p99/max formatted in milliseconds, e.g. "3.1/4.0/5.2/9.8 ms"
    std::string to_ms_string() const;
};

class Histogram {
public:
    static constexpr int kLinear = 32;
    static constexpr int kSubBits = 4;
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kBuckets = kLinear + (32 - 5) * kSub;

    void record(uint64_t value);
    void reset();
    // Merges into a plain snapshot (counts) so percentiles can be computed
    // across several histograms.
    void accumulate(std::array<uint64_t, kBuckets>& counts, uint64_t& max, uint64_t& sum) const;

    static int bucket_of(uint64_t v);
    static uint64_t bucket_value(int idx);  // representative (midpoint) value
    static LatencySummary summarize(const std::array<uint64_t, kBuckets>& counts, uint64_t max, uint64_t sum);

    LatencySummary summary() const;

private:
    std::array<std::atomic<uint32_t>, kBuckets> buckets_{};
    std::atomic<uint64_t> max_{0};
    std::atomic<uint64_t> sum_{0};
};

// Two-generation rolling window: percentiles cover the last 1-2 windows,
// so a stall shows up immediately and ages out after a couple of seconds.
class RollingHistogram {
public:
    explicit RollingHistogram(uint64_t window_us = 2'000'000) : window_us_(window_us) {}

    void record(uint64_t value, uint64_t now);
    LatencySummary summary(uint64_t now);
    void reset();

private:
    void maybe_rotate(uint64_t now);

    Histogram gen_[2];
    std::atomic<int> cur_{0};
    std::atomic<uint64_t> rotated_at_{0};
    uint64_t window_us_;
};

// Events-per-second counter (FPS, packets/s, ...).
class RateCounter {
public:
    void add(uint64_t n = 1) { count_.fetch_add(n, std::memory_order_relaxed); }
    uint64_t total() const { return count_.load(std::memory_order_relaxed); }
    // Returns the rate since the previous call. Single reader only.
    double sample(uint64_t now_us);

private:
    std::atomic<uint64_t> count_{0};
    uint64_t last_count_ = 0;
    uint64_t last_us_ = 0;
    double last_rate_ = 0.0;
};

}  // namespace hikari

#include "hikari/histogram.h"

#include <algorithm>
#include <cstdio>

namespace hikari {

namespace {
int msb64(uint64_t v)
{
    int n = 0;
    while (v >>= 1) ++n;
    return n;
}
}  // namespace

std::string LatencySummary::to_string(const char* unit) const
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "p50 %u  p95 %u  p99 %u  max %u %s (n=%u)", p50, p95, p99, max, unit, count);
    return buf;
}

std::string LatencySummary::to_ms_string() const
{
    if (count == 0) return "--";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f/%.1f/%.1f/%.1f ms", p50 / 1000.0, p95 / 1000.0, p99 / 1000.0,
                  max / 1000.0);
    return buf;
}

int Histogram::bucket_of(uint64_t v)
{
    if (v < static_cast<uint64_t>(kLinear)) return static_cast<int>(v);
    if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
    const int m = msb64(v);  // >= 5
    const int sub = static_cast<int>((v >> (m - kSubBits)) & (kSub - 1));
    return kLinear + (m - 5) * kSub + sub;
}

uint64_t Histogram::bucket_value(int idx)
{
    if (idx < kLinear) return static_cast<uint64_t>(idx);
    const int m = (idx - kLinear) / kSub + 5;
    const int sub = (idx - kLinear) % kSub;
    const uint64_t lo = (1ull << m) + (static_cast<uint64_t>(sub) << (m - kSubBits));
    const uint64_t width = 1ull << (m - kSubBits);
    return lo + width / 2;
}

void Histogram::record(uint64_t v)
{
    buckets_[bucket_of(v)].fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(v, std::memory_order_relaxed);
    uint64_t prev = max_.load(std::memory_order_relaxed);
    while (v > prev && !max_.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

void Histogram::reset()
{
    for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    max_.store(0, std::memory_order_relaxed);
    sum_.store(0, std::memory_order_relaxed);
}

void Histogram::accumulate(std::array<uint64_t, kBuckets>& counts, uint64_t& max, uint64_t& sum) const
{
    for (int i = 0; i < kBuckets; ++i) counts[i] += buckets_[i].load(std::memory_order_relaxed);
    max = std::max<uint64_t>(max, max_.load(std::memory_order_relaxed));
    sum += sum_.load(std::memory_order_relaxed);
}

LatencySummary Histogram::summarize(const std::array<uint64_t, kBuckets>& counts, uint64_t max, uint64_t sum)
{
    LatencySummary s;
    uint64_t total = 0;
    for (auto c : counts) total += c;
    if (total == 0) return s;
    s.count = static_cast<uint32_t>(std::min<uint64_t>(total, 0xFFFFFFFFu));
    s.max = static_cast<uint32_t>(std::min<uint64_t>(max, 0xFFFFFFFFu));
    s.mean = static_cast<double>(sum) / static_cast<double>(total);
    auto pct = [&](double q) -> uint32_t {
        const uint64_t target = static_cast<uint64_t>(q * static_cast<double>(total - 1)) + 1;
        uint64_t acc = 0;
        for (int i = 0; i < kBuckets; ++i) {
            acc += counts[i];
            if (acc >= target) return static_cast<uint32_t>(std::min<uint64_t>(bucket_value(i), max));
        }
        return s.max;
    };
    s.p50 = pct(0.50);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
    return s;
}

LatencySummary Histogram::summary() const
{
    std::array<uint64_t, kBuckets> counts{};
    uint64_t max = 0, sum = 0;
    accumulate(counts, max, sum);
    return summarize(counts, max, sum);
}

void RollingHistogram::maybe_rotate(uint64_t now)
{
    uint64_t at = rotated_at_.load(std::memory_order_relaxed);
    if (at == 0) {
        rotated_at_.compare_exchange_strong(at, now);
        return;
    }
    if (now - at < window_us_) return;
    if (!rotated_at_.compare_exchange_strong(at, now)) return;  // another thread rotated
    const int next = 1 - cur_.load(std::memory_order_relaxed);
    gen_[next].reset();
    cur_.store(next, std::memory_order_release);
}

void RollingHistogram::record(uint64_t value, uint64_t now)
{
    maybe_rotate(now);
    gen_[cur_.load(std::memory_order_acquire)].record(value);
}

LatencySummary RollingHistogram::summary(uint64_t now)
{
    maybe_rotate(now);
    std::array<uint64_t, Histogram::kBuckets> counts{};
    uint64_t max = 0, sum = 0;
    gen_[0].accumulate(counts, max, sum);
    gen_[1].accumulate(counts, max, sum);
    return Histogram::summarize(counts, max, sum);
}

void RollingHistogram::reset()
{
    gen_[0].reset();
    gen_[1].reset();
}

double RateCounter::sample(uint64_t now)
{
    const uint64_t c = count_.load(std::memory_order_relaxed);
    if (last_us_ != 0 && now > last_us_) {
        const uint64_t dt = now - last_us_;
        if (dt < 100'000) return last_rate_;  // too short to be meaningful
        last_rate_ = static_cast<double>(c - last_count_) * 1e6 / static_cast<double>(dt);
    }
    last_count_ = c;
    last_us_ = now;
    return last_rate_;
}

}  // namespace hikari

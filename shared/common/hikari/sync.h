#pragma once
// Real-time hand-off primitives.
//
// Policy: no unbounded queues anywhere in a real-time path. Producers never
// block. When a consumer falls behind, the oldest data is discarded and the
// discard is counted, because a fresh frame/state is worth more than a
// complete history of stale ones.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hikari {

// Auto-reset Win32 event wrapper (works with WaitForMultipleObjects).
class Event {
public:
    Event() : h_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~Event() { CloseHandle(h_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    void set() { SetEvent(h_); }
    bool wait(DWORD ms = INFINITE) { return WaitForSingleObject(h_, ms) == WAIT_OBJECT_0; }
    HANDLE handle() const { return h_; }

private:
    HANDLE h_;
};

// Single-slot "newest value wins" mailbox. put() overwrites any value the
// consumer has not taken yet and reports that as an overwrite (a drop).
template <typename T>
class Mailbox {
public:
    // Returns true if an unconsumed value was replaced.
    bool put(T v)
    {
        bool replaced;
        {
            std::lock_guard<std::mutex> lk(mu_);
            replaced = value_.has_value();
            value_ = std::move(v);
        }
        if (replaced) overwrites_.fetch_add(1, std::memory_order_relaxed);
        ev_.set();
        return replaced;
    }
    std::optional<T> take()
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::optional<T> v = std::move(value_);
        value_.reset();
        return v;
    }
    bool empty() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return !value_.has_value();
    }
    HANDLE event() const { return ev_.handle(); }
    uint64_t overwrites() const { return overwrites_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mu_;
    std::optional<T> value_;
    Event ev_;
    std::atomic<uint64_t> overwrites_{0};
};

// Fixed-capacity FIFO for data that must not be skipped (encoded video
// frames feeding a decoder). On overflow the caller decides how to recover
// (typically: flush + request a keyframe).
template <typename T, size_t N>
class BoundedQueue {
public:
    bool push(T v)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (count_ == N) return false;
            items_[(head_ + count_) % N] = std::move(v);
            ++count_;
        }
        ev_.set();
        return true;
    }
    std::optional<T> pop()
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (count_ == 0) return std::nullopt;
        std::optional<T> v = std::move(items_[head_]);
        head_ = (head_ + 1) % N;
        --count_;
        return v;
    }
    size_t size() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return count_;
    }
    template <typename F>
    void clear(F&& on_item)
    {
        std::lock_guard<std::mutex> lk(mu_);
        while (count_) {
            on_item(std::move(items_[head_]));
            head_ = (head_ + 1) % N;
            --count_;
        }
    }
    HANDLE event() const { return ev_.handle(); }
    static constexpr size_t capacity() { return N; }

private:
    mutable std::mutex mu_;
    T items_[N]{};
    size_t head_ = 0;
    size_t count_ = 0;
    Event ev_;
};

// NTP-style clock offset estimator. Uses the minimum-RTT sample in a sliding
// window: queueing delay only ever inflates RTT, so the fastest exchange is
// the one whose midpoint is most trustworthy.
class ClockSync {
public:
    // local_send/local_recv: our clock; remote: peer clock when it replied.
    void add_sample(uint64_t local_send, uint64_t remote, uint64_t local_recv)
    {
        if (local_recv < local_send) return;
        const uint64_t rtt = local_recv - local_send;
        const int64_t offset = static_cast<int64_t>(remote) - static_cast<int64_t>(local_send + rtt / 2);
        Sample s{rtt, offset, local_recv};
        samples_[next_++ % kWindow] = s;
        if (count_ < kWindow) ++count_;
        Sample best = samples_[0];
        for (size_t i = 1; i < count_; ++i)
            if (samples_[i].rtt < best.rtt) best = samples_[i];
        offset_.store(best.offset, std::memory_order_relaxed);
        rtt_.store(rtt, std::memory_order_relaxed);
        min_rtt_.store(best.rtt, std::memory_order_relaxed);
        valid_.store(true, std::memory_order_release);
    }
    // remote_clock - local_clock
    int64_t offset() const { return offset_.load(std::memory_order_relaxed); }
    uint64_t last_rtt() const { return rtt_.load(std::memory_order_relaxed); }
    uint64_t min_rtt() const { return min_rtt_.load(std::memory_order_relaxed); }
    bool valid() const { return valid_.load(std::memory_order_acquire); }
    uint64_t to_local(uint64_t remote_time) const { return remote_time - static_cast<uint64_t>(offset()); }
    uint64_t to_remote(uint64_t local_time) const { return local_time + static_cast<uint64_t>(offset()); }

private:
    struct Sample {
        uint64_t rtt = UINT64_MAX;
        int64_t offset = 0;
        uint64_t at = 0;
    };
    static constexpr size_t kWindow = 32;
    Sample samples_[kWindow];
    size_t next_ = 0;
    size_t count_ = 0;
    std::atomic<int64_t> offset_{0};
    std::atomic<uint64_t> rtt_{0};
    std::atomic<uint64_t> min_rtt_{0};
    std::atomic<bool> valid_{false};
};

}  // namespace hikari

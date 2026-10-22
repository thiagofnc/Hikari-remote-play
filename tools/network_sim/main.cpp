// hikari-netsim: UDP impairment proxy for testing the client <-> host
// protocol under bad networks.
//
//   client  --->  [listen port]  netsim  [ephemeral] --->  target (host)
//   client  <---                 netsim              <---  target
//
// Each direction has its own impairment pipeline, applied in the order a
// real path applies them:
//   1. random loss (Bernoulli + Gilbert-Elliott bursts)   -> "loss" drops
//   2. bottleneck link: token bucket at --rate with a bounded FIFO; packets
//      that would wait longer than --queue-ms are tail-dropped -> "queue" drops
//   3. propagation: fixed delay + random jitter (order-preserving by default),
//      optional reordering (packet held back by --reorder-gap)
// Departures are scheduled on one priority queue served by a high-resolution
// waitable timer + short spin, so timing is accurate to tens of microseconds
// instead of the 15.6 ms Sleep() granularity.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "hikari/clock.h"
#include "hikari/histogram.h"
#include "hikari/sync.h"
#include "hikari/system.h"
#include "hikari/udp.h"
#include "tool_common.h"

using namespace hikari;
using namespace hikari::tool;

namespace {

const char* kHelp = R"(hikari-netsim - UDP network impairment proxy

Usage:
  hikari-netsim [--listen PORT] [--target HOST:PORT] [impairments] [options]
  hikari-netsim --selftest

Point the client at the listen port; datagrams are forwarded to the target and
replies are relayed back to the most recent client address (single client).

Impairments (apply to both directions; prefix with up- or down- to set one
direction only, e.g. --down-loss 2. "up" = client->target):
  --delay MS         fixed one-way delay (default 0)
  --jitter MS        random extra delay, uniform in [-J, +J] around --delay
  --reorder-jitter   let jitter reorder packets (default: jitter keeps order,
                     a packet never overtakes one sent before it)
  --loss PCT         independent random loss, percent
  --burst-loss PCT   Gilbert-Elliott: chance per packet to enter a loss burst
  --burst-len N      mean burst length in packets (default 4); every packet
                     inside a burst is lost. Long-run burst loss rate is
                     p/(p+1/N) with p = burst-loss/100.
  --reorder PCT      chance a packet is held back --reorder-gap ms extra so
                     later packets overtake it
  --reorder-gap MS   hold-back time for reordered packets (default 8)
  --rate KBPS        bottleneck bandwidth in kbit/s (0 = unlimited)
  --queue-ms MS      bottleneck queue size, in ms of data at --rate; packets
                     beyond it are tail-dropped (default 60)
  --bucket-kb KB     token bucket depth = burst allowed at line speed (default 4)
  --profile NAME     start from a named profile; explicit options override it

Profiles (per direction, one-way values):
  wifi           3 ms +- 4 ms, 0.3% loss + bursts (0.2%, len 3), 80 Mbit/s,
                 30 ms queue                      -- busy 5 GHz home Wi-Fi
  lte            30 ms +- 12 ms, 0.5% loss + bursts (0.3%, len 5),
                 up 8 / down 30 Mbit/s, 80 ms queue  -- decent mobile link
  bad            50 ms +- 25 ms, 3% loss + bursts (1%, len 6), 1% reorder,
                 6 Mbit/s, 150 ms queue           -- congested / marginal
  transatlantic  45 ms +- 2 ms (~90 ms RTT), 0.1% loss, 100 Mbit/s,
                 50 ms queue                      -- long fat pipe

Options:
  --listen PORT      port clients send to (default 47801)
  --target H:P       where to forward (default 127.0.0.1:47800)
  --seconds N        stop after N seconds (default: run until Ctrl+C)
  --csv FILE         log the once-per-second stats as CSV
  --seed N           random seed (default: time based)
  --quiet            no per-second output
  --selftest         run an in-process loopback check of every impairment
  --help             this text

Per-second output, per direction: packets/s in and out, delivered Mbit/s,
drops by random loss and by queue overflow, mean bottleneck queueing delay.
)";

// ---- configuration -------------------------------------------------------

struct DirConfig {
    double delay_ms = 0;
    double jitter_ms = 0;
    bool keep_order = true;
    double loss_pct = 0;
    double burst_pct = 0;
    double burst_len = 4;
    double reorder_pct = 0;
    double reorder_gap_ms = 8;
    double rate_kbps = 0;
    double queue_ms = 60;
    double bucket_kb = 4;

    std::string describe() const
    {
        std::string s = strf("delay %.1f+-%.1f ms%s, loss %.2f%%", delay_ms, jitter_ms,
                             jitter_ms > 0 && !keep_order ? " (reordering)" : "", loss_pct);
        if (burst_pct > 0) s += strf(", bursts %.2f%% len %.1f", burst_pct, burst_len);
        if (reorder_pct > 0) s += strf(", reorder %.2f%% (+%.0f ms)", reorder_pct, reorder_gap_ms);
        s += rate_kbps > 0 ? strf(", rate %.0f kbit/s, queue %.0f ms", rate_kbps, queue_ms) : ", unlimited rate";
        return s;
    }
};

bool apply_profile(const std::string& name, DirConfig& up, DirConfig& down)
{
    DirConfig c;
    if (name == "wifi") {
        c.delay_ms = 3, c.jitter_ms = 4, c.loss_pct = 0.3, c.burst_pct = 0.2, c.burst_len = 3;
        c.rate_kbps = 80000, c.queue_ms = 30;
        up = down = c;
    } else if (name == "lte") {
        c.delay_ms = 30, c.jitter_ms = 12, c.loss_pct = 0.5, c.burst_pct = 0.3, c.burst_len = 5, c.queue_ms = 80;
        up = down = c;
        up.rate_kbps = 8000;
        down.rate_kbps = 30000;
    } else if (name == "bad") {
        c.delay_ms = 50, c.jitter_ms = 25, c.loss_pct = 3, c.burst_pct = 1, c.burst_len = 6, c.reorder_pct = 1;
        c.rate_kbps = 6000, c.queue_ms = 150;
        up = down = c;
    } else if (name == "transatlantic") {
        c.delay_ms = 45, c.jitter_ms = 2, c.loss_pct = 0.1, c.rate_kbps = 100000, c.queue_ms = 50;
        up = down = c;
    } else {
        return false;
    }
    return true;
}

// ---- per-direction state ---------------------------------------------------

struct DirStats {
    std::atomic<uint64_t> in_pkts{0}, in_bytes{0};
    std::atomic<uint64_t> out_pkts{0}, out_bytes{0};
    std::atomic<uint64_t> drop_loss{0}, drop_queue{0}, drop_noclient{0};
    std::atomic<uint64_t> queue_us_sum{0}, queue_n{0};
};

struct DirSnapshot {
    uint64_t in_pkts = 0, in_bytes = 0, out_pkts = 0, out_bytes = 0, drop_loss = 0, drop_queue = 0, queue_us = 0,
             queue_n = 0;
    static DirSnapshot take(const DirStats& s)
    {
        DirSnapshot d;
        d.in_pkts = s.in_pkts.load();
        d.in_bytes = s.in_bytes.load();
        d.out_pkts = s.out_pkts.load();
        d.out_bytes = s.out_bytes.load();
        d.drop_loss = s.drop_loss.load();
        d.drop_queue = s.drop_queue.load() + s.drop_noclient.load();
        d.queue_us = s.queue_us_sum.load();
        d.queue_n = s.queue_n.load();
        return d;
    }
};

// Impairment decisions for one direction. Only the direction's receive
// thread touches this, so no locking.
class Impairer {
public:
    void init(const DirConfig& c, uint64_t seed)
    {
        cfg_ = c;
        rng_.seed(seed);
        bad_ = false;
        tb_time_ = 0;
        tb_tokens_ = bucket_bytes();
        last_deliver_ = 0;
    }

    // Returns false when the packet is dropped; otherwise its delivery time.
    bool admit(uint64_t now, size_t bytes, DirStats& st, uint64_t& deliver)
    {
        // 1. Random loss. The burst state machine advances on every packet.
        if (cfg_.burst_pct > 0) {
            if (bad_) {
                if (uni() < 1.0 / std::max(1.0, cfg_.burst_len)) bad_ = false;
            } else if (uni() < cfg_.burst_pct / 100.0) {
                bad_ = true;
            }
        }
        if (bad_ || (cfg_.loss_pct > 0 && uni() < cfg_.loss_pct / 100.0)) {
            ++st.drop_loss;
            return false;
        }

        // 2. Bottleneck: token bucket with a FIFO in front of it. Computed
        // analytically (departure time), so no per-byte timer is needed.
        uint64_t depart = now;
        if (cfg_.rate_kbps > 0) {
            const double bytes_per_us = cfg_.rate_kbps * 1000.0 / 8.0 / 1e6;
            const uint64_t start = std::max(now, tb_time_);
            double tokens = std::min(bucket_bytes(), tb_tokens_ + double(start - tb_time_) * bytes_per_us);
            uint64_t t = start;
            if (tokens >= double(bytes)) {
                tokens -= double(bytes);
            } else {
                t = start + static_cast<uint64_t>(std::ceil((double(bytes) - tokens) / bytes_per_us));
                tokens = 0;
            }
            const uint64_t wait = t - now;
            if (double(wait) > cfg_.queue_ms * 1000.0) {
                ++st.drop_queue;  // tail drop: the queue already holds queue_ms of data
                return false;
            }
            tb_time_ = t;
            tb_tokens_ = tokens;
            depart = t;
            st.queue_us_sum += wait;
            ++st.queue_n;
        }

        // 3. Propagation delay + jitter, then optional reordering.
        double d_us = cfg_.delay_ms * 1000.0;
        if (cfg_.jitter_ms > 0) d_us += (uni() * 2.0 - 1.0) * cfg_.jitter_ms * 1000.0;
        deliver = depart + static_cast<uint64_t>(std::max(0.0, d_us));
        if (cfg_.reorder_pct > 0 && uni() < cfg_.reorder_pct / 100.0) {
            // Held back: everything sent in the next reorder_gap overtakes it.
            // It does not advance the in-order horizon.
            deliver += static_cast<uint64_t>(cfg_.reorder_gap_ms * 1000.0);
            return true;
        }
        if (cfg_.keep_order) {
            deliver = std::max(deliver, last_deliver_);
            last_deliver_ = deliver;
        }
        return true;
    }

private:
    double uni() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
    double bucket_bytes() const { return std::max(1500.0, cfg_.bucket_kb * 1024.0); }

    DirConfig cfg_;
    std::mt19937_64 rng_;
    bool bad_ = false;
    uint64_t tb_time_ = 0;
    double tb_tokens_ = 0;
    uint64_t last_deliver_ = 0;
};

// ---- the proxy -------------------------------------------------------------

class Proxy {
public:
    enum Dir { kUp = 0, kDown = 1 };

    ~Proxy() { stop(); }

    bool start(uint16_t listen_port, const NetAddress& target, const DirConfig& up, const DirConfig& down,
               uint64_t seed)
    {
        target_ = target;
        cfg_[kUp] = up;
        cfg_[kDown] = down;
        imp_[kUp].init(up, seed);
        imp_[kDown].init(down, seed ^ 0x9E3779B97F4A7C15ull);
        if (!listen_.open(listen_port)) return false;
        if (!upstream_.open(0)) return false;
        running_ = true;
        sched_ = std::thread(&Proxy::scheduler, this);
        rx_[kUp] = std::thread(&Proxy::receiver, this, kUp);
        rx_[kDown] = std::thread(&Proxy::receiver, this, kDown);
        return true;
    }

    void stop()
    {
        if (!running_.exchange(false)) return;
        wake_.set();
        for (auto& t : rx_)
            if (t.joinable()) t.join();
        if (sched_.joinable()) sched_.join();
        listen_.close();
        upstream_.close();
    }

    uint16_t listen_port() const { return listen_.local_port(); }
    const DirStats& stats(Dir d) const { return stats_[d]; }
    std::string client() const
    {
        std::lock_guard<std::mutex> lk(client_mu_);
        return client_.valid() ? client_.to_string() : std::string("(none yet)");
    }

private:
    struct Item {
        uint64_t due;
        uint64_t order;
        int dir;
        std::vector<uint8_t> data;
    };
    struct Later {
        bool operator()(const Item& a, const Item& b) const
        {
            return a.due != b.due ? a.due > b.due : a.order > b.order;
        }
    };

    void receiver(int dir)
    {
        set_thread_role(ThreadRole::Network, dir == kUp ? "netsim-up-rx" : "netsim-down-rx");
        UdpSocket& sock = dir == kUp ? listen_ : upstream_;
        std::vector<uint8_t> buf(65536);
        while (running_) {
            NetAddress from;
            const int n = sock.recv_from(buf.data(), buf.size(), from, 50);
            if (n < 0) continue;
            const uint64_t now = now_us();
            if (dir == kUp) {
                std::lock_guard<std::mutex> lk(client_mu_);
                client_ = from;  // replies go to the most recent client
            } else if (from != target_) {
                continue;  // only the target may talk back
            }
            DirStats& st = stats_[dir];
            ++st.in_pkts;
            st.in_bytes += static_cast<uint64_t>(n);
            uint64_t deliver = 0;
            if (!imp_[dir].admit(now, static_cast<size_t>(n), st, deliver)) continue;
            Item it{deliver, 0, dir, std::vector<uint8_t>(buf.begin(), buf.begin() + n)};
            bool earliest;
            {
                std::lock_guard<std::mutex> lk(mu_);
                it.order = order_++;
                heap_.push_back(std::move(it));
                std::push_heap(heap_.begin(), heap_.end(), Later{});
                earliest = heap_.front().order == order_ - 1;
            }
            if (earliest) wake_.set();
        }
    }

    void send(Item& it)
    {
        DirStats& st = stats_[it.dir];
        if (it.dir == kUp) {
            upstream_.send_to(target_, it.data.data(), it.data.size());
        } else {
            NetAddress to;
            {
                std::lock_guard<std::mutex> lk(client_mu_);
                to = client_;
            }
            if (!to.valid()) {
                ++st.drop_noclient;
                return;
            }
            listen_.send_to(to, it.data.data(), it.data.size());
        }
        ++st.out_pkts;
        st.out_bytes += it.data.size();
    }

    void scheduler()
    {
        set_thread_role(ThreadRole::Network, "netsim-sched");
        PreciseTimer timer;
        constexpr uint64_t kSlackUs = 20;   // send this early rather than spin for it
        constexpr uint64_t kSpinUs = 250;   // timer wake-up error budget, spun away
        std::vector<Item> batch;
        while (running_) {
            uint64_t due = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const uint64_t now = now_us();
                while (!heap_.empty() && heap_.front().due <= now + kSlackUs) {
                    std::pop_heap(heap_.begin(), heap_.end(), Later{});
                    batch.push_back(std::move(heap_.back()));
                    heap_.pop_back();
                }
                if (heap_.empty() == false) due = heap_.front().due;
            }
            if (!batch.empty()) {
                for (auto& it : batch) send(it);
                batch.clear();
                continue;
            }
            if (due == 0) {
                wake_.wait(50);
                continue;
            }
            const uint64_t now = now_us();
            if (due > now + kSpinUs) {
                // Wake on the timer or on a new, earlier packet.
                timer.arm_relative_us(due - now - kSpinUs);
                HANDLE hs[2] = {static_cast<HANDLE>(timer.handle()), wake_.handle()};
                WaitForMultipleObjects(2, hs, FALSE, 100);
            } else {
                while (now_us() + kSlackUs < due) YieldProcessor();
            }
        }
    }

    UdpSocket listen_, upstream_;
    NetAddress target_;
    mutable std::mutex client_mu_;
    NetAddress client_;
    DirConfig cfg_[2];
    Impairer imp_[2];
    DirStats stats_[2];

    std::mutex mu_;
    std::vector<Item> heap_;
    uint64_t order_ = 0;
    Event wake_;
    std::atomic<bool> running_{false};
    std::thread rx_[2];
    std::thread sched_;
};

// ---- argument handling -----------------------------------------------------

const char* kImpairKeys[] = {"delay",  "jitter",   "loss",     "burst-loss", "burst-len",
                             "reorder", "reorder-gap", "rate", "queue-ms",   "bucket-kb"};

bool apply_option(DirConfig& c, const std::string& key, double v)
{
    if (key == "delay") c.delay_ms = v;
    else if (key == "jitter") c.jitter_ms = v;
    else if (key == "loss") c.loss_pct = v;
    else if (key == "burst-loss") c.burst_pct = v;
    else if (key == "burst-len") c.burst_len = v;
    else if (key == "reorder") c.reorder_pct = v;
    else if (key == "reorder-gap") c.reorder_gap_ms = v;
    else if (key == "rate") c.rate_kbps = v;
    else if (key == "queue-ms") c.queue_ms = v;
    else if (key == "bucket-kb") c.bucket_kb = v;
    else return false;
    return true;
}

bool valid(const DirConfig& c, std::string& why)
{
    auto pct = [](double v) { return v >= 0 && v <= 100; };
    if (c.delay_ms < 0 || c.jitter_ms < 0 || c.reorder_gap_ms < 0 || c.rate_kbps < 0 || c.queue_ms < 0 ||
        c.bucket_kb < 0) {
        why = "negative values are not allowed";
        return false;
    }
    if (!pct(c.loss_pct) || !pct(c.burst_pct) || !pct(c.reorder_pct)) {
        why = "percentages must be within 0..100";
        return false;
    }
    if (c.burst_len < 1) {
        why = "--burst-len must be >= 1";
        return false;
    }
    return true;
}

// ---- self test ---------------------------------------------------------------

// Echo server + paced sender on loopback, measuring what actually arrives.
struct Probe {
    uint32_t seq;
    uint64_t sent_us;
};

struct SelfResult {
    uint64_t sent = 0, received = 0;
    LatencySummary rtt;
    uint64_t rtt_min = 0;
    double loss_pct = 0;
    double mean_burst = 0;   // mean run length of consecutive losses
    double reordered_pct = 0;
    double delivered_mbps = 0;
};

SelfResult run_probe(const DirConfig& up, const DirConfig& down, double pps, size_t size, double seconds)
{
    SelfResult r;
    UdpSocket echo, client;
    echo.open(0);
    client.open(0);
    NetAddress echo_addr, proxy_addr;
    NetAddress::resolve("127.0.0.1", echo.local_port(), echo_addr);

    Proxy proxy;
    if (!proxy.start(0, echo_addr, up, down, 12345)) return r;
    NetAddress::resolve("127.0.0.1", proxy.listen_port(), proxy_addr);

    std::atomic<bool> run{true};
    std::thread echo_thread([&] {
        std::vector<uint8_t> b(65536);
        while (run) {
            NetAddress from;
            const int n = echo.recv_from(b.data(), b.size(), from, 20);
            if (n > 0) echo.send_to(from, b.data(), static_cast<size_t>(n));
        }
    });

    const uint64_t total = static_cast<uint64_t>(pps * seconds);
    std::vector<uint64_t> arrival_rtt(total, UINT64_MAX);
    Histogram hist;
    std::atomic<uint64_t> received{0}, bytes_rx{0}, reordered{0};
    std::atomic<uint64_t> last_rx_us{0};
    std::thread rx_thread([&] {
        set_thread_role(ThreadRole::Network, "selftest-rx");
        std::vector<uint8_t> b(65536);
        int64_t highest = -1;
        while (run) {
            NetAddress from;
            const int n = client.recv_from(b.data(), b.size(), from, 20);
            if (n < static_cast<int>(sizeof(Probe))) continue;
            const uint64_t now = now_us();
            Probe p;
            std::memcpy(&p, b.data(), sizeof(p));
            if (p.seq >= total || arrival_rtt[p.seq] != UINT64_MAX) continue;
            arrival_rtt[p.seq] = now - p.sent_us;
            hist.record(now - p.sent_us);
            if (static_cast<int64_t>(p.seq) < highest) ++reordered;
            highest = std::max<int64_t>(highest, p.seq);
            ++received;
            bytes_rx += static_cast<uint64_t>(n);
            last_rx_us = now;
        }
    });

    // Paced sender: 1 ms ticks, sending whatever the schedule owes.
    std::vector<uint8_t> pkt(std::max(size, sizeof(Probe)), 0xA5);
    PreciseTimer timer;
    const uint64_t t0 = now_us();
    uint64_t sent = 0;
    while (sent < total) {
        const uint64_t now = now_us();
        const uint64_t owed = std::min<uint64_t>(total, static_cast<uint64_t>(double(now - t0) * pps / 1e6) + 1);
        while (sent < owed) {
            Probe p{static_cast<uint32_t>(sent), now_us()};
            std::memcpy(pkt.data(), &p, sizeof(p));
            client.send_to(proxy_addr, pkt.data(), pkt.size());
            ++sent;
        }
        timer.sleep_until_us(now + 1000);
    }
    const uint64_t send_end = now_us();
    // Drain: wait for stragglers (max delay + queue + reorder hold-back).
    const double max_ms = up.delay_ms + up.jitter_ms + up.queue_ms + up.reorder_gap_ms + down.delay_ms +
                          down.jitter_ms + down.queue_ms + down.reorder_gap_ms + 200;
    timer.sleep_until_us(now_us() + static_cast<uint64_t>(max_ms * 1000));
    run = false;
    rx_thread.join();
    echo_thread.join();
    proxy.stop();

    r.sent = sent;
    r.received = received.load();
    r.rtt = hist.summary();
    r.rtt_min = UINT64_MAX;
    for (auto v : arrival_rtt)
        if (v != UINT64_MAX) r.rtt_min = std::min(r.rtt_min, v);
    if (r.rtt_min == UINT64_MAX) r.rtt_min = 0;
    r.loss_pct = sent ? 100.0 * double(sent - r.received) / double(sent) : 0;
    uint64_t runs = 0, lost = 0;
    bool in_run = false;
    for (auto v : arrival_rtt) {
        if (v == UINT64_MAX) {
            ++lost;
            if (!in_run) ++runs;
            in_run = true;
        } else {
            in_run = false;
        }
    }
    r.mean_burst = runs ? double(lost) / double(runs) : 0;
    r.reordered_pct = r.received ? 100.0 * double(reordered.load()) / double(r.received) : 0;
    const uint64_t span = std::max<uint64_t>(1, std::max(last_rx_us.load(), send_end) - t0);
    r.delivered_mbps = double(bytes_rx.load()) * 8.0 / double(span);
    return r;
}

int selftest()
{
    std::printf("hikari-netsim self test (loopback: sender -> netsim -> echo -> netsim -> sender)\n\n");
    int failures = 0;
    Table t({"Scenario", "Expect", "Measured", "Result"});
    auto check = [&](const std::string& name, const std::string& expect, const std::string& got, bool ok) {
        t.row({name, expect, got, ok ? "PASS" : "FAIL"});
        if (!ok) ++failures;
    };
    const DirConfig clean;

    // Baseline: what the proxy itself adds at video-like rates.
    {
        std::printf("  [1/6] 40 Mbit/s of 1200-byte datagrams, no impairment ...\n");
        std::fflush(stdout);
        const auto r = run_probe(clean, clean, 40e6 / 8 / 1200, 1200, 3);
        check("throughput 40 Mbit/s", "loss < 0.5%", strf("loss %.2f%%, %.1f Mbit/s", r.loss_pct, r.delivered_mbps),
              r.loss_pct < 0.5);
        check("added latency (RTT, 2 hops)", "p99 < 2 ms",
              strf("p50 %.3f / p99 %.3f / max %.2f ms", r.rtt.p50 / 1e3, r.rtt.p99 / 1e3, r.rtt.max / 1e3),
              r.rtt.p99 < 2000);
    }
    // Delay + jitter + Bernoulli loss, upstream only.
    {
        std::printf("  [2/6] delay 40 ms, jitter 10 ms (reordering allowed), loss 2%% ...\n");
        std::fflush(stdout);
        DirConfig up;
        up.delay_ms = 40, up.jitter_ms = 10, up.keep_order = false, up.loss_pct = 2;
        const auto r = run_probe(up, clean, 500, 200, 6);
        check("delay p50", "40 +- 2 ms", strf("%.2f ms", r.rtt.p50 / 1e3), std::abs(r.rtt.p50 / 1e3 - 40) < 2.0);
        check("jitter range", "min >= 30, max <= 51 ms", strf("min %.2f, max %.2f ms", r.rtt_min / 1e3, r.rtt.max / 1e3),
              r.rtt_min >= 29'900 && r.rtt.max <= 51'500);
        check("loss", "2% +- 0.8", strf("%.2f%%", r.loss_pct), std::abs(r.loss_pct - 2.0) < 0.8);
    }
    // Same jitter, order preserved.
    {
        std::printf("  [3/6] delay 40 ms, jitter 10 ms, order preserved ...\n");
        std::fflush(stdout);
        DirConfig up;
        up.delay_ms = 40, up.jitter_ms = 10;
        const auto r = run_probe(up, clean, 500, 200, 3);
        check("keep order", "0% reordered, min >= 30 ms",
              strf("%.2f%% reordered, min %.2f ms", r.reordered_pct, r.rtt_min / 1e3),
              r.reordered_pct == 0 && r.rtt_min >= 29'900);
    }
    // Gilbert-Elliott bursts.
    {
        std::printf("  [4/6] burst loss 1%% enter, mean burst 5 ...\n");
        std::fflush(stdout);
        DirConfig up;
        up.burst_pct = 1, up.burst_len = 5;
        const double expect = 100.0 * 0.01 / (0.01 + 0.2);
        const auto r = run_probe(up, clean, 2500, 200, 6);
        check("burst loss rate", strf("%.1f%% +- 1.5", expect), strf("%.2f%%", r.loss_pct),
              std::abs(r.loss_pct - expect) < 1.5);
        check("mean burst length", "5 +- 1.2 packets", strf("%.2f", r.mean_burst), std::abs(r.mean_burst - 5) < 1.2);
    }
    // Reordering.
    {
        std::printf("  [5/6] reorder 5%% (8 ms hold-back) ...\n");
        std::fflush(stdout);
        DirConfig up;
        up.reorder_pct = 5;
        const auto r = run_probe(up, clean, 1000, 200, 4);
        check("reordered", "5% +- 1.5", strf("%.2f%%", r.reordered_pct), std::abs(r.reordered_pct - 5) < 1.5);
    }
    // Bottleneck: 30 Mbit/s offered into 20 Mbit/s with a 50 ms queue.
    {
        std::printf("  [6/6] 30 Mbit/s offered into a 20 Mbit/s bottleneck, 50 ms queue ...\n");
        std::fflush(stdout);
        DirConfig up;
        up.rate_kbps = 20000, up.queue_ms = 50;
        const auto r = run_probe(up, clean, 30e6 / 8 / 1200, 1200, 4);
        check("bottleneck throughput", "20 Mbit/s +- 5%", strf("%.2f Mbit/s", r.delivered_mbps),
              std::abs(r.delivered_mbps - 20) < 1.0);
        check("tail drop", "~33% lost", strf("%.1f%%", r.loss_pct), std::abs(r.loss_pct - 33.3) < 4);
        check("queueing delay", "p50 ~50 ms (full queue)", strf("p50 %.1f ms", r.rtt.p50 / 1e3),
              r.rtt.p50 > 40'000 && r.rtt.p50 < 55'000);
    }
    std::printf("\n");
    t.print();
    std::printf("\n%s\n", failures ? strf("%d check(s) FAILED", failures).c_str() : "All checks passed.");
    return failures ? 1 : 0;
}

std::atomic<bool> g_stop{false};
BOOL WINAPI on_ctrl(DWORD)
{
    g_stop = true;
    return TRUE;
}

}  // namespace

int main(int argc, char** argv)
{
    std::set<std::string> options{"listen", "target", "profile", "seconds", "csv", "seed"};
    for (const char* k : kImpairKeys) {
        options.insert(k);
        options.insert(std::string("up-") + k);
        options.insert(std::string("down-") + k);
    }
    Args args(argc, argv, options,
              {"quiet", "selftest", "verbose", "reorder-jitter", "up-reorder-jitter", "down-reorder-jitter"});
    if (args.has("help")) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    init_console(args.has("verbose"));
    if (!args.error().empty()) {
        std::fprintf(stderr, "error: %s\nRun with --help for usage.\n", args.error().c_str());
        return 64;
    }
    apply_process_performance_policy();
    if (!UdpSocket::startup()) {
        std::fprintf(stderr, "Winsock initialisation failed.\n");
        return 1;
    }
    if (args.has("selftest")) return selftest();

    DirConfig up, down;
    if (args.has("profile") && !apply_profile(lower(args.get("profile")), up, down)) {
        std::fprintf(stderr, "error: unknown profile '%s' (wifi, lte, bad, transatlantic)\n",
                     args.get("profile").c_str());
        return 64;
    }
    for (const char* k : kImpairKeys) {
        if (args.has(k)) {
            const double v = args.num(k, 0);
            apply_option(up, k, v);
            apply_option(down, k, v);
        }
    }
    for (const char* k : kImpairKeys) {
        if (args.has(std::string("up-") + k)) apply_option(up, k, args.num(std::string("up-") + k, 0));
        if (args.has(std::string("down-") + k)) apply_option(down, k, args.num(std::string("down-") + k, 0));
    }
    if (args.has("reorder-jitter")) up.keep_order = down.keep_order = false;
    if (args.has("up-reorder-jitter")) up.keep_order = false;
    if (args.has("down-reorder-jitter")) down.keep_order = false;

    const double listen = args.num("listen", 47801);
    const double seconds = args.num("seconds", 0);
    uint64_t seed = static_cast<uint64_t>(args.num("seed", 0));
    if (!seed) seed = now_us();
    std::string why;
    if (!args.error().empty() || listen < 1 || listen > 65535 || !valid(up, why) || !valid(down, why)) {
        std::fprintf(stderr, "error: %s\nRun with --help for usage.\n",
                     !args.error().empty() ? args.error().c_str() : !why.empty() ? why.c_str() : "bad --listen port");
        return 64;
    }
    NetAddress target;
    const std::string target_str = args.get("target", "127.0.0.1:47800");
    if (!NetAddress::resolve(target_str, 47800, target)) {
        std::fprintf(stderr, "error: cannot resolve target '%s'\n", target_str.c_str());
        return 64;
    }

    Proxy proxy;
    if (!proxy.start(static_cast<uint16_t>(listen), target, up, down, seed)) {
        std::fprintf(stderr, "error: cannot bind UDP port %d (in use?)\n", static_cast<int>(listen));
        return 1;
    }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    std::printf("hikari-netsim: listening on UDP %d -> %s\n", static_cast<int>(listen), target.to_string().c_str());
    std::printf("  up   (client->target): %s\n", up.describe().c_str());
    std::printf("  down (target->client): %s\n", down.describe().c_str());
    std::printf("Ctrl+C to stop.\n\n");

    FILE* csv = nullptr;
    if (args.has("csv")) {
        csv = std::fopen(args.get("csv").c_str(), "w");
        if (!csv) {
            std::fprintf(stderr, "error: cannot write %s\n", args.get("csv").c_str());
            return 1;
        }
        std::fprintf(csv, "time_s,dir,in_pps,out_pps,out_mbps,drop_loss,drop_queue,avg_queue_ms\n");
    }

    const bool quiet = args.has("quiet");
    if (!quiet)
        std::printf("%6s %-4s %8s %8s %9s %10s %10s %9s\n", "t(s)", "dir", "in pps", "out pps", "Mbit/s", "drop loss",
                    "drop queue", "queue ms");
    DirSnapshot prev[2] = {DirSnapshot::take(proxy.stats(Proxy::kUp)), DirSnapshot::take(proxy.stats(Proxy::kDown))};
    const uint64_t t0 = now_us();
    uint64_t next = t0 + 1'000'000;
    uint64_t last = t0;
    PreciseTimer timer;
    while (!g_stop) {
        timer.sleep_until_us(std::min(next, now_us() + 100'000));
        const uint64_t now = now_us();
        if (seconds > 0 && double(now - t0) >= seconds * 1e6) g_stop = true;
        if (now < next && !g_stop) continue;
        const double dt = double(now - last) / 1e6;
        last = now;
        next += 1'000'000;
        for (int d = 0; d < 2; ++d) {
            const DirSnapshot cur = DirSnapshot::take(proxy.stats(static_cast<Proxy::Dir>(d)));
            const DirSnapshot& p = prev[d];
            const double in_pps = double(cur.in_pkts - p.in_pkts) / dt;
            const double out_pps = double(cur.out_pkts - p.out_pkts) / dt;
            const double mbps = double(cur.out_bytes - p.out_bytes) * 8.0 / dt / 1e6;
            const uint64_t qn = cur.queue_n - p.queue_n;
            const double qms = qn ? double(cur.queue_us - p.queue_us) / double(qn) / 1000.0 : 0.0;
            const uint64_t dl = cur.drop_loss - p.drop_loss, dq = cur.drop_queue - p.drop_queue;
            const double ts = double(now - t0) / 1e6;
            if (!quiet)
                std::printf("%6.1f %-4s %8.0f %8.0f %9.2f %10llu %10llu %9.2f\n", ts, d == 0 ? "up" : "down", in_pps,
                            out_pps, mbps, static_cast<unsigned long long>(dl), static_cast<unsigned long long>(dq),
                            qms);
            if (csv)
                std::fprintf(csv, "%.3f,%s,%.1f,%.1f,%.3f,%llu,%llu,%.3f\n", ts, d == 0 ? "up" : "down", in_pps,
                             out_pps, mbps, static_cast<unsigned long long>(dl), static_cast<unsigned long long>(dq),
                             qms);
            prev[d] = cur;
        }
        if (csv) std::fflush(csv);
    }
    proxy.stop();
    if (csv) std::fclose(csv);

    std::printf("\nTotals (client %s):\n", proxy.client().c_str());
    for (int d = 0; d < 2; ++d) {
        const DirSnapshot s = DirSnapshot::take(proxy.stats(static_cast<Proxy::Dir>(d)));
        std::printf("  %-4s in %llu pkts, out %llu pkts, dropped %llu (loss) + %llu (queue/no client)\n",
                    d == 0 ? "up" : "down", static_cast<unsigned long long>(s.in_pkts),
                    static_cast<unsigned long long>(s.out_pkts), static_cast<unsigned long long>(s.drop_loss),
                    static_cast<unsigned long long>(s.drop_queue));
    }
    return 0;
}

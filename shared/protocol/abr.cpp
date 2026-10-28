#include "hikari/abr.h"

#include <algorithm>

namespace hikari {

namespace {
// Steps relative to the native mode. Frame rate drops only as a last resort.
struct Step {
    uint16_t h;
    uint16_t fps;
    uint32_t min_kbps;  // step down when bitrate stays below this
};
// Full resolution holds down to a lower bitrate than it used to: a softer
// 1080p picture still looks better than an upscaled 900p one.
constexpr Step kLadder[] = {
    {1080, 60, 5000},
    {900, 60, 3500},
    {720, 60, 2000},
    {720, 30, 0},
};
}  // namespace

AbrConfig AdaptiveBitrate::preset_config(proto::Preset p, uint32_t client_max_kbps)
{
    AbrConfig c;
    switch (p) {
    case proto::Preset::MinimumLatency:
        c.start_kbps = 12000;
        c.max_kbps = 20000;
        c.queue_delay_limit_us = 6000;
        break;
    case proto::Preset::Balanced:
        c.start_kbps = 16000;
        c.max_kbps = 30000;
        c.queue_delay_limit_us = 12000;
        break;
    case proto::Preset::Quality:
        c.start_kbps = 24000;
        c.max_kbps = 50000;
        c.queue_delay_limit_us = 25000;
        break;
    }
    if (client_max_kbps) c.max_kbps = std::min(c.max_kbps, client_max_kbps);
    c.start_kbps = std::min(c.start_kbps, c.max_kbps);
    return c;
}

void AdaptiveBitrate::reset(const AbrConfig& cfg, VideoMode native)
{
    cfg_ = cfg;
    native_ = native;
    bitrate_ = cfg.start_kbps;
    loss_ = 0;
    qdelay_ = 0;
    step_ = 0;
    fec_ = 0;
    last_decrease_ = last_increase_ = clean_since_ = low_since_ = high_since_ = lossy_since_ = fec_clean_since_ = 0;
    last_reported_kbps_ = static_cast<uint32_t>(bitrate_);
}

int AdaptiveBitrate::max_step() const
{
    int n = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0])) - 1;
    if (!cfg_.allow_fps_drop) --n;
    if (!cfg_.allow_resolution_change) n = 0;
    return n;
}

VideoMode AdaptiveBitrate::ladder(int step) const
{
    VideoMode m = native_;
    if (step <= 0) return m;
    const Step& s = kLadder[step];
    if (s.h < native_.height) {
        m.height = s.h;
        m.width = static_cast<uint16_t>((uint32_t(native_.width) * s.h / native_.height + 15) & ~15u);
    }
    m.fps = std::min<uint16_t>(native_.fps, s.fps);
    return m;
}

AbrDecision AdaptiveBitrate::current() const
{
    AbrDecision d;
    d.bitrate_kbps = static_cast<uint32_t>(bitrate_);
    d.fec_group = fec_;
    d.mode = ladder(step_);
    return d;
}

AbrDecision AdaptiveBitrate::on_report(const proto::ReceiverReport& r, uint64_t now)
{
    const VideoMode before = ladder(step_);

    // Smooth the signals: fast attack, slower release.
    const double expected = double(r.packets_received) + double(r.packets_lost);
    const double loss = expected > 0 ? double(r.packets_lost) / expected : 0.0;
    loss_ = loss > loss_ ? 0.5 * loss_ + 0.5 * loss : 0.85 * loss_ + 0.15 * loss;
    const double qd = std::max(0, r.queue_delay_us);
    qdelay_ = qd > qdelay_ ? 0.4 * qdelay_ + 0.6 * qd : 0.8 * qdelay_ + 0.2 * qd;
    const double recv_kbps = r.interval_us ? double(r.bytes_received) * 8.0 / double(r.interval_us) * 1000.0 : 0;

    const bool congested = qdelay_ > cfg_.queue_delay_limit_us || r.decode_queue > 2;
    const bool heavy_loss = loss_ > 0.08;

    if ((congested || heavy_loss) && now - last_decrease_ > 250'000) {
        // Don't go below what actually got through, minus a margin.
        double target = bitrate_ * (heavy_loss ? 0.7 : 0.85);
        if (recv_kbps > 0 && congested) target = std::min(target, recv_kbps * 0.9);
        bitrate_ = std::max<double>(cfg_.min_kbps, target);
        last_decrease_ = now;
        clean_since_ = 0;
    } else if (!congested && loss_ < 0.02) {
        if (!clean_since_) clean_since_ = now;
        // Probe upward gently after one clean second, at most every 500 ms.
        if (now - clean_since_ > 1'000'000 && now - last_increase_ > 500'000 && now - last_decrease_ > 2'000'000) {
            bitrate_ = std::min<double>(cfg_.max_kbps, bitrate_ * 1.08 + 300);
            last_increase_ = now;
        }
    } else {
        clean_since_ = 0;
    }

    // FEC follows residual random loss (loss without queueing = not congestion).
    uint8_t want_fec = 0;
    if (loss_ > 0.04)
        want_fec = 4;
    else if (loss_ > 0.015)
        want_fec = 6;
    else if (loss_ > 0.003)
        want_fec = 10;
    if (want_fec && (fec_ == 0 || want_fec < fec_)) {
        fec_ = want_fec;
        fec_clean_since_ = 0;
    } else if (!want_fec && fec_) {
        if (!fec_clean_since_) fec_clean_since_ = now;
        if (now - fec_clean_since_ > 5'000'000) fec_ = 0;  // clean for 5 s
    } else {
        fec_clean_since_ = 0;
    }

    // Resolution ladder with hysteresis.
    if (step_ < max_step() && bitrate_ < kLadder[step_].min_kbps) {
        if (!low_since_) low_since_ = now;
        if (now - low_since_ > 3'000'000) {
            ++step_;
            low_since_ = 0;
        }
    } else {
        low_since_ = 0;
    }
    if (step_ > 0 && bitrate_ > kLadder[step_ - 1].min_kbps * 1.4) {
        if (!high_since_) high_since_ = now;
        if (now - high_since_ > 6'000'000) {
            --step_;
            high_since_ = 0;
        }
    } else {
        high_since_ = 0;
    }

    AbrDecision d = current();
    d.mode_changed = d.mode != before;
    d.bitrate_changed = d.bitrate_kbps != last_reported_kbps_;
    last_reported_kbps_ = d.bitrate_kbps;
    return d;
}

}  // namespace hikari

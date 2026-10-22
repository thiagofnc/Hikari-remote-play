#pragma once
// Adaptive bitrate, FEC and resolution controller (runs on the host, fed by
// the client's receiver reports every ~100 ms).
//
// Signals, in order of trust:
//   1. queueing delay - one-way delay above its recent minimum. Rises
//      *before* loss on a bottleneck, so reacting to it keeps latency flat.
//   2. packet loss    - after FEC; random Wi-Fi loss is answered with more
//                       parity, congestion loss (with delay) with less bitrate.
//   3. client decode queue - client can't keep up -> lower bitrate.
//
// Decreases are multiplicative and fast; increases are slow and only after a
// clean hold period. Frame rate is protected: resolution steps down before
// 60 fps is ever given up.

#include <cstdint>

#include "hikari/net_protocol.h"

namespace hikari {

struct AbrConfig {
    uint32_t min_kbps = 2000;
    uint32_t max_kbps = 20000;
    uint32_t start_kbps = 12000;
    double queue_delay_limit_us = 12000;  // above this we are filling a buffer somewhere
    bool allow_resolution_change = true;
    bool allow_fps_drop = true;
};

struct VideoMode {
    uint16_t width = 1920, height = 1080, fps = 60;
    bool operator==(const VideoMode& o) const { return width == o.width && height == o.height && fps == o.fps; }
    bool operator!=(const VideoMode& o) const { return !(*this == o); }
};

struct AbrDecision {
    uint32_t bitrate_kbps = 0;
    uint8_t fec_group = 0;  // 0 = off
    VideoMode mode;
    bool mode_changed = false;
    bool bitrate_changed = false;
};

class AdaptiveBitrate {
public:
    void reset(const AbrConfig& cfg, VideoMode native);
    AbrDecision on_report(const proto::ReceiverReport& r, uint64_t now_us);
    AbrDecision current() const;

    double smoothed_loss() const { return loss_; }
    double smoothed_queue_delay_us() const { return qdelay_; }

    static AbrConfig preset_config(proto::Preset p, uint32_t client_max_kbps);

private:
    VideoMode ladder(int step) const;
    int max_step() const;

    AbrConfig cfg_;
    VideoMode native_;
    double bitrate_ = 12000;
    double loss_ = 0;
    double qdelay_ = 0;
    int step_ = 0;  // 0 = native, higher = smaller
    uint8_t fec_ = 0;
    uint64_t last_decrease_ = 0;
    uint64_t last_increase_ = 0;
    uint64_t clean_since_ = 0;
    uint64_t low_since_ = 0;
    uint64_t high_since_ = 0;
    uint64_t lossy_since_ = 0;
    uint64_t fec_clean_since_ = 0;
    uint32_t last_reported_kbps_ = 0;
};

}  // namespace hikari

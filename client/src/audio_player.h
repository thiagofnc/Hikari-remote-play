#pragma once
// Low-latency audio playback.
//
//  * WASAPI shared mode via IAudioClient3 at the engine's minimum period when
//    available (often 2.67-3 ms instead of 10 ms).
//  * A jitter buffer whose target depends on the preset. Clock drift between
//    the host's capture clock and our DAC is absorbed by inaudible sample
//    drop/repeat (<=0.2%), never by letting the buffer grow.
//  * Opus packet-loss concealment fills gaps; PCM gaps become silence.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "hikari/net_protocol.h"

struct OpusDecoder;

namespace hikari {

class AudioPlayer {
public:
    ~AudioPlayer();

    bool start(uint32_t target_ms);
    void stop();
    void set_target_ms(uint32_t ms) { target_ms_ = ms; }
    void set_muted(bool m) { muted_ = m; }
    void set_volume(float v) { volume_ = v; }

    // Called from the network thread for every audio packet.
    void push(const proto::AudioPacket& p);

    uint16_t buffered_ms() const { return buffered_ms_.load(); }
    uint64_t underruns() const { return underruns_.load(); }

private:
    void run();
    void write_samples(const int16_t* s, size_t frames);
    size_t read_samples(int16_t* out, size_t frames);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> target_ms_{30};
    std::atomic<bool> muted_{false};
    std::atomic<float> volume_{1.0f};

    std::mutex mu_;
    std::vector<int16_t> ring_;  // interleaved stereo, 1 s
    size_t head_ = 0, count_ = 0;
    bool priming_ = true;
    uint32_t last_seq_ = 0;
    bool have_seq_ = false;
    OpusDecoder* opus_ = nullptr;
    int drift_counter_ = 0;

    std::atomic<uint16_t> buffered_ms_{0};
    std::atomic<uint64_t> underruns_{0};
};

}  // namespace hikari

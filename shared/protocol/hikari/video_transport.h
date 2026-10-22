#pragma once
// Video framing: encoded frame <-> UDP fragments, with optional XOR parity.
//
// FEC model: every group of K consecutive data fragments gets one parity
// fragment (XOR of the group). Any single loss inside a group is repaired
// without a round trip. K is chosen by the host from measured loss (off when
// the path is clean, because parity costs 1/K extra bandwidth).

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "hikari/net_protocol.h"

namespace hikari {

class VideoPacketizer {
public:
    using Emit = std::function<void(const uint8_t* datagram, size_t len)>;

    // Emits data fragments in order; a parity fragment follows each group.
    void packetize(uint32_t session, proto::VideoFragment meta, const uint8_t* data, size_t size,
                   const Emit& emit);

    static uint16_t fragment_count(size_t size)
    {
        return static_cast<uint16_t>((size + proto::kVideoPayload - 1) / proto::kVideoPayload);
    }

private:
    uint8_t datagram_[proto::kMaxDatagram];
    uint8_t parity_[proto::kVideoPayload];
};

struct AssembledFrame {
    uint32_t frame_id = 0;
    uint8_t flags = 0;
    uint64_t capture_us = 0;      // host clock
    uint16_t encode_us = 0;
    uint16_t host_us = 0;
    uint64_t first_packet_us = 0; // client clock
    uint64_t complete_us = 0;     // client clock
    bool fec_recovered = false;
    bool after_loss = false;      // a frame before this one was abandoned
    std::vector<uint8_t> data;
};

struct AssemblerStats {
    uint64_t packets_received = 0;       // data + parity
    uint64_t data_packets_received = 0;  // data fragments that arrived on the wire
    uint64_t packets_expected = 0;       // data fragments of completed/abandoned frames
    uint64_t bytes_received = 0;
    uint64_t frames_complete = 0;
    uint64_t frames_dropped = 0;
    uint64_t frames_fec_recovered = 0;
    uint64_t late_packets = 0;
    uint64_t duplicate_packets = 0;
};

class FrameAssembler {
public:
    // reorder_window_us: how long an incomplete older frame may hold back a
    // newer complete one before it is abandoned.
    explicit FrameAssembler(uint64_t reorder_window_us = 3000);

    void set_reorder_window(uint64_t us) { reorder_window_us_ = us; }

    // Feeds one datagram (after the common header). Parity packets use
    // is_parity = true.
    void push(const proto::VideoFragment& frag, bool is_parity, uint64_t now_us);

    // Returns the next frame ready for decode, in frame order, or nullptr.
    // Abandons stale incomplete frames whose deadline passed.
    std::unique_ptr<AssembledFrame> pop(uint64_t now_us);

    // Return a frame's buffer to the pool once decoded.
    void recycle(std::unique_ptr<AssembledFrame> f);

    const AssemblerStats& stats() const { return stats_; }
    uint32_t highest_frame() const { return highest_seen_; }
    // True once since the last call if any frame was abandoned.
    bool take_loss_event()
    {
        const bool v = loss_event_;
        loss_event_ = false;
        return v;
    }
    void reset();

private:
    static constexpr int kSlots = 16;
    struct Slot {
        bool active = false;
        uint32_t frame_id = 0;
        uint16_t data_count = 0;
        uint16_t received = 0;
        uint16_t parity_received = 0;
        uint8_t fec_group = 0;
        uint8_t flags = 0;
        uint32_t frame_size = 0;
        uint64_t capture_us = 0;
        uint16_t encode_us = 0;
        uint16_t host_us = 0;
        uint64_t first_us = 0;
        uint64_t last_us = 0;
        bool fec_used = false;
        std::vector<uint8_t> have;         // per data fragment
        std::vector<uint8_t> have_parity;  // per group
        std::vector<uint8_t> data;
        std::vector<uint8_t> parity;       // groups * kVideoPayload
        std::unique_ptr<AssembledFrame> out;
    };

    Slot* slot_for(uint32_t frame_id) { return &slots_[frame_id % kSlots]; }
    void begin(Slot& s, const proto::VideoFragment& f, uint64_t now);
    void try_recover(Slot& s, uint16_t group);
    bool complete(const Slot& s) const { return s.active && s.received == s.data_count; }
    void abandon(Slot& s);
    std::unique_ptr<AssembledFrame> take(Slot& s, uint64_t now);
    std::unique_ptr<AssembledFrame> alloc();

    Slot slots_[kSlots];
    uint64_t reorder_window_us_;
    bool started_ = false;
    uint32_t next_frame_ = 0;  // next frame id to deliver
    uint32_t highest_seen_ = 0;
    bool loss_event_ = false;
    bool pending_after_loss_ = false;
    AssemblerStats stats_;
    std::vector<std::unique_ptr<AssembledFrame>> pool_;
};

}  // namespace hikari

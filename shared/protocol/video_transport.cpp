#include "hikari/video_transport.h"

#include <algorithm>

#include "hk_link.h"

namespace hikari {

using proto::kVideoPayload;

namespace {
inline void xor_into(uint8_t* dst, const uint8_t* src, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t a, b;
        std::memcpy(&a, dst + i, 8);
        std::memcpy(&b, src + i, 8);
        a ^= b;
        std::memcpy(dst + i, &a, 8);
    }
    for (; i < n; ++i) dst[i] ^= src[i];
}

inline size_t frag_len(size_t frame_size, size_t index)
{
    const size_t off = index * kVideoPayload;
    return std::min(kVideoPayload, frame_size - off);
}
}  // namespace

void VideoPacketizer::packetize(uint32_t session, proto::VideoFragment meta, const uint8_t* data, size_t size,
                                const Emit& emit)
{
    if (size == 0) return;
    const uint16_t n = fragment_count(size);
    const int k = meta.fec_group;
    meta.data_count = n;
    meta.frame_size = static_cast<uint32_t>(size);

    for (uint16_t i = 0; i < n; ++i) {
        const size_t off = size_t(i) * kVideoPayload;
        const size_t len = frag_len(size, i);
        if (k) {
            if (i % k == 0) std::memset(parity_, 0, sizeof(parity_));
            xor_into(parity_, data + off, len);
        }

        ByteWriter w(datagram_, sizeof(datagram_));
        proto::write_header(w, proto::PacketType::Video, session);
        meta.index = i;
        proto::write_video_header(w, meta);
        w.bytes(data + off, len);
        emit(datagram_, w.size());

        if (k && (i % k == k - 1 || i == n - 1)) {
            const uint16_t group = static_cast<uint16_t>(i / k);
            const size_t glen = frag_len(size, size_t(group) * k);  // first fragment of group is the longest
            ByteWriter pw(datagram_, sizeof(datagram_));
            proto::write_header(pw, proto::PacketType::VideoParity, session);
            meta.index = group;
            proto::write_video_header(pw, meta);
            pw.bytes(parity_, glen);
            emit(datagram_, pw.size());
        }
    }
}

FrameAssembler::FrameAssembler(uint64_t reorder_window_us) : reorder_window_us_(reorder_window_us) {}

void FrameAssembler::reset()
{
    for (auto& s : slots_) s.active = false;
    started_ = false;
    pending_after_loss_ = false;
    loss_event_ = false;
}

std::unique_ptr<AssembledFrame> FrameAssembler::alloc()
{
    if (!pool_.empty()) {
        auto f = std::move(pool_.back());
        pool_.pop_back();
        return f;
    }
    return std::make_unique<AssembledFrame>();
}

void FrameAssembler::recycle(std::unique_ptr<AssembledFrame> f)
{
    if (f && pool_.size() < 2 * kSlots) pool_.push_back(std::move(f));
}

void FrameAssembler::begin(Slot& s, const proto::VideoFragment& f, uint64_t now)
{
    s.active = true;
    s.frame_id = f.frame_id;
    s.data_count = f.data_count;
    s.received = 0;
    s.parity_received = 0;
    s.fec_group = f.fec_group;
    s.flags = f.flags;
    s.frame_size = f.frame_size;
    s.capture_us = f.capture_us;
    s.encode_us = f.encode_us;
    s.host_us = f.host_us;
    s.first_us = now;
    s.last_us = now;
    s.fec_used = false;
    s.have.assign(s.data_count, 0);
    const size_t groups = s.fec_group ? (s.data_count + s.fec_group - 1) / s.fec_group : 0;
    s.have_parity.assign(groups, 0);
    s.data.resize(s.frame_size);
    s.parity.resize(groups * kVideoPayload);
}

void FrameAssembler::try_recover(Slot& s, uint16_t group)
{
    if (!s.fec_group || group >= s.have_parity.size() || !s.have_parity[group]) return;
    const size_t first = size_t(group) * s.fec_group;
    const size_t last = std::min<size_t>(first + s.fec_group, s.data_count);
    size_t missing = SIZE_MAX;
    int n_missing = 0;
    for (size_t i = first; i < last; ++i) {
        if (!s.have[i]) {
            missing = i;
            if (++n_missing > 1) return;
        }
    }
    if (n_missing != 1) return;
    const size_t mlen = frag_len(s.frame_size, missing);
    uint8_t* dst = s.data.data() + missing * kVideoPayload;
    std::memcpy(dst, s.parity.data() + size_t(group) * kVideoPayload, mlen);
    for (size_t i = first; i < last; ++i) {
        if (i == missing) continue;
        xor_into(dst, s.data.data() + i * kVideoPayload, std::min(mlen, frag_len(s.frame_size, i)));
    }
    s.have[missing] = 1;
    ++s.received;
    s.fec_used = true;
}

void FrameAssembler::push(const proto::VideoFragment& f, bool is_parity, uint64_t now)
{
    if (f.data_count == 0 || f.frame_size == 0 ||
        f.frame_size > size_t(f.data_count) * kVideoPayload ||
        f.frame_size <= size_t(f.data_count - 1) * kVideoPayload)
        return;  // inconsistent header

    if (!started_) {
        started_ = true;
        next_frame_ = f.frame_id;
        highest_seen_ = f.frame_id;
    }
    if (hk_seq_newer(next_frame_, f.frame_id)) {
        ++stats_.late_packets;
        return;
    }
    // A frame far ahead of what we are waiting for: everything older is
    // hopeless, skip forward instead of stalling.
    while (static_cast<int32_t>(f.frame_id - next_frame_) >= kSlots) {
        Slot& old = *slot_for(next_frame_);
        if (old.active && old.frame_id == next_frame_)
            abandon(old);
        else {
            ++stats_.frames_dropped;
            loss_event_ = true;
            pending_after_loss_ = true;
        }
        ++next_frame_;
    }

    Slot& s = *slot_for(f.frame_id);
    if (!s.active || s.frame_id != f.frame_id) {
        if (s.active) abandon(s);
        begin(s, f, now);
    }
    if (f.data_count != s.data_count || f.frame_size != s.frame_size) return;
    if (hk_seq_newer(f.frame_id, highest_seen_)) highest_seen_ = f.frame_id;

    ++stats_.packets_received;
    stats_.bytes_received += f.payload_len + proto::kHeaderSize + proto::kVideoHeaderSize;
    s.last_us = now;

    if (is_parity) {
        const uint16_t g = f.index;
        if (g >= s.have_parity.size()) return;
        if (s.have_parity[g]) {
            ++stats_.duplicate_packets;
            return;
        }
        const size_t glen = frag_len(s.frame_size, size_t(g) * s.fec_group);
        if (f.payload_len != glen) return;
        std::memcpy(s.parity.data() + size_t(g) * kVideoPayload, f.payload, glen);
        s.have_parity[g] = 1;
        ++s.parity_received;
        try_recover(s, g);
        return;
    }

    const uint16_t i = f.index;
    if (i >= s.data_count) return;
    if (s.have[i]) {
        ++stats_.duplicate_packets;
        return;
    }
    if (f.payload_len != frag_len(s.frame_size, i)) return;
    std::memcpy(s.data.data() + size_t(i) * kVideoPayload, f.payload, f.payload_len);
    s.have[i] = 1;
    ++s.received;
    ++stats_.data_packets_received;
    if (s.fec_group) try_recover(s, static_cast<uint16_t>(i / s.fec_group));
}

void FrameAssembler::abandon(Slot& s)
{
    ++stats_.frames_dropped;
    stats_.packets_expected += s.data_count;
    s.active = false;
    loss_event_ = true;
    pending_after_loss_ = true;
}

std::unique_ptr<AssembledFrame> FrameAssembler::take(Slot& s, uint64_t now)
{
    auto out = alloc();
    out->frame_id = s.frame_id;
    out->flags = s.flags;
    out->capture_us = s.capture_us;
    out->encode_us = s.encode_us;
    out->host_us = s.host_us;
    out->first_packet_us = s.first_us;
    out->complete_us = now;
    out->fec_recovered = s.fec_used;
    out->after_loss = pending_after_loss_;
    pending_after_loss_ = false;
    out->data.swap(s.data);  // zero-copy hand-off; slot inherits the pooled buffer
    out->data.resize(s.frame_size);

    ++stats_.frames_complete;
    stats_.packets_expected += s.data_count;
    if (s.fec_used) ++stats_.frames_fec_recovered;
    s.active = false;
    ++next_frame_;
    return out;
}

std::unique_ptr<AssembledFrame> FrameAssembler::pop(uint64_t now)
{
    constexpr uint64_t kStallDeadline = 60'000;  // tail of a stream lost entirely
    for (;;) {
        if (!started_) return nullptr;
        Slot& s = *slot_for(next_frame_);
        const bool here = s.active && s.frame_id == next_frame_;
        if (here && complete(s)) return take(s, now);

        // Is anything newer in flight?
        bool newer = false;
        uint64_t newer_first = UINT64_MAX;
        for (auto& o : slots_) {
            if (o.active && hk_seq_newer(o.frame_id, next_frame_)) {
                newer = true;
                newer_first = std::min(newer_first, o.first_us);
            }
        }
        bool give_up = false;
        if (here) {
            const uint64_t idle = now - s.last_us;
            give_up = (newer && idle > reorder_window_us_) || idle > kStallDeadline;
        } else if (newer) {
            give_up = now - newer_first > reorder_window_us_;  // whole frame never arrived
        }
        if (!give_up) return nullptr;

        if (here)
            abandon(s);
        else {
            ++stats_.frames_dropped;
            loss_event_ = true;
            pending_after_loss_ = true;
        }
        ++next_frame_;
    }
}

}  // namespace hikari

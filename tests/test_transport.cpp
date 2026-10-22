// Video packetization, FEC repair, loss deadlines and reordering.

#include <algorithm>
#include <random>
#include <vector>

#include "hikari/video_transport.h"
#include "test_framework.h"

using namespace hikari;

namespace {

struct Datagram {
    std::vector<uint8_t> bytes;
};

std::vector<Datagram> make_frame(uint32_t id, size_t size, uint8_t fec, std::vector<uint8_t>& payload)
{
    std::mt19937 rng(id);
    payload.resize(size);
    for (auto& b : payload) b = static_cast<uint8_t>(rng());
    VideoPacketizer pk;
    proto::VideoFragment meta;
    meta.frame_id = id;
    meta.fec_group = fec;
    meta.capture_us = 1000 + id;
    std::vector<Datagram> out;
    pk.packetize(9, meta, payload.data(), payload.size(),
                 [&](const uint8_t* d, size_t n) { out.push_back({std::vector<uint8_t>(d, d + n)}); });
    return out;
}

void feed(FrameAssembler& a, const Datagram& d, uint64_t now)
{
    ByteReader r(d.bytes.data(), d.bytes.size());
    proto::Header h;
    CHECK(proto::read_header(r, h));
    proto::VideoFragment f;
    CHECK(proto::read_video_header(r, f));
    a.push(f, h.type == proto::PacketType::VideoParity, now);
}

}  // namespace

TEST(transport_clean_path)
{
    FrameAssembler a;
    std::vector<uint8_t> want;
    auto pkts = make_frame(1, 50'000, 0, want);
    CHECK_EQ(pkts.size(), VideoPacketizer::fragment_count(50'000));
    for (auto& p : pkts) feed(a, p, 10);
    auto f = a.pop(10);
    CHECK(f != nullptr);
    if (f) {
        CHECK(f->data == want);
        CHECK_EQ(f->capture_us, 1001u);
        CHECK(!f->fec_recovered);
    }
    CHECK(a.pop(11) == nullptr);
}

TEST(transport_fec_repairs_one_loss_per_group)
{
    for (size_t size : {1u, 1174u, 1175u, 9000u, 100'000u}) {
        FrameAssembler a;
        std::vector<uint8_t> want;
        auto pkts = make_frame(5, size, 4, want);
        // Drop exactly one data fragment in every group: the last one of each
        // group (exercises the short tail fragment too).
        const uint16_t n = VideoPacketizer::fragment_count(size);
        std::vector<Datagram> kept;
        for (auto& p : pkts) {
            ByteReader r(p.bytes.data(), p.bytes.size());
            proto::Header h;
            proto::read_header(r, h);
            proto::VideoFragment f;
            proto::read_video_header(r, f);
            const bool parity = h.type == proto::PacketType::VideoParity;
            if (!parity && (f.index % 4 == 3 || f.index == n - 1)) continue;
            kept.push_back(p);
        }
        for (auto& p : kept) feed(a, p, 10);
        auto f = a.pop(10);
        CHECK(f != nullptr);
        if (f) {
            CHECK(f->data == want);
            CHECK(f->fec_recovered);
        }
    }
}

TEST(transport_two_losses_in_group_abandon_after_deadline)
{
    FrameAssembler a(2000);
    std::vector<uint8_t> w1, w2;
    auto f1 = make_frame(1, 20'000, 4, w1);
    auto f2 = make_frame(2, 20'000, 4, w2);
    feed(a, f1[0], 0);  // lose fragments 1 and 2 of group 0 in frame 1
    for (size_t i = 3; i < f1.size(); ++i) feed(a, f1[i], 0);
    for (auto& p : f2) feed(a, p, 100);
    CHECK(a.pop(500) == nullptr);  // still inside reorder window
    auto f = a.pop(3000);
    CHECK(f != nullptr);
    if (f) {
        CHECK_EQ(f->frame_id, 2u);
        CHECK(f->after_loss);
    }
    CHECK(a.take_loss_event());
    CHECK_EQ(a.stats().frames_dropped, 1u);
}

TEST(transport_reordered_packets_within_window)
{
    FrameAssembler a(5000);
    std::vector<uint8_t> w1, w2;
    auto f1 = make_frame(1, 30'000, 0, w1);
    auto f2 = make_frame(2, 30'000, 0, w2);
    // Frame 2 fully arrives before the tail of frame 1.
    for (size_t i = 0; i + 1 < f1.size(); ++i) feed(a, f1[i], 0);
    for (auto& p : f2) feed(a, p, 100);
    CHECK(a.pop(200) == nullptr);
    feed(a, f1.back(), 300);
    auto x = a.pop(300);
    auto y = a.pop(300);
    CHECK(x && x->frame_id == 1);
    CHECK(y && y->frame_id == 2);
    CHECK(!a.take_loss_event());
}

TEST(transport_whole_frame_missing)
{
    FrameAssembler a(1000);
    std::vector<uint8_t> w;
    for (auto& p : make_frame(1, 5000, 0, w)) feed(a, p, 0);
    CHECK(a.pop(0) != nullptr);
    // Frame 2 lost entirely, frame 3 arrives.
    for (auto& p : make_frame(3, 5000, 0, w)) feed(a, p, 100);
    CHECK(a.pop(500) == nullptr);
    auto f = a.pop(2000);
    CHECK(f && f->frame_id == 3);
    CHECK(f && f->after_loss);
}

TEST(transport_random_loss_statistics)
{
    std::mt19937 rng(99);
    FrameAssembler a(2000);
    uint64_t now = 0;
    int delivered = 0;
    for (uint32_t id = 1; id <= 600; ++id) {
        std::vector<uint8_t> w;
        auto pkts = make_frame(id, 15'000 + rng() % 20'000, 8, w);
        for (auto& p : pkts)
            if (rng() % 100 >= 1) feed(a, p, now);  // 1% loss
        now += 16'667;
        while (auto f = a.pop(now)) {
            ++delivered;
            a.recycle(std::move(f));
        }
    }
    // With K=8 parity, 1% random loss should be almost entirely repaired.
    CHECK(delivered >= 590);
    CHECK(a.stats().frames_fec_recovered > 50);
}

// Host <-> ESP32 wired link: framing, codecs, robustness.

#include <cstring>
#include <random>

#include "hikari/pad_convert.h"
#include "hk_link.h"
#include "test_framework.h"

namespace {

bool decode_stream(hk_link_decoder& d, const uint8_t* wire, size_t n, hk_link_frame& f, uint8_t* scratch)
{
    bool got = false;
    for (size_t i = 0; i < n; ++i)
        if (hk_link_decoder_push(&d, wire[i], scratch, HK_LINK_MAX_FRAME, &f)) got = true;
    return got;
}

hk_pad_state sample_state()
{
    hk_pad_state s{};
    s.session_id = 0xA1B2C3D4;
    s.seq = 42;
    s.host_time_us = 123456;
    s.buttons = HK_BTN_A | HK_BTN_ZR | HK_BTN_DPAD_UP;
    s.lx = 0;
    s.ly = 65535;
    s.rx = 32768;
    s.ry = 1;
    s.lt = 10;
    s.rt = 255;
    return s;
}

}  // namespace

TEST(crc16_known_vector)
{
    const char* msg = "123456789";
    CHECK_EQ(hk_crc16(reinterpret_cast<const uint8_t*>(msg), 9), 0x29B1);
}

TEST(cobs_roundtrip_with_zeros)
{
    std::mt19937 rng(7);
    for (int len = 1; len < 600; len += 7) {
        std::vector<uint8_t> in(len), enc(len + len / 254 + 2), dec(len);
        for (auto& b : in) b = static_cast<uint8_t>(rng() % 3 == 0 ? 0 : rng());
        const size_t n = hk_cobs_encode(in.data(), in.size(), enc.data());
        for (size_t i = 0; i < n; ++i) CHECK(enc[i] != 0);
        const size_t m = hk_cobs_decode(enc.data(), n, dec.data(), dec.size());
        CHECK_EQ(m, in.size());
        CHECK(std::memcmp(in.data(), dec.data(), in.size()) == 0);
    }
}

TEST(pad_frame_roundtrip)
{
    hk_pad_state s = sample_state();
    uint8_t payload[HK_PAD_PAYLOAD_IMU], wire[HK_LINK_MAX_WIRE], scratch[HK_LINK_MAX_FRAME];
    const size_t pl = hk_pad_encode(&s, payload);
    CHECK_EQ(pl, HK_PAD_PAYLOAD_BASE);
    const size_t wl = hk_link_build(HK_LINK_PAD_STATE, payload, pl, wire, sizeof(wire));
    CHECK(wl > 0 && wl <= HK_LINK_MAX_WIRE);
    hk_link_decoder d;
    hk_link_decoder_init(&d);
    hk_link_frame f{};
    CHECK(decode_stream(d, wire, wl, f, scratch));
    CHECK_EQ(f.type, HK_LINK_PAD_STATE);
    hk_pad_state out{};
    CHECK(hk_pad_decode(f.payload, f.len, &out));
    CHECK_EQ(out.seq, s.seq);
    CHECK_EQ(out.buttons, s.buttons);
    CHECK_EQ(out.ly, s.ly);
    CHECK_EQ(out.rt, s.rt);
}

TEST(pad_imu_roundtrip)
{
    hk_pad_state s = sample_state();
    s.flags = HK_PAD_FLAG_HAS_IMU;
    s.gyro[0] = -1234;
    s.accel[2] = 4096;
    uint8_t payload[HK_PAD_PAYLOAD_IMU];
    CHECK_EQ(hk_pad_encode(&s, payload), HK_PAD_PAYLOAD_IMU);
    hk_pad_state out{};
    CHECK(hk_pad_decode(payload, HK_PAD_PAYLOAD_IMU, &out));
    CHECK_EQ(out.gyro[0], -1234);
    CHECK_EQ(out.accel[2], 4096);
    // Length/flag mismatch must be rejected.
    CHECK(!hk_pad_decode(payload, HK_PAD_PAYLOAD_BASE, &out));
}

TEST(decoder_rejects_corruption_and_resyncs)
{
    hk_pad_state s = sample_state();
    uint8_t payload[HK_PAD_PAYLOAD_IMU], wire[HK_LINK_MAX_WIRE], scratch[HK_LINK_MAX_FRAME];
    const size_t wl = hk_link_build(HK_LINK_PAD_STATE, payload, hk_pad_encode(&s, payload), wire, sizeof(wire));
    hk_link_decoder d;
    hk_link_decoder_init(&d);
    hk_link_frame f{};

    std::vector<uint8_t> bad(wire, wire + wl);
    bad[5] ^= 0x40;  // flip a bit
    CHECK(!decode_stream(d, bad.data(), bad.size(), f, scratch));
    CHECK_EQ(d.frames_bad, 1u);
    // Boot-loader garbage without delimiters followed by a good frame.
    std::vector<uint8_t> noise(300, 0x55);
    decode_stream(d, noise.data(), noise.size(), f, scratch);
    const uint8_t zero = 0;
    decode_stream(d, &zero, 1, f, scratch);
    CHECK(decode_stream(d, wire, wl, f, scratch));
}

TEST(decoder_fuzz_never_crashes)
{
    std::mt19937 rng(1234);
    hk_link_decoder d;
    hk_link_decoder_init(&d);
    uint8_t scratch[HK_LINK_MAX_FRAME];
    hk_link_frame f{};
    int accepted = 0;
    for (int i = 0; i < 2'000'000; ++i) {
        const uint8_t b = static_cast<uint8_t>(rng() % 16 == 0 ? 0 : rng());
        if (hk_link_decoder_push(&d, b, scratch, sizeof(scratch), &f)) {
            ++accepted;
            CHECK(f.len <= HK_LINK_MAX_FRAME - 3);
            hk_pad_state s;
            hk_pad_decode(f.payload, f.len, &s);
        }
    }
    // CRC16 should let through roughly 1/65536 random frames, not more.
    CHECK(accepted < 20);
}

TEST(sequence_wraparound)
{
    CHECK(hk_seq_newer(1, 0));
    CHECK(!hk_seq_newer(0, 1));
    CHECK(hk_seq_newer(0, 0xFFFFFFFFu));
    CHECK(hk_seq_newer(5, 0xFFFFFFF0u));
    CHECK(!hk_seq_newer(0xFFFFFFF0u, 5));
    CHECK(!hk_seq_newer(7, 7));
}

TEST(dpad_to_hat)
{
    CHECK_EQ(hk_dpad_to_hat(0), 8);
    CHECK_EQ(hk_dpad_to_hat(HK_BTN_DPAD_UP), 0);
    CHECK_EQ(hk_dpad_to_hat(HK_BTN_DPAD_UP | HK_BTN_DPAD_RIGHT), 1);
    CHECK_EQ(hk_dpad_to_hat(HK_BTN_DPAD_DOWN | HK_BTN_DPAD_LEFT), 5);
    CHECK_EQ(hk_dpad_to_hat(HK_BTN_DPAD_LEFT | HK_BTN_DPAD_RIGHT), 8);  // SOCD neutral
}

TEST(telemetry_and_control_roundtrip)
{
    hk_telemetry t{};
    t.uptime_ms = 99;
    t.rx_to_consumed.p99 = 1777;
    t.usb_state = HK_USB_SUSPENDED;
    t.remote_wakeup_enabled = 1;
    uint8_t buf[HK_TELEMETRY_PAYLOAD];
    CHECK_EQ(hk_telemetry_encode(&t, buf), HK_TELEMETRY_PAYLOAD);
    hk_telemetry o{};
    CHECK(hk_telemetry_decode(buf, sizeof(buf), &o));
    CHECK_EQ(o.rx_to_consumed.p99, 1777);
    CHECK_EQ(o.usb_state, HK_USB_SUSPENDED);
    CHECK_EQ(o.remote_wakeup_enabled, 1);

    hk_wake w{HK_WAKE_USB_REMOTE_WAKEUP | HK_WAKE_POWER_BUTTON, 250};
    uint8_t wb[8];
    hk_wake wo{};
    CHECK(hk_wake_decode(wb, hk_wake_encode(&w, wb), &wo));
    CHECK_EQ(wo.methods, w.methods);
    CHECK_EQ(wo.press_ms, 250);

    hk_hello_ack a{HK_LINK_VERSION, HK_PROFILE_HORI_PAD, 0x0102, 77, HK_USB_ACTIVE, 3, 1};
    uint8_t ab[16];
    hk_hello_ack ao{};
    CHECK(hk_hello_ack_decode(ab, hk_hello_ack_encode(&a, ab), &ao));
    CHECK_EQ(ao.session_id, 77u);
    CHECK_EQ(ao.wake_methods, 3);
}

TEST(pad_conversion_axes)
{
    hikari::proto::PadState p;
    p.ly = 32767;  // full up
    p.lx = -32768; // full left
    p.rt = 200;
    const hk_pad_state s = hikari::to_link_state(p, 1, 2, 3, 0);
    CHECK(s.ly < 4);       // up -> 0 in HID convention
    CHECK_EQ(s.lx, 0);
    CHECK_EQ(s.rx, 32768); // centre stays centre
    CHECK_EQ(s.ry, 32768);
    CHECK(s.buttons & HK_BTN_ZR);
    CHECK(!(s.buttons & HK_BTN_ZL));
}

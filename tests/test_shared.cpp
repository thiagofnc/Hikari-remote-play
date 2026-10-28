// Crypto, protocol codecs, histograms, clock sync, queues.

#include <cstring>

#include "hikari/abr.h"
#include "hikari/crypto.h"
#include "hikari/histogram.h"
#include "hikari/invite.h"
#include "hikari/net_protocol.h"
#include "hikari/sync.h"
#include "hikari/tailscale.h"
#include "test_framework.h"

using namespace hikari;

TEST(sha256_vectors)
{
    const auto d = crypto::sha256("abc", 3);
    const uint8_t want[4] = {0xba, 0x78, 0x16, 0xbf};
    CHECK(std::memcmp(d.data(), want, 4) == 0);
    CHECK_EQ(d[31], 0xad);
    // Multi-block input exercises the buffering path.
    std::string m(1000, 'a');
    crypto::Sha256 s;
    s.update(m.data(), 3);
    s.update(m.data() + 3, m.size() - 3);
    CHECK(s.finish() == crypto::sha256(m.data(), m.size()));
}

TEST(hmac_rfc4231_case2)
{
    const char* key = "Jefe";
    const char* msg = "what do ya want for nothing?";
    crypto::Hmac h(reinterpret_cast<const uint8_t*>(key), 4);
    const auto d = h.mac(msg, std::strlen(msg));
    const uint8_t want[8] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e};
    CHECK(std::memcmp(d.data(), want, 8) == 0);
    uint8_t tag[8];
    h.tag(msg, std::strlen(msg), tag);
    CHECK(h.verify_tag(msg, std::strlen(msg), tag));
    tag[0] ^= 1;
    CHECK(!h.verify_tag(msg, std::strlen(msg), tag));
}

TEST(pairing_code_normalization)
{
    const auto code = crypto::generate_pairing_code();
    CHECK_EQ(code.size(), 14u);
    CHECK(crypto::normalize_pairing_code("ab-cd-oi-lu") == "ABCD011V");
    CHECK(crypto::psk_from_code("k7qm-2xpa-9trb") == crypto::psk_from_code("K7QM2XPA9TRB"));
    CHECK(crypto::psk_from_code("K7QM2XPA9TRB") != crypto::psk_from_code("K7QM2XPA9TRC"));
}

TEST(handshake_derivations_bind_both_nonces)
{
    const auto psk = crypto::psk_from_code("AAAA-BBBB-CCCC");
    uint8_t cn[16] = {1}, hn[16] = {2}, hn2[16] = {3};
    CHECK(crypto::auth_mac(psk, cn, hn) != crypto::auth_mac(psk, cn, hn2));
    CHECK(crypto::session_key(psk, cn, hn) != crypto::auth_mac(psk, cn, hn));
    CHECK(crypto::welcome_mac(psk, hn, cn) != crypto::auth_mac(psk, cn, hn));
}

TEST(protocol_input_roundtrip)
{
    proto::Input in;
    in.seq = 77;
    in.client_time_us = 0x1122334455667788ull;
    in.flags = proto::kInputConnected;
    in.pad.buttons = 0xF00F;
    in.pad.lx = -32768;
    in.pad.ry = 32767;
    in.pad.lt = 255;
    uint8_t buf[128];
    ByteWriter w(buf, sizeof(buf));
    proto::write_header(w, proto::PacketType::Input, 5);
    proto::write(w, in);
    CHECK(w.ok());
    ByteReader r(buf, w.size());
    proto::Header h;
    CHECK(proto::read_header(r, h));
    CHECK(h.type == proto::PacketType::Input);
    proto::Input out;
    CHECK(proto::read(r, out));
    CHECK_EQ(out.seq, 77u);
    CHECK(out.client_time_us == in.client_time_us);
    CHECK(out.pad == in.pad);
    CHECK(!out.pad.has_imu);
}

TEST(protocol_truncated_packets_rejected)
{
    proto::HostStats hs;
    std::strcpy(hs.encoder_name, "Intel Quick Sync H.264");
    uint8_t buf[512];
    ByteWriter w(buf, sizeof(buf));
    proto::write(w, hs);
    for (size_t cut = 0; cut < w.size(); cut += 5) {
        ByteReader r(buf, cut);
        proto::HostStats o;
        CHECK(!proto::read(r, o));
    }
    ByteReader r(buf, w.size());
    proto::HostStats o;
    CHECK(proto::read(r, o));
    CHECK(std::strcmp(o.encoder_name, hs.encoder_name) == 0);
}

TEST(protocol_host_status_roundtrip)
{
    proto::HostStatus s;
    s.console = proto::ConsoleState::Asleep;
    s.phase = proto::SessionPhase::Waking;
    s.wake_methods = 5;
    std::strcpy(s.message, "Waking your Switch");
    uint8_t buf[256];
    ByteWriter w(buf, sizeof(buf));
    proto::write(w, s);
    ByteReader r(buf, w.size());
    proto::HostStatus o;
    CHECK(proto::read(r, o));
    CHECK(o.console == proto::ConsoleState::Asleep);
    CHECK(o.phase == proto::SessionPhase::Waking);
    CHECK(std::strcmp(o.message, s.message) == 0);
}

TEST(histogram_percentiles)
{
    Histogram h;
    for (int i = 1; i <= 1000; ++i) h.record(static_cast<uint64_t>(i) * 10);  // 10us..10ms
    const auto s = h.summary();
    CHECK_EQ(s.count, 1000u);
    CHECK(s.p50 > 4700 && s.p50 < 5300);
    CHECK(s.p99 > 9300 && s.p99 <= 10000);
    CHECK_EQ(s.max, 10000u);
    for (uint64_t v : {0ull, 31ull, 32ull, 1000ull, 123456ull, 0xFFFFFFFFull}) {
        const int b = Histogram::bucket_of(v);
        CHECK(b >= 0 && b < Histogram::kBuckets);
        const double rep = static_cast<double>(Histogram::bucket_value(b));
        if (v >= 32) CHECK(rep / static_cast<double>(v) < 1.07 && rep / static_cast<double>(v) > 0.93);
    }
}

TEST(rolling_histogram_ages_out)
{
    RollingHistogram r(1000);
    r.record(50'000, 1);
    CHECK_EQ(r.summary(10).max, 50'000u);
    r.record(10, 1500);
    r.record(10, 2600);
    CHECK(r.summary(2600).max < 50'000u);
}

TEST(clock_sync_prefers_min_rtt)
{
    ClockSync c;
    // Remote clock is 1'000'000 ahead. Second sample has queueing delay.
    c.add_sample(100, 1'000'150, 200);   // rtt 100, symmetric
    c.add_sample(300, 1'000'700, 900);   // rtt 600, asymmetric
    CHECK_EQ(c.offset(), 1'000'000);
    CHECK_EQ(c.min_rtt(), 100u);
}

TEST(mailbox_newest_wins)
{
    Mailbox<int> m;
    CHECK(!m.put(1));
    CHECK(m.put(2));
    CHECK_EQ(m.overwrites(), 1u);
    auto v = m.take();
    CHECK(v && *v == 2);
    CHECK(!m.take());
}

TEST(bounded_queue_capacity)
{
    BoundedQueue<int, 2> q;
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(!q.push(3));
    CHECK_EQ(*q.pop(), 1);
    CHECK(q.push(3));
    int cleared = 0;
    q.clear([&](int) { ++cleared; });
    CHECK_EQ(cleared, 2);
}

TEST(discovery_tailscale_extension_and_legacy_compatibility)
{
    uint8_t bytes[128]{};
    proto::DiscoverReply host{};
    host.port = 49000;
    std::strcpy(host.host_name, "Living room");
    ByteWriter legacy(bytes, sizeof(bytes));
    proto::write(legacy, host);
    const size_t legacy_size = legacy.size();
    CHECK_EQ(legacy_size, 44u);
    proto::DiscoverReply decoded{};
    std::strcpy(decoded.tailscale_ipv4, "100.64.1.2");
    ByteReader old_reader(bytes, legacy_size);
    CHECK(proto::read(old_reader, decoded));
    CHECK_EQ(decoded.tailscale_ipv4[0], 0);

    std::strcpy(host.tailscale_ipv4, "100.101.102.103");
    ByteWriter extended(bytes, sizeof(bytes));
    proto::write(extended, host);
    CHECK(extended.ok());
    ByteReader new_reader(bytes, extended.size());
    CHECK(proto::read(new_reader, decoded));
    CHECK(std::string(decoded.tailscale_ipv4) == "100.101.102.103");
    CHECK_EQ(decoded.port, 49000);
    // Old decoders consume the original prefix and ignore the extension.
    ByteReader prefix(bytes, legacy_size);
    CHECK(proto::read(prefix, decoded));
    for (size_t size = legacy_size + 1; size < extended.size(); ++size) {
        ByteReader truncated(bytes, size);
        CHECK(!proto::read(truncated, decoded));
    }
}

TEST(tailscale_address_and_adapter_validation)
{
    CHECK(is_tailscale_ipv4("100.64.0.0"));
    CHECK(is_tailscale_ipv4("100.127.255.255"));
    for (const auto* ip : {"100.63.255.255", "100.128.0.0", "192.168.1.1", "100.64.1.2:47800", "host", ""})
        CHECK(!is_tailscale_ipv4(ip));
    CHECK(is_tailscale_adapter(L"TAILSCALE", nullptr));
    CHECK(is_tailscale_adapter(L"Ethernet 3", L"Tailscale Tunnel"));
    CHECK(!is_tailscale_adapter(L"Ethernet", L"Network card"));
}

TEST(invite_roundtrip_and_tolerance)
{
    const std::string code = "K7QM-2XPA-9TRB";
    const std::string inv = make_invite("living-room.tail1234.ts.net", proto::kDefaultPort, "100.101.102.103", code);
    CHECK(inv == "hikari://living-room.tail1234.ts.net/K7QM-2XPA-9TRB?ip=100.101.102.103");
    Invite out;
    CHECK(parse_invite(inv, out));
    CHECK(out.host == "living-room.tail1234.ts.net");
    CHECK(out.fallback == "100.101.102.103");
    CHECK(out.code == "K7QM2XPA9TRB");
    // Pasted with quotes, whitespace, no scheme, lower case, custom port.
    CHECK(parse_invite("  \"HIKARI://100.64.0.7:47801/k7qm-2xpa-9trb\"\n", out));
    CHECK(out.host == "100.64.0.7:47801");
    CHECK(out.fallback.empty());
    CHECK(parse_invite("myhost/K7QM2XPA9TRB", out));
    CHECK(make_invite("100.64.0.7", 47801, "100.64.0.7", code) == "hikari://100.64.0.7:47801/K7QM-2XPA-9TRB");
    // Garbage and wrong-length codes are rejected.
    CHECK(!parse_invite("hello world", out));
    CHECK(!parse_invite("hikari://host/ABC", out));
    CHECK(!parse_invite("hikari://bad host!/K7QM2XPA9TRB", out));
}

TEST(tailscale_status_json)
{
    const std::string json = R"({
      "Version": "1.76.1", "BackendState": "Running",
      "Self": {"HostName": "living-room", "DNSName": "living-room.tail1234.ts.net.", "TailscaleIPs": ["100.64.0.7", "fd7a::7"]},
      "Peer": {
        "nodekey:aa": {"HostName": "laptop", "TailscaleIPs": ["100.64.0.9"], "CurAddr": "", "Relay": "nyc", "Online": true},
        "nodekey:bb": {"HostName": "desk", "TailscaleIPs": ["100.64.0.10"], "CurAddr": "203.0.113.5:41641", "Relay": "ord",
                       "Tags": null, "RxBytes": 12345, "Escaped": "a\"b\cé"}
      }
    })";
    CHECK(tailscale_dns_name_from_json(json) == "living-room.tail1234.ts.net");
    const TailscalePath relayed = tailscale_path_from_json(json, "100.64.0.9");
    CHECK(relayed.known && !relayed.direct && relayed.relay == "nyc");
    const TailscalePath direct = tailscale_path_from_json(json, "100.64.0.10");
    CHECK(direct.known && direct.direct && direct.endpoint == "203.0.113.5:41641");
    CHECK(!tailscale_path_from_json(json, "100.64.0.99").known);
    CHECK(tailscale_dns_name_from_json("{\"BackendState\":\"Stopped\",\"Self\":{\"DNSName\":\"x.\"}}").empty());
    CHECK(tailscale_dns_name_from_json("not json").empty());
    CHECK(!tailscale_path_from_json("{\"Peer\":", "100.64.0.9").known);
}

TEST(protocol_probe_and_bye_extensions)
{
    uint8_t buf[512];
    // Hello: the probe flag is an optional trailing byte.
    proto::Hello h;
    h.flags = proto::kHelloProbe;
    ByteWriter w(buf, sizeof(buf));
    proto::write(w, h);
    ByteReader r(buf, w.size());
    proto::Hello ho;
    CHECK(proto::read(r, ho));
    CHECK(ho.flags == proto::kHelloProbe);
    ByteWriter w2(buf, sizeof(buf));
    proto::write(w2, proto::Hello{});
    ByteReader r2(buf, w2.size());
    CHECK(proto::read(r2, ho));
    CHECK(ho.flags == 0);
    // Bye: empty body (older peers) reads as Closed.
    proto::Bye b;
    ByteReader empty(buf, 0);
    CHECK(proto::read(empty, b));
    CHECK(b.reason == proto::ByeReason::Closed);
    ByteWriter w3(buf, sizeof(buf));
    proto::write(w3, proto::Bye{proto::ByeReason::Replaced});
    ByteReader r3(buf, w3.size());
    CHECK(proto::read(r3, b));
    CHECK(b.reason == proto::ByeReason::Replaced);
    // Welcome carries the host's name.
    proto::Welcome wel;
    std::strcpy(wel.host_name, "Living room");
    ByteWriter w4(buf, sizeof(buf));
    proto::write(w4, wel);
    ByteReader r4(buf, w4.size());
    proto::Welcome wo;
    CHECK(proto::read(r4, wo));
    CHECK(std::strcmp(wo.host_name, "Living room") == 0);
}

// A few percent of packets lost (below the heavy-loss threshold) still costs
// whole frames; the bitrate must come down instead of sitting at the cap.
TEST(abr_backs_off_when_frames_are_lost)
{
    AdaptiveBitrate abr;
    const AbrConfig cfg = AdaptiveBitrate::preset_config(proto::Preset::Quality, 0);
    abr.reset(cfg, VideoMode{});
    proto::ReceiverReport r;
    r.interval_us = 100'000;
    r.bytes_received = 300'000;
    uint64_t now = 1'000'000;
    for (int i = 0; i < 100; ++i, now += 100'000) {  // clean: probes up to the cap
        r.packets_received = 400;
        r.frames_complete = 6;
        abr.on_report(r, now);
    }
    CHECK_EQ(abr.current().bitrate_kbps, cfg.max_kbps);
    r.packets_received = 388;
    r.packets_lost = 12;  // 3 %
    r.frames_complete = 5;
    r.frames_dropped = 1;
    for (int i = 0; i < 50; ++i, now += 100'000) abr.on_report(r, now);
    CHECK(abr.current().bitrate_kbps < cfg.max_kbps * 0.6);
}

int main(int argc, char** argv) { return hktest::run_all(argc > 1 ? argv[1] : nullptr); }

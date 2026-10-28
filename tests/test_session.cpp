#include <atomic>
#include <chrono>
#include <thread>

#include "host_net.h"
#include "host_probe.h"
#include "net_client.h"
#include "hikari/clock.h"
#include "test_framework.h"

using namespace hikari;

template<class F> bool wait_for(F condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

TEST(loopback_pairing_input_video_and_reconnect)
{
    const std::string code = crypto::generate_pairing_code();
    const uint8_t id[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::atomic<uint32_t> input_buttons{0};
    std::atomic<uint32_t> lost_report{0};
    HostNetEvents events;
    events.input = [&](const proto::Input& input, uint64_t) { input_buttons = input.pad.buttons; };
    events.receiver_report = [&](const proto::ReceiverReport& report) { lost_report = report.packets_lost; };
    HostNet host;
    uint16_t port = 0;
    for (int attempt = 0; attempt < 20 && !port; ++attempt) {
        const auto candidate = static_cast<uint16_t>(50000 + crypto::random_u32() % 10000);
        if (host.start(candidate, crypto::psk_from_code(code), "Loopback", id, events)) port = candidate;
    }
    CHECK(port != 0);
    if (!port) return;
    const std::string address = "127.0.0.1:" + std::to_string(port);
    // Discovery advertises the persistent remote address and custom port,
    // while the ordinary authenticated UDP session remains compatible.
    host.set_tailscale_address("100.101.102.103");
    UdpSocket probe;
    CHECK(probe.open(0));
    NetAddress target, from;
    CHECK(NetAddress::resolve(address, port, target));
    uint8_t packet[128]{};
    ByteWriter discover(packet, sizeof(packet));
    proto::write_header(discover, proto::PacketType::Discover, 0);
    CHECK(probe.send_to(target, packet, discover.size()));
    const int count = probe.recv_from(packet, sizeof(packet), from, 1000);
    CHECK(count > 0);
    if (count > 0) {
        ByteReader reply(packet, static_cast<size_t>(count));
        proto::Header header;
        proto::DiscoverReply advertised;
        CHECK(proto::read_header(reply, header));
        CHECK(header.type == proto::PacketType::DiscoverReply);
        CHECK(proto::read(reply, advertised));
        CHECK(std::string(advertised.tailscale_ipv4) == "100.101.102.103");
        CHECK_EQ(advertised.port, port);
    }
    ClientNet client;
    CHECK(client.start({}));
    client.connect(address, "000000000000", proto::Preset::Balanced, 0);
    CHECK(wait_for([&] { return client.state() == ConnState::Rejected; }));
    CHECK(!host.client_connected());

    client.connect(address, code, proto::Preset::Balanced, 0);
    CHECK(wait_for([&] { return client.state() == ConnState::Connected && host.client_connected(); }));
    proto::Input input{};
    input.seq = 1;
    input.pad.buttons = 4;
    input.client_time_us = now_us();
    client.send_input(input);
    CHECK(wait_for([&] { return input_buttons == 4; }));

    OutgoingFrame frame;
    frame.frame_id = 1;
    frame.keyframe = true;
    frame.capture_us = now_us();
    frame.data.assign(4096, 0x5a);
    host.send_video(std::move(frame));
    CHECK(wait_for([&] { return client.frames().size() > 0; }));
    auto received = client.frames().pop();
    CHECK(received.has_value());
    if (received) {
        CHECK_EQ((*received)->data.size(), 4096u);
        CHECK((*received)->data == std::vector<uint8_t>(4096, 0x5a));
        client.recycle(std::move(*received));
    }
    CHECK(wait_for([&] { return client.clock().valid(); }));

    client.disconnect();
    CHECK(wait_for([&] { return !host.client_connected(); }));
    client.connect(address, code, proto::Preset::MinimumLatency, 0);
    CHECK(wait_for([&] { return client.state() == ConnState::Connected; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK_EQ(lost_report.load(), 0u);
    CHECK_EQ(client.stats().frames_complete, 1u);  // lifetime counters survive reconnects
    client.stop();
    host.stop();
}

TEST(loopback_probe_takeover_fallback_and_new_code)
{
    const std::string code = crypto::generate_pairing_code();
    const uint8_t id[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    std::atomic<int> connects{0};
    std::atomic<int> codecs{0};
    HostNetEvents events;
    events.connected = [&](const std::string&, proto::Preset, uint32_t, uint8_t video_codecs) {
        codecs = video_codecs;
        ++connects;
    };
    HostNet host;
    uint16_t port = 0;
    for (int attempt = 0; attempt < 20 && !port; ++attempt) {
        const auto candidate = static_cast<uint16_t>(50000 + crypto::random_u32() % 10000);
        if (host.start(candidate, crypto::psk_from_code(code), "Loopback", id, events)) port = candidate;
    }
    CHECK(port != 0);
    if (!port) return;
    const std::string address = "127.0.0.1:" + std::to_string(port);

    ClientNet first;
    CHECK(first.start({}));
    first.set_video_codecs(3);  // a PC that decodes HEVC
    first.connect(address, code, proto::Preset::Balanced, 0);
    CHECK(wait_for([&] { return first.state() == ConnState::Connected; }));
    CHECK(first.host_name() == "Loopback");
    CHECK_EQ(codecs.load(), 3);

    // A pairing check from Settings must not disturb the session in progress.
    const ProbeOutcome ok = probe_host({address}, code);
    CHECK(ok.result == ProbeResult::Ok);
    CHECK(ok.host_name == "Loopback");
    CHECK(probe_host({address}, "000000000000").result == ProbeResult::WrongCode);
    CHECK(probe_host({"no-such-host.invalid"}, code, nullptr, 1500).result == ProbeResult::CannotResolve);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(first.state() == ConnState::Connected);
    CHECK_EQ(connects.load(), 1);

    // An unresolvable primary name falls back to the invite's IP; the new
    // client takes over and the previous one is told and stays off.
    ClientNet second;
    CHECK(second.start({}));
    second.connect("no-such-host.invalid", code, proto::Preset::Balanced, 0, address);
    CHECK(wait_for([&] { return second.state() == ConnState::Connected; }));
    CHECK(wait_for([&] { return first.state() == ConnState::Replaced; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(first.state() == ConnState::Replaced);  // no tug-of-war
    CHECK(second.state() == ConnState::Connected);
    CHECK_EQ(connects.load(), 2);
    CHECK_EQ(codecs.load(), 1);  // the new client only offered H.264

    // A new pairing code applies immediately, without restarting the host.
    const std::string code2 = crypto::generate_pairing_code();
    host.set_psk(crypto::psk_from_code(code2));
    CHECK(probe_host({address}, code).result == ProbeResult::WrongCode);
    CHECK(probe_host({address}, code2).result == ProbeResult::Ok);

    first.stop();
    second.stop();
    host.stop();
}

int main() { return hktest::run_all(nullptr); }

#include <atomic>
#include <chrono>
#include <thread>

#include "host_net.h"
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

int main() { return hktest::run_all(nullptr); }

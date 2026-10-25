#pragma once
// Host side of the ESP32-S3 controller bridge.
//
//  * Finds the bridge automatically: candidate COM ports are ranked by USB
//    VID/PID (CP210x, CH34x, FTDI, Espressif) and confirmed with a HELLO
//    handshake. The user never selects a COM port.
//  * Opens the port with DTR/RTS released so the dev board's auto-reset
//    circuit never reboots the ESP32 (or drops it into the ROM loader).
//  * Newest-state-wins transmit: a dedicated thread writes the latest pad
//    state the moment it is published; a state that arrives while a write is
//    in flight replaces any older unsent one.
//  * Reconnects on unplug, re-handshakes with a fresh session ID.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hikari/histogram.h"
#include "hikari/net_protocol.h"
#include "hikari/sync.h"
#include "hk_link.h"

namespace hikari {

struct SerialPortInfo {
    std::string port;      // "COM5"
    std::string name;      // friendly name
    uint16_t vid = 0, pid = 0;
    int score = 0;
};

std::vector<SerialPortInfo> enumerate_serial_ports();

struct BridgeStatus {
    bool connected = false;
    std::string port;
    uint16_t fw_version = 0;
    uint8_t profile = HK_PROFILE_HORI_PAD;  // controller type the bridge presents
    uint8_t usb_state = HK_USB_DETACHED;
    uint8_t wake_methods = 0;
    bool remote_wakeup_enabled = false;
    bool failsafe = true;
    hk_telemetry telemetry{};
    bool telemetry_valid = false;
    LatencySummary link_rtt;     // UART ping round trip
    LatencySummary host_write;   // publish -> WriteFile complete
    uint64_t pad_frames_sent = 0;
    uint32_t last_wake_event = 0;  // (method << 8) | result
    uint64_t last_wake_event_us = 0;
};

class Bridge {
public:
    Bridge();
    ~Bridge();

    // profile: hk_profile the bridge should present; it restarts itself to switch.
    void start(const std::string& preferred_port, uint32_t baud, uint8_t profile);
    void stop();

    // Publishes the newest controller state (any thread, never blocks).
    void publish(const proto::PadState& pad, bool latency_mark);
    // Publishes the neutral state (client gone / stream stopped).
    void publish_neutral();
    void request_wake(uint8_t methods, uint16_t press_ms);

    BridgeStatus status();
    bool connected() const { return connected_.load(); }
    std::string port() const;

private:
    struct Pending {
        hk_pad_state state;
        uint64_t publish_us;
    };

    void run();
    bool try_open(const std::string& port);
    bool handshake();
    void close_port();
    bool write_frame(uint8_t type, const uint8_t* payload, size_t len, bool wait);
    void handle_frame(const hk_link_frame& f);
    void session_loop();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::string preferred_;
    uint32_t baud_ = HK_LINK_DEFAULT_BAUD;
    uint8_t profile_ = HK_PROFILE_PRO_CONTROLLER;

    void* port_ = nullptr;  // HANDLE
    std::string port_name_;
    uint32_t session_ = 0;
    std::atomic<uint32_t> seq_{0};

    Mailbox<Pending> pad_mailbox_;
    Event wake_event_;
    std::mutex wake_mu_;
    hk_wake wake_req_{};
    bool wake_pending_ = false;

    hk_link_decoder decoder_{};
    uint8_t scratch_[HK_LINK_MAX_FRAME];

    mutable std::mutex status_mu_;
    BridgeStatus status_;
    RollingHistogram rtt_hist_;
    RollingHistogram write_hist_;
    uint32_t ping_seq_ = 0;
    uint64_t last_ack_us_ = 0;
    std::atomic<uint64_t> frames_sent_{0};
};

}  // namespace hikari

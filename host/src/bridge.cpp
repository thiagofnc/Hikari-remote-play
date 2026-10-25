#include "bridge.h"

#include <windows.h>
#include <setupapi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>

#include "hikari/clock.h"
#include "hikari/crypto.h"
#include "hikari/log.h"
#include "hikari/pad_convert.h"
#include "hikari/system.h"

namespace hikari {

namespace {

const GUID kGuidDevClassPorts = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

int score_for(uint16_t vid, uint16_t pid)
{
    if (vid == 0x303A) return 100;                                    // Espressif native USB
    if (vid == 0x10C4 && pid == 0xEA60) return 90;                    // CP210x (DevKitC-1)
    if (vid == 0x1A86 && (pid == 0x55D3 || pid == 0x55D4)) return 90; // CH343 (DevKitC-1 v1.1)
    if (vid == 0x1A86) return 70;                                     // CH340/CH9102
    if (vid == 0x0403) return 50;                                     // FTDI
    return 0;
}

uint16_t parse_hex_after(const std::string& s, const char* key)
{
    const auto p = s.find(key);
    if (p == std::string::npos) return 0;
    return static_cast<uint16_t>(std::strtoul(s.c_str() + p + std::strlen(key), nullptr, 16));
}

constexpr uint64_t kPingPeriodUs = 1'000'000;
constexpr uint64_t kKeepaliveUs = 40'000;    // well inside the bridge's 250 ms failsafe
constexpr uint64_t kSilenceTimeoutUs = 3'500'000;

}  // namespace

std::vector<SerialPortInfo> enumerate_serial_ports()
{
    std::vector<SerialPortInfo> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kGuidDevClassPorts, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        SerialPortInfo info;
        wchar_t buf[512];
        if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<PBYTE>(buf),
                                              sizeof(buf), nullptr))
            info.name = utf8(buf);
        if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(buf),
                                              sizeof(buf), nullptr)) {
            std::string hw = utf8(buf);
            for (auto& c : hw) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            info.vid = parse_hex_after(hw, "VID_");
            info.pid = parse_hex_after(hw, "PID_");
        }
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            DWORD size = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
                ERROR_SUCCESS)
                info.port = utf8(buf);
            RegCloseKey(key);
        }
        if (info.port.rfind("COM", 0) != 0) continue;
        info.score = score_for(info.vid, info.pid);
        out.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    return out;
}

Bridge::Bridge() : rtt_hist_(5'000'000), write_hist_(2'000'000) {}

Bridge::~Bridge() { stop(); }

void Bridge::start(const std::string& preferred_port, uint32_t baud, uint8_t profile)
{
    stop();
    preferred_ = preferred_port;
    baud_ = baud ? baud : HK_LINK_DEFAULT_BAUD;
    profile_ = profile;
    running_ = true;
    thread_ = std::thread(&Bridge::run, this);
}

void Bridge::stop()
{
    running_ = false;
    wake_event_.set();
    if (thread_.joinable()) thread_.join();
}

std::string Bridge::port() const
{
    std::lock_guard<std::mutex> lk(status_mu_);
    return status_.port;
}

void Bridge::publish(const proto::PadState& pad, bool latency_mark)
{
    const uint64_t now = now_us();
    Pending p;
    p.state = to_link_state(pad, session_, seq_.fetch_add(1) + 1, static_cast<uint32_t>(now),
                            latency_mark ? HK_PAD_FLAG_LATENCY_MARK : 0);
    p.publish_us = now;
    pad_mailbox_.put(p);
}

void Bridge::publish_neutral()
{
    const uint64_t now = now_us();
    Pending p;
    p.state = neutral_link_state(session_, seq_.fetch_add(1) + 1, static_cast<uint32_t>(now));
    p.publish_us = now;
    pad_mailbox_.put(p);
}

void Bridge::request_wake(uint8_t methods, uint16_t press_ms)
{
    {
        std::lock_guard<std::mutex> lk(wake_mu_);
        wake_req_.methods = methods;
        wake_req_.press_ms = press_ms;
        wake_pending_ = true;
    }
    wake_event_.set();
}

BridgeStatus Bridge::status()
{
    const uint64_t now = now_us();
    std::lock_guard<std::mutex> lk(status_mu_);
    BridgeStatus s = status_;
    s.connected = connected_.load();
    s.link_rtt = rtt_hist_.summary(now);
    s.host_write = write_hist_.summary(now);
    s.pad_frames_sent = frames_sent_.load();
    return s;
}

bool Bridge::try_open(const std::string& port)
{
    const std::wstring path = L"\\\\.\\" + widen(port);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    SetupComm(h, 4096, 4096);
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = baud_;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    // DTR/RTS drive EN/IO0 on ESP32 dev boards: keep both released so opening
    // the port never resets the bridge.
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return false;
    }
    // "Return as soon as at least one byte is available" semantics.
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 50;
    to.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    port_ = h;
    port_name_ = port;
    hk_link_decoder_init(&decoder_);
    return true;
}

void Bridge::close_port()
{
    if (port_) {
        CancelIo(static_cast<HANDLE>(port_));
        CloseHandle(static_cast<HANDLE>(port_));
    }
    port_ = nullptr;
    connected_ = false;
}

bool Bridge::write_frame(uint8_t type, const uint8_t* payload, size_t len, bool wait)
{
    uint8_t wire[HK_LINK_MAX_WIRE];
    const size_t n = hk_link_build(type, payload, len, wire, sizeof(wire));
    if (!n) return false;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(static_cast<HANDLE>(port_), wire, static_cast<DWORD>(n), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(static_cast<HANDLE>(port_), &ov, &written, wait ? TRUE : TRUE);
    CloseHandle(ov.hEvent);
    return ok && written == n;
}

bool Bridge::handshake()
{
    session_ = crypto::random_u32() | 1;
    hk_hello h{session_, 250, profile_};
    uint8_t buf[16];
    const size_t n = hk_hello_encode(&h, buf);

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool acked = false;
    for (int attempt = 0; attempt < 3 && !acked; ++attempt) {
        if (!write_frame(HK_LINK_HELLO, buf, n, true)) break;
        const uint64_t deadline = now_us() + 400'000;
        while (!acked && now_us() < deadline) {
            uint8_t rx[256];
            DWORD got = 0;
            ResetEvent(ov.hEvent);
            BOOL ok = ReadFile(static_cast<HANDLE>(port_), rx, sizeof(rx), &got, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, 100) != WAIT_OBJECT_0) {
                    CancelIo(static_cast<HANDLE>(port_));
                    GetOverlappedResult(static_cast<HANDLE>(port_), &ov, &got, TRUE);
                    continue;
                }
                ok = GetOverlappedResult(static_cast<HANDLE>(port_), &ov, &got, FALSE);
            }
            if (!ok) break;
            for (DWORD i = 0; i < got; ++i) {
                hk_link_frame f;
                if (!hk_link_decoder_push(&decoder_, rx[i], scratch_, sizeof(scratch_), &f)) continue;
                hk_hello_ack a;
                if (f.type == HK_LINK_HELLO_ACK && hk_hello_ack_decode(f.payload, f.len, &a) &&
                    a.session_id == session_) {
                    std::lock_guard<std::mutex> lk(status_mu_);
                    status_.fw_version = a.fw_version;
                    status_.profile = a.profile;
                    status_.usb_state = a.usb_state;
                    status_.wake_methods = a.wake_methods;
                    status_.remote_wakeup_enabled = a.remote_wakeup_enabled != 0;
                    acked = true;
                }
            }
        }
    }
    CloseHandle(ov.hEvent);
    return acked;
}

void Bridge::handle_frame(const hk_link_frame& f)
{
    const uint64_t now = now_us();
    last_ack_us_ = now;
    std::lock_guard<std::mutex> lk(status_mu_);
    switch (f.type) {
    case HK_LINK_TELEMETRY: {
        hk_telemetry t;
        if (hk_telemetry_decode(f.payload, f.len, &t)) {
            status_.telemetry = t;
            status_.telemetry_valid = true;
            status_.usb_state = t.usb_state;
            status_.failsafe = t.failsafe_active != 0;
            status_.remote_wakeup_enabled = t.remote_wakeup_enabled != 0;
        }
        break;
    }
    case HK_LINK_PONG: {
        hk_pong p;
        if (hk_pong_decode(f.payload, f.len, &p)) {
            const uint32_t rtt = static_cast<uint32_t>(now) - p.host_time_us;
            if (rtt < 1'000'000) rtt_hist_.record(rtt, now);
        }
        break;
    }
    case HK_LINK_EVENT: {
        hk_event e;
        if (!hk_event_decode(f.payload, f.len, &e)) break;
        if (e.code == HK_EVENT_USB_STATE) {
            status_.usb_state = static_cast<uint8_t>(e.arg);
            HK_LOGI("Console USB: %s", e.arg == HK_USB_ACTIVE      ? "active (awake)"
                                       : e.arg == HK_USB_SUSPENDED ? "suspended (asleep)"
                                                                   : "detached");
        } else if (e.code == HK_EVENT_FAILSAFE) {
            status_.failsafe = e.arg != 0;
        } else if (e.code == HK_EVENT_WAKE) {
            status_.last_wake_event = e.arg;
            status_.last_wake_event_us = now;
            HK_LOGI("Wake attempt: method 0x%02x result %u", (e.arg >> 8) & 0xFF, e.arg & 0xFF);
        }
        break;
    }
    case HK_LINK_HELLO_ACK: {
        hk_hello_ack a;
        if (hk_hello_ack_decode(f.payload, f.len, &a)) {
            status_.usb_state = a.usb_state;
            status_.wake_methods = a.wake_methods;
        }
        break;
    }
    default:
        break;
    }
}

void Bridge::session_loop()
{
    HANDLE h = static_cast<HANDLE>(port_);
    OVERLAPPED rov{}, wov{};
    rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    uint8_t rx[512];
    uint8_t wire[HK_LINK_MAX_WIRE];
    bool read_pending = false, write_pending = false;
    uint64_t write_publish_us = 0;
    bool write_is_pad = false;
    std::deque<std::vector<uint8_t>> control;  // small control frames (ping, wake)
    hk_pad_state last_pad{};
    bool have_last = false;
    bool keepalive = false;
    uint64_t last_pad_write = 0;
    uint64_t next_ping = now_us();
    last_ack_us_ = now_us();
    bool alive = true;

    auto start_read = [&]() -> bool {
        DWORD got = 0;
        ResetEvent(rov.hEvent);
        if (ReadFile(h, rx, sizeof(rx), &got, &rov)) {
            SetEvent(rov.hEvent);  // completed synchronously; handled by the event path
            return true;
        }
        if (GetLastError() == ERROR_IO_PENDING) return true;
        return false;
    };

    auto start_write = [&](const uint8_t* data, size_t n) -> bool {
        std::memcpy(wire, data, n);
        ResetEvent(wov.hEvent);
        DWORD w = 0;
        if (WriteFile(h, wire, static_cast<DWORD>(n), &w, &wov) || GetLastError() == ERROR_IO_PENDING) {
            write_pending = true;
            return true;
        }
        return false;
    };

    auto schedule_write = [&]() {
        if (write_pending) return;
        // Controller state always goes first: it is the latency-critical payload.
        if (auto p = pad_mailbox_.take()) {
            uint8_t payload[HK_PAD_PAYLOAD_IMU];
            uint8_t frame[HK_LINK_MAX_WIRE];
            const size_t n = hk_link_build(HK_LINK_PAD_STATE, payload, hk_pad_encode(&p->state, payload), frame,
                                           sizeof(frame));
            last_pad = p->state;
            have_last = true;
            keepalive = !(p->state.flags & HK_PAD_FLAG_NEUTRAL);
            write_publish_us = p->publish_us;
            write_is_pad = true;
            last_pad_write = now_us();
            if (!start_write(frame, n)) alive = false;
            return;
        }
        if (!control.empty()) {
            write_is_pad = false;
            if (!start_write(control.front().data(), control.front().size())) alive = false;
            control.pop_front();
        }
    };

    auto queue_control = [&](uint8_t type, const uint8_t* payload, size_t len) {
        std::vector<uint8_t> v(HK_LINK_MAX_WIRE);
        const size_t n = hk_link_build(type, payload, len, v.data(), v.size());
        v.resize(n);
        if (control.size() < 8) control.push_back(std::move(v));
    };

    if (!start_read()) alive = false;
    read_pending = true;

    while (running_ && alive) {
        const uint64_t now = now_us();
        if (now >= next_ping) {
            hk_ping p{++ping_seq_, static_cast<uint32_t>(now)};
            uint8_t b[16];
            queue_control(HK_LINK_PING, b, hk_ping_encode(&p, b));
            next_ping = now + kPingPeriodUs;
        }
        // Keepalive: re-send the held state with a fresh sequence number so the
        // bridge's failsafe only fires when the host/client truly went away.
        if (keepalive && have_last && !write_pending && now - last_pad_write > kKeepaliveUs && pad_mailbox_.empty()) {
            Pending p;
            p.state = last_pad;
            p.state.seq = seq_.fetch_add(1) + 1;
            p.state.flags &= static_cast<uint8_t>(~HK_PAD_FLAG_LATENCY_MARK);
            p.state.host_time_us = static_cast<uint32_t>(now);
            p.publish_us = now;
            pad_mailbox_.put(p);
        }
        {
            std::lock_guard<std::mutex> lk(wake_mu_);
            if (wake_pending_) {
                uint8_t b[8];
                queue_control(HK_LINK_WAKE, b, hk_wake_encode(&wake_req_, b));
                wake_pending_ = false;
            }
        }
        schedule_write();
        if (now - last_ack_us_ > kSilenceTimeoutUs) {
            HK_LOGW("Bridge on %s stopped responding", port_name_.c_str());
            break;
        }

        HANDLE waits[3] = {rov.hEvent, pad_mailbox_.event(), wake_event_.handle()};
        HANDLE waits_w[4] = {rov.hEvent, wov.hEvent, pad_mailbox_.event(), wake_event_.handle()};
        DWORD timeout = 20;
        const DWORD r = write_pending ? WaitForMultipleObjects(4, waits_w, FALSE, timeout)
                                      : WaitForMultipleObjects(3, waits, FALSE, timeout);
        (void)r;

        if (WaitForSingleObject(rov.hEvent, 0) == WAIT_OBJECT_0) {
            DWORD got = 0;
            if (!GetOverlappedResult(h, &rov, &got, FALSE)) {
                alive = false;
                break;
            }
            for (DWORD i = 0; i < got; ++i) {
                hk_link_frame f;
                if (hk_link_decoder_push(&decoder_, rx[i], scratch_, sizeof(scratch_), &f)) handle_frame(f);
            }
            if (!start_read()) alive = false;
        }
        if (write_pending && WaitForSingleObject(wov.hEvent, 0) == WAIT_OBJECT_0) {
            DWORD w = 0;
            if (!GetOverlappedResult(h, &wov, &w, FALSE)) {
                alive = false;
                break;
            }
            write_pending = false;
            if (write_is_pad) {
                const uint64_t t = now_us();
                write_hist_.record(t - write_publish_us, t);
                ++frames_sent_;
            }
        }
    }
    CancelIo(h);
    DWORD dummy = 0;
    if (read_pending) GetOverlappedResult(h, &rov, &dummy, TRUE);
    if (write_pending) GetOverlappedResult(h, &wov, &dummy, TRUE);
    CloseHandle(rov.hEvent);
    CloseHandle(wov.hEvent);
}

void Bridge::run()
{
    set_thread_role(ThreadRole::Input, "hikari-bridge");
    while (running_) {
        // Candidate list: the last known-good port first, then by USB ID score.
        std::vector<std::string> candidates;
        if (!preferred_.empty()) candidates.push_back(preferred_);
        for (const auto& p : enumerate_serial_ports())
            if (p.score > 0 && p.port != preferred_) candidates.push_back(p.port);

        bool found = false;
        for (const auto& port : candidates) {
            if (!running_) break;
            if (!try_open(port)) continue;
            if (handshake()) {
                found = true;
                preferred_ = port;
                {
                    std::lock_guard<std::mutex> lk(status_mu_);
                    status_.port = port;
                    status_.connected = true;
                }
                connected_ = true;
                BridgeStatus st = status();
                HK_LOGI("Controller bridge on %s (fw %u.%u, %s, console USB %s, wake methods 0x%02x)", port.c_str(),
                        st.fw_version >> 8, st.fw_version & 0xFF,
                        st.profile == HK_PROFILE_PRO_CONTROLLER ? "Pro Controller" : "HORI pad",
                        st.usb_state == HK_USB_ACTIVE      ? "active"
                        : st.usb_state == HK_USB_SUSPENDED ? "suspended"
                                                           : "detached",
                        st.wake_methods);
                // Firmware 1.0 predates profile switching and stays a HORI pad.
                if (st.profile != profile_ && st.fw_version < 0x0101)
                    HK_LOGW("Bridge firmware %u.%u cannot act as a Pro Controller; flash the latest hikari-bridge-esp32s3.bin",
                            st.fw_version >> 8, st.fw_version & 0xFF);
                else if (st.profile != profile_)
                    HK_LOGI("Bridge restarting to switch controller type");
                session_loop();
                HK_LOGW("Controller bridge on %s disconnected", port.c_str());
            }
            close_port();
            {
                std::lock_guard<std::mutex> lk(status_mu_);
                status_.connected = false;
                status_.usb_state = HK_USB_DETACHED;
            }
            if (found) break;
        }
        // Rescan every 2 s (USB plug events are rare; this costs nothing).
        for (int i = 0; i < 20 && running_; ++i) Sleep(100);
    }
    close_port();
}

}  // namespace hikari

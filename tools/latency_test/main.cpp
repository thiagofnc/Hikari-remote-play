// hikari-latency-test: measures the host -> ESP32 -> console controller path
// at several publish rates using the bridge's own telemetry, and drives the
// latency-marker GPIO for glass-to-glass camera tests.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bridge.h"
#include "bridge_util.h"
#include "hikari/clock.h"
#include "hikari/histogram.h"
#include "hikari/system.h"
#include "tool_common.h"

using namespace hikari;
using namespace hikari::tool;

namespace {

const char* kHelp = R"(hikari-latency-test - measure the host -> ESP32 -> console input path

Usage:
  hikari-latency-test [--rates 125,250,500,1000] [--seconds 5] [options]
  hikari-latency-test --marker [--seconds 20] [--marker-button A] [options]

Rate sweep (default): for each publish rate, sends a continuously changing
pad state (stick sweep + button toggles) paced by a high-resolution timer,
then reads the bridge firmware's 1 Hz telemetry:
  rx->submit     host packet received -> USB report armed (ESP32 internal)
  rx->consumed   host packet received -> console actually read the report
  poll           interval between console USB reads (its polling rate)
  rx/s, rep/s    packets received from the host / reports read by console
  gaps, stale    sequence gaps (states replaced on the host before reaching
                 the wire) and out-of-date packets; invalid = CRC/format errors
  write          host publish -> serial WriteFile complete (p99)
  rtt            serial link ping round trip (p50)
and recommends the lowest rate beyond which rx->consumed p99 stops improving.
Run it with the console awake on a harmless screen (e.g. System Settings >
Controllers > Test Input Devices): sticks and the toggle buttons are pressed.

Marker mode (--marker): once per second presses --marker-button with the
latency-mark flag; the ESP32 pulses its marker GPIO when it hands that report
to the console, so a camera/photodiode can time GPIO -> on-screen reaction.

Options:
  --rates LIST         publish rates in Hz (default 125,250,500,1000)
  --seconds N          measurement time per rate (default 5; marker: 20)
  --toggle BUTTONS     buttons toggled during the sweep (default ZL+ZR;
                       "none" to only move the sticks)
  --marker             marker mode (see above)
  --marker-button B    button pressed on each marker (default A, or none)
  --hold-ms N          marker press length (default 100)
  --port COMx          try this serial port first
  --baud N             link baud rate (default 2000000)
  --json FILE          save results as JSON
  --csv FILE           save the comparison table as CSV
  --verbose            show library log output
  --help               this text

Exit codes: 0 ok, 2 no bridge found, 64 bad arguments.
)";

std::atomic<bool> g_stop{false};
Bridge* g_bridge = nullptr;

BOOL WINAPI on_ctrl(DWORD)
{
    g_stop = true;
    if (g_bridge) g_bridge->publish_neutral();
    return TRUE;
}

struct Agg {
    double p50 = 0, p95 = 0, p99 = 0, worst_p99 = 0, max = 0;
    void add_all(const std::vector<hk_lat4>& v)
    {
        if (v.empty()) return;
        for (const auto& l : v) {
            p50 += l.p50;
            p95 += l.p95;
            p99 += l.p99;
            worst_p99 = std::max(worst_p99, double(l.p99));
            max = std::max(max, double(l.max));
        }
        p50 /= double(v.size());
        p95 /= double(v.size());
        p99 /= double(v.size());
    }
};

struct RateResult {
    int rate = 0;
    double publish_hz = 0;
    LatencySummary pace;  // host publish interval
    int windows = 0;      // telemetry snapshots aggregated
    Agg submit, consumed, poll;
    double rx_per_s = 0, reports_per_s = 0;
    uint32_t gaps = 0, stale = 0, invalid = 0, usb_fail = 0;
    double frames_sent_per_s = 0;
    LatencySummary host_write, link_rtt;
    uint8_t usb_state = HK_USB_DETACHED;
};

proto::PadState sweep_state(uint64_t i, int rate, uint32_t toggle)
{
    // Full-circle stick sweep once per second and a button toggle every
    // 50 ms: every packet differs from the previous one.
    proto::PadState p;
    const double phase = 2.0 * 3.14159265358979 * double(i) / double(rate);
    p.lx = static_cast<int16_t>(std::lround(std::cos(phase) * 30000));
    p.ly = static_cast<int16_t>(std::lround(std::sin(phase) * 30000));
    p.rx = static_cast<int16_t>(-p.lx);
    p.ry = p.ly;
    const uint64_t toggle_period = std::max<uint64_t>(1, uint64_t(rate) / 20);
    if ((i / toggle_period) % 2) p.buttons = toggle;
    return p;
}

RateResult run_rate(Bridge& bridge, int rate, double seconds, uint32_t toggle)
{
    RateResult r;
    r.rate = rate;
    const uint64_t period = 1'000'000 / static_cast<uint64_t>(rate);
    PreciseTimer timer;
    Histogram pace;

    // Warm-up covers one full 1 Hz telemetry window, so every snapshot taken
    // during measurement describes this rate only.
    const double warmup = 1.3;
    const uint64_t t0 = now_us();
    const uint64_t measure_start = t0 + static_cast<uint64_t>(warmup * 1e6);
    const uint64_t end = measure_start + static_cast<uint64_t>(seconds * 1e6);
    uint64_t next = t0, i = 0, last_pub = 0, published = 0;
    uint32_t last_uptime = 0;
    std::vector<hk_telemetry> snaps;
    hk_telemetry baseline{};
    bool have_baseline = false;
    uint64_t sent0 = 0;
    uint64_t next_poll = t0;

    while (!g_stop) {
        const uint64_t now = now_us();
        if (now >= end) break;
        bridge.publish(sweep_state(i++, rate, toggle), false);
        if (now >= measure_start) {
            if (last_pub) pace.record(now - last_pub);
            ++published;
        }
        last_pub = now >= measure_start ? now : 0;
        if (now >= next_poll) {
            next_poll = now + 50'000;
            const BridgeStatus st = bridge.status();
            if (st.telemetry_valid && st.telemetry.uptime_ms != last_uptime) {
                last_uptime = st.telemetry.uptime_ms;
                if (now >= measure_start) {
                    snaps.push_back(st.telemetry);
                } else {
                    baseline = st.telemetry;
                    have_baseline = true;
                }
            }
            if (now < measure_start) sent0 = st.pad_frames_sent;
        }
        next += period;
        if (next < now) next = now;  // never burst to catch up
        timer.sleep_until_us(next);
    }

    const BridgeStatus st = bridge.status();
    const double secs = double(now_us() - measure_start) / 1e6;
    r.publish_hz = secs > 0 ? double(published) / secs : 0;
    r.pace = pace.summary();
    r.frames_sent_per_s = secs > 0 ? double(st.pad_frames_sent - sent0) / secs : 0;
    r.host_write = st.host_write;
    r.link_rtt = st.link_rtt;
    r.usb_state = st.usb_state;
    r.windows = static_cast<int>(snaps.size());
    if (!snaps.empty()) {
        std::vector<hk_lat4> a, b, c;
        for (const auto& s : snaps) {
            a.push_back(s.rx_to_submit);
            b.push_back(s.rx_to_consumed);
            c.push_back(s.poll_interval);
            r.rx_per_s += s.rx_packets_per_s;
            r.reports_per_s += s.usb_reports_per_s;
        }
        r.submit.add_all(a);
        r.consumed.add_all(b);
        r.poll.add_all(c);
        r.rx_per_s /= double(snaps.size());
        r.reports_per_s /= double(snaps.size());
        const hk_telemetry& first = have_baseline ? baseline : snaps.front();
        const hk_telemetry& last = snaps.back();
        r.gaps = last.rx_seq_gaps - first.rx_seq_gaps;
        r.stale = last.rx_stale - first.rx_stale;
        r.invalid = last.rx_invalid - first.rx_invalid;
        r.usb_fail = last.usb_fail - first.usb_fail;
    }
    return r;
}

std::string f2(double us) { return strf("%.2f", us / 1000.0); }

int run_marker(Bridge& bridge, double seconds, uint32_t button, int hold_ms)
{
    std::printf("Marker mode: one latency mark per second for %.0f s (button %s, held %d ms).\n", seconds,
                button ? button_names(button).c_str() : "none", hold_ms);
    std::printf("The ESP32 pulses its marker GPIO when the marked report is handed to the console.\n\n");
    std::printf("%4s %14s %12s %14s\n", "#", "host time (s)", "t (s)", "write done ms");
    PreciseTimer timer;
    const uint64_t t0 = now_us();
    uint64_t next = t0 + 500'000;
    int n = 0;
    proto::PadState idle;
    while (!g_stop && double(now_us() - t0) < seconds * 1e6) {
        timer.sleep_until_us(next);
        if (g_stop) break;
        proto::PadState mark;
        mark.buttons = button;
        const uint64_t sent_before = bridge.status().pad_frames_sent;
        const uint64_t pub = now_us();
        bridge.publish(mark, true);
        // Time until the bridge thread finished writing it to the UART.
        uint64_t done = 0;
        while (now_us() - pub < 20'000) {
            if (bridge.status().pad_frames_sent != sent_before) {
                done = now_us();
                break;
            }
            timer.sleep_until_us(now_us() + 100);
        }
        ++n;
        std::printf("%4d %14.6f %12.3f %14s\n", n, double(pub) / 1e6, double(pub - t0) / 1e6,
                    done ? f2(double(done - pub)).c_str() : "timeout");
        std::fflush(stdout);
        timer.sleep_until_us(pub + uint64_t(hold_ms) * 1000);
        bridge.publish(idle, false);
        next += 1'000'000;
    }
    std::printf("\n%d markers sent. Host times are QPC microseconds / 1e6 (same clock as Hikari logs).\n", n);
    std::printf("Note: the ESP32 also reports HK_EVENT_LATENCY_MARK, but Bridge does not expose it yet,\n"
                "so device-side marker timestamps are not shown here.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Args args(argc, argv, {"rates", "seconds", "toggle", "marker-button", "hold-ms", "port", "baud", "json", "csv"},
              {"marker", "verbose"});
    if (args.has("help")) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    init_console(args.has("verbose"));

    std::vector<int> rates;
    for (const auto& s : split(args.get("rates", "125,250,500,1000"), ',')) {
        double v = 0;
        if (!parse_double(s, v) || v < 1 || v > 8000) {
            std::fprintf(stderr, "error: bad rate '%s' (1..8000 Hz)\n", s.c_str());
            return 64;
        }
        rates.push_back(static_cast<int>(v));
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    const bool marker = args.has("marker");
    const double seconds = args.num("seconds", marker ? 20 : 5);
    const int hold_ms = static_cast<int>(args.num("hold-ms", 100));
    const uint32_t baud = static_cast<uint32_t>(args.num("baud", HK_LINK_DEFAULT_BAUD));
    uint32_t toggle = 0, mark_button = 0;
    std::string bad;
    const std::string toggle_s = args.get("toggle", "ZL+ZR");
    if (lower(toggle_s) != "none" && !parse_buttons(toggle_s, toggle, &bad)) {
        std::fprintf(stderr, "error: unknown button '%s'\n", bad.c_str());
        return 64;
    }
    const std::string mark_s = args.get("marker-button", "A");
    if (lower(mark_s) != "none" && !parse_buttons(mark_s, mark_button, &bad)) {
        std::fprintf(stderr, "error: unknown button '%s'\n", bad.c_str());
        return 64;
    }
    if (!args.error().empty() || seconds <= 0 || hold_ms < 0) {
        std::fprintf(stderr, "error: %s\nRun with --help for usage.\n",
                     args.error().empty() ? "invalid arguments" : args.error().c_str());
        return 64;
    }

    apply_process_performance_policy();
    set_thread_role(ThreadRole::Input, "latency-test");

    Bridge bridge;
    std::printf("Looking for the controller bridge...\n");
    std::fflush(stdout);
    if (!connect_bridge(bridge, args.get("port"), baud, 5.0)) return 2;
    g_bridge = &bridge;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    {
        const BridgeStatus st = bridge.status();
        std::printf("Bridge on %s, firmware %u.%u, console USB %s\n\n", st.port.c_str(), st.fw_version >> 8,
                    st.fw_version & 0xFF, usb_state_name(st.usb_state));
        if (st.usb_state != HK_USB_ACTIVE)
            std::printf("Warning: the console is not polling the bridge (asleep/off/undocked); rx->consumed and\n"
                        "poll figures will be empty. Wake the console for meaningful numbers.\n\n");
    }

    int rc = 0;
    if (marker) {
        rc = run_marker(bridge, seconds, mark_button, hold_ms);
    } else {
        std::vector<RateResult> results;
        for (int rate : rates) {
            if (g_stop) break;
            std::printf("  %4d Hz for %.0f s (+1.3 s warm-up) ...\n", rate, seconds);
            std::fflush(stdout);
            results.push_back(run_rate(bridge, rate, seconds, toggle));
            if (!bridge.connected()) {
                std::printf("Bridge disconnected during the test.\n");
                break;
            }
        }
        std::printf("\n");

        Table t({"Rate Hz", "pub/s", "rx/s", "rep/s", "submit p50", "p99", "consumed p50", "p95", "p99", "worst p99",
                 "poll p50", "gaps", "stale", "invalid", "write p99", "rtt p50"});
        for (const auto& r : results) {
            const bool tv = r.windows > 0;
            t.row({strf("%d", r.rate), strf("%.0f", r.publish_hz), tv ? strf("%.0f", r.rx_per_s) : "-",
                   tv ? strf("%.0f", r.reports_per_s) : "-", tv ? f2(r.submit.p50) : "-", tv ? f2(r.submit.p99) : "-",
                   tv ? f2(r.consumed.p50) : "-", tv ? f2(r.consumed.p95) : "-", tv ? f2(r.consumed.p99) : "-",
                   tv ? f2(r.consumed.worst_p99) : "-", tv ? f2(r.poll.p50) : "-", tv ? strf("%u", r.gaps) : "-",
                   tv ? strf("%u", r.stale) : "-", tv ? strf("%u", r.invalid) : "-",
                   r.host_write.count ? ms(r.host_write.p99) : "-", r.link_rtt.count ? ms(r.link_rtt.p50) : "-"});
        }
        if (!t.empty()) t.print();
        std::printf("  (latencies in ms; ESP32 percentiles are the mean of its 1 s telemetry windows,\n"
                    "   'worst p99' the worst window. gaps = states superseded on the host before the UART took "
                    "them.)\n\n");

        // Recommendation: walk up the rates and keep a higher one only if it
        // buys a real p99 improvement (>10% and >100 us).
        const RateResult* pick = nullptr;
        for (const auto& r : results) {
            if (r.windows == 0 || r.consumed.p99 <= 0) continue;
            if (!pick || (r.consumed.p99 < pick->consumed.p99 * 0.9 && pick->consumed.p99 - r.consumed.p99 > 100))
                pick = &r;
        }
        if (pick) {
            const double poll_hz = pick->poll.p50 > 0 ? 1e6 / pick->poll.p50 : 0;
            std::printf("Recommendation: publish at %d Hz. rx->consumed p99 is %.2f ms there and does not improve\n"
                        "meaningfully at higher rates. The console polls the controller every %.2f ms (~%.0f Hz):\n"
                        "sending faster than it reads only replaces states it never sees and adds UART/USB contention.\n",
                        pick->rate, pick->consumed.p99 / 1000.0, pick->poll.p50 / 1000.0, poll_hz);
        } else {
            std::printf("No usable telemetry (console asleep or firmware not reporting), so no recommendation.\n");
        }

        if (args.has("csv")) {
            if (t.save_csv(args.get("csv")))
                std::printf("\nSaved CSV to %s\n", args.get("csv").c_str());
            else
                std::fprintf(stderr, "Could not write %s\n", args.get("csv").c_str());
        }
        if (args.has("json")) {
            JsonWriter j;
            j.begin_object().begin_array("rates");
            auto agg = [&j](const char* key, const Agg& a) {
                j.begin_object(key)
                    .value("p50_us", a.p50)
                    .value("p95_us", a.p95)
                    .value("p99_us", a.p99)
                    .value("worst_p99_us", a.worst_p99)
                    .value("max_us", a.max)
                    .end_object();
            };
            for (const auto& r : results) {
                j.begin_object()
                    .value("rate_hz", r.rate)
                    .value("publish_hz", r.publish_hz)
                    .value("frames_sent_per_s", r.frames_sent_per_s)
                    .value("telemetry_windows", r.windows)
                    .value("rx_per_s", r.rx_per_s)
                    .value("reports_per_s", r.reports_per_s)
                    .value("seq_gaps", r.gaps)
                    .value("stale", r.stale)
                    .value("invalid", r.invalid)
                    .value("usb_fail", r.usb_fail);
                agg("rx_to_submit", r.submit);
                agg("rx_to_consumed", r.consumed);
                agg("poll_interval", r.poll);
                j.value("host_write_p50_us", r.host_write.p50)
                    .value("host_write_p99_us", r.host_write.p99)
                    .value("link_rtt_p50_us", r.link_rtt.p50)
                    .value("link_rtt_p99_us", r.link_rtt.p99)
                    .value("publish_interval_p99_us", r.pace.p99)
                    .end_object();
            }
            j.end_array();
            if (pick) j.value("recommended_rate_hz", pick->rate);
            j.end_object();
            if (j.save(args.get("json")))
                std::printf("Saved JSON to %s\n", args.get("json").c_str());
            else
                std::fprintf(stderr, "Could not write %s\n", args.get("json").c_str());
        }
    }

    bridge.publish_neutral();
    flush_bridge(bridge);
    g_bridge = nullptr;
    bridge.stop();
    return rc;
}

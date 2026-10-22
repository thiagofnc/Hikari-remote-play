// hikari-capture-probe: lists capture devices and their native modes, picks
// the likely HDMI card, benchmarks its ~1080p60/720p60 modes through the real
// VideoCapture path and recommends the mode + format preference to use.

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "audio_capture.h"
#include "capture.h"
#include "hikari/clock.h"
#include "hikari/histogram.h"
#include "hikari/media.h"
#include "hikari/system.h"
#include "tool_common.h"

using namespace hikari;
using namespace hikari::tool;

namespace {

const char* kHelp = R"(hikari-capture-probe - measure HDMI capture devices

Usage: hikari-capture-probe [options]

Lists every video capture device with all native modes and its HDMI-card
score, picks the most likely capture card, then captures from each plausible
1080p60 / 720p60 mode for a few seconds through the same path the host uses
(async MF source reader + GPU upload) and measures:
  fps          achieved frame rate
  interval     frame-to-frame time p50/p95/p99/max (jitter; 16.67 ms ideal)
  driver       device timestamp -> callback latency (when the driver stamps)
  cpu          process CPU during the run (% of whole machine)
  signal       whether a real picture (not a flat "no signal" screen) is seen
Also lists audio capture endpoints and which one pairs with the card, and
ends with the recommended mode and capture format preference.

Options:
  --seconds N        capture time per mode (default 4)
  --device N|TEXT    benchmark device index N or the first whose name
                     contains TEXT (default: highest score)
  --all-modes        benchmark every supported mode of the device, not just
                     the ~1080p60/720p60 ones
  --json FILE        save everything as JSON
  --csv FILE         save the benchmark table as CSV
  --verbose          show library log output
  --help             this text
)";

bool supported_format(const GUID& g)
{
    return g == MFVideoFormat_NV12 || g == MFVideoFormat_YUY2 || g == MFVideoFormat_MJPG ||
           g == MFVideoFormat_RGB32 || g == MFVideoFormat_ARGB32;
}
bool uncompressed(const GUID& g) { return g != MFVideoFormat_MJPG; }

// Built-in devices all report this PnP container ("the computer itself").
const GUID kMachineContainer = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};

std::string guid_str(const GUID& g)
{
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return utf8(buf);
}

struct Bench {
    CaptureMode mode;
    bool opened = false;
    bool failed = false;
    double nominal_fps = 0;
    double fps = 0;
    uint64_t frames = 0;
    uint64_t errors = 0;
    LatencySummary interval;
    LatencySummary driver;
    double cpu = 0;
    bool signal = false;
    float luma = 0;
    bool real_rate() const { return opened && !failed && frames > 0 && fps >= nominal_fps * 0.95; }
};

// Records over the whole run (VideoCapture's own histograms are a rolling
// 2 s window and include the warm-up frames).
struct Recorder {
    Histogram interval;
    Histogram driver;
    std::atomic<bool> recording{false};
    std::atomic<uint64_t> frames{0};
    uint64_t last_cb = 0;  // only touched by the MF callback thread
};

Bench run_mode(GpuDevice& gpu, const CaptureDeviceInfo& dev, const CaptureMode& mode, double seconds)
{
    Bench b;
    b.mode = mode;
    b.nominal_fps = mode.fps();
    Recorder rec;
    VideoCapture cap;
    cap.set_upload(true);  // include the per-frame GPU upload the host pays
    cap.set_frame_callback([&rec](CapturedFrame&& f) {
        if (!rec.recording.load(std::memory_order_relaxed)) {
            rec.last_cb = 0;
            return;
        }
        if (rec.last_cb) rec.interval.record(f.callback_us - rec.last_cb);
        rec.last_cb = f.callback_us;
        if (f.capture_us != f.callback_us) rec.driver.record(f.callback_us - f.capture_us);
        rec.frames.fetch_add(1, std::memory_order_relaxed);
    });
    if (!cap.open(gpu, dev, mode)) return b;
    b.opened = true;

    // Warm-up: the first frames after stream start are often late/duplicated.
    PreciseTimer timer;
    timer.sleep_until_us(now_us() + 700'000);
    CpuMeter cpu;
    cpu.start();
    const uint64_t t0 = now_us();
    rec.recording = true;
    const uint64_t end = t0 + static_cast<uint64_t>(seconds * 1e6);
    while (now_us() < end && !cap.failed()) timer.sleep_until_us(std::min(end, now_us() + 100'000));
    rec.recording = false;
    const uint64_t t1 = now_us();
    b.cpu = cpu.machine_percent();

    const CaptureStats st = cap.stats();
    b.failed = cap.failed();
    b.errors = st.errors;
    b.signal = st.signal;
    b.luma = st.mean_luma;
    cap.close();

    b.frames = rec.frames.load();
    b.fps = t1 > t0 ? b.frames * 1e6 / double(t1 - t0) : 0;
    b.interval = rec.interval.summary();
    b.driver = rec.driver.summary();
    return b;
}

std::string mode_label(const CaptureMode& m)
{
    return strf("%ux%u@%.2f %s", m.width, m.height, m.fps(), subtype_name(m.subtype).c_str());
}

}  // namespace

int main(int argc, char** argv)
{
    Args args(argc, argv, {"seconds", "device", "json", "csv"}, {"all-modes", "verbose"});
    if (args.has("help")) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    init_console(args.has("verbose"));
    const double seconds = std::clamp(args.num("seconds", 4), 1.0, 120.0);
    if (!args.error().empty()) {
        std::fprintf(stderr, "error: %s\nRun with --help for usage.\n", args.error().c_str());
        return 64;
    }

    apply_process_performance_policy();
    set_thread_role(ThreadRole::Video, "capture-probe");
    MediaRuntime mf;
    if (!mf.ok()) {
        std::fprintf(stderr, "Media Foundation is not available on this system.\n");
        return 1;
    }

    std::printf("Hikari capture probe\n\n");
    auto devices = enumerate_capture_devices(true);
    JsonWriter j;
    j.begin_object();
    j.begin_array("devices");

    int picked = -1;
    int best_score = -1000000;
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        const int score = capture_device_score(d);
        if (score > best_score) {
            best_score = score;
            picked = static_cast<int>(i);
        }
        std::printf("[%zu] %s  (HDMI-card score %d, %zu modes)\n", i, d.name.c_str(), score, d.modes.size());
        Table t({"#", "Resolution", "fps", "Format"});
        j.begin_object().value("index", i).value("name", d.name).value("symlink", utf8(d.symlink.c_str()));
        j.value("score", score);
        j.begin_array("modes");
        for (const auto& m : d.modes) {
            t.row({strf("%lu", static_cast<unsigned long>(m.native_index)), strf("%ux%u", m.width, m.height),
                   strf("%.2f", m.fps()), subtype_name(m.subtype)});
            j.begin_object()
                .value("native_index", m.native_index)
                .value("width", m.width)
                .value("height", m.height)
                .value("fps", m.fps())
                .value("format", subtype_name(m.subtype))
                .end_object();
        }
        j.end_array().end_object();
        if (!t.empty()) t.print();
        std::printf("\n");
    }
    j.end_array();

    if (args.has("device")) {
        const std::string sel = args.get("device");
        double idx = 0;
        picked = -1;
        if (parse_double(sel, idx) && idx >= 0 && idx < static_cast<double>(devices.size())) {
            picked = static_cast<int>(idx);
        } else {
            for (size_t i = 0; i < devices.size(); ++i)
                if (lower(devices[i].name).find(lower(sel)) != std::string::npos) {
                    picked = static_cast<int>(i);
                    break;
                }
        }
        if (picked < 0) {
            std::fprintf(stderr, "No capture device matches '%s'.\n", sel.c_str());
            return 1;
        }
    }

    // Audio endpoints and the one the host would pair with the picked card.
    const auto eps = enumerate_audio_capture_endpoints();
    const int paired = picked >= 0 ? match_audio_endpoint(eps, devices[static_cast<size_t>(picked)].symlink,
                                                          devices[static_cast<size_t>(picked)].name)
                                   : -1;
    GUID video_container{};
    const bool have_container =
        picked >= 0 && container_id_for_symlink(devices[static_cast<size_t>(picked)].symlink, video_container);
    std::printf("Audio capture endpoints (video device container %s):\n",
                have_container ? guid_str(video_container).c_str() : "unknown");
    if (eps.empty()) std::printf("  (none)\n");
    for (size_t i = 0; i < eps.size(); ++i) {
        const bool same = have_container && IsEqualGUID(eps[i].container, video_container);
        std::printf("  [%zu] %s%s%s\n", i, eps[i].name.c_str(),
                    static_cast<int>(i) == paired ? "   <- paired with the capture device" : "",
                    static_cast<int>(i) == paired ? (same ? " (same USB device)" : " (by name)") : "");
        if (args.has("verbose")) std::printf("       container %s\n", guid_str(eps[i].container).c_str());
    }
    if (paired >= 0 && have_container && IsEqualGUID(video_container, kMachineContainer))
        std::printf("  Note: the video device is built in (machine container), so this pairing is not meaningful.\n");
    j.value("video_container", have_container ? guid_str(video_container) : std::string());
    j.begin_array("audio_endpoints");
    for (size_t i = 0; i < eps.size(); ++i)
        j.begin_object()
            .value("name", eps[i].name)
            .value("container", guid_str(eps[i].container))
            .value("paired", static_cast<int>(i) == paired)
            .end_object();
    j.end_array();
    std::printf("\n");

    if (devices.empty() || picked < 0) {
        std::printf("No video capture devices found. Connect the HDMI capture card (USB 3 port) and retry.\n");
        j.value("picked", -1).end_object();
        if (args.has("json")) j.save(args.get("json"));
        return 1;
    }

    const auto& dev = devices[static_cast<size_t>(picked)];
    const int dev_score = capture_device_score(dev);
    if (dev_score < 30 && !args.has("device"))
        std::printf("No device looks like an HDMI capture card (best score %d). Benchmarking the best guess "
                    "anyway: %s\n\n",
                    dev_score, dev.name.c_str());
    else
        std::printf("Likely HDMI capture card: [%d] %s\n\n", picked, dev.name.c_str());

    // Candidate modes: ~1080p60 and ~720p60 in every usable format. A device
    // without those (webcams) still gets its closest mode per format so the
    // run says something useful about the capture path.
    std::vector<CaptureMode> candidates;
    auto add = [&](const CaptureMode& m) {
        for (const auto& c : candidates)
            if (c.subtype == m.subtype && c.width == m.width && c.height == m.height &&
                std::abs(c.fps() - m.fps()) < 0.01)
                return;
        candidates.push_back(m);
    };
    for (const auto& m : dev.modes) {
        if (!supported_format(m.subtype)) continue;
        const bool p1080 = m.width == 1920 && m.height == 1080;
        const bool p720 = m.width == 1280 && m.height == 720;
        if (args.has("all-modes") || ((p1080 || p720) && m.fps() >= 49.0)) add(m);
    }
    if (candidates.empty()) {
        for (const char* fmt : {"NV12", "YUY2", "MJPG"}) {
            CaptureMode m;
            if (choose_capture_mode(dev, 1920, 1080, 60, fmt, m) && subtype_name(m.subtype) == fmt) add(m);
        }
        if (!candidates.empty())
            std::printf("The device offers no 1080p60/720p60 mode; benchmarking its closest modes instead.\n\n");
    }
    if (candidates.empty()) {
        std::printf("The device exposes no mode in a format Hikari can capture (NV12/YUY2/MJPG/RGB32).\n");
        j.value("picked", picked).end_object();
        if (args.has("json")) j.save(args.get("json"));
        return 1;
    }

    GpuDevice gpu;
    if (!gpu.create()) {
        std::fprintf(stderr, "Could not create a D3D11 device.\n");
        return 1;
    }

    std::vector<Bench> results;
    for (const auto& m : candidates) {
        std::printf("  capturing %-28s for %.0f s ...\n", mode_label(m).c_str(), seconds);
        std::fflush(stdout);
        results.push_back(run_mode(gpu, dev, m, seconds));
    }
    std::printf("\n");

    Table t({"Mode", "fps", "intv p50", "p95", "p99", "max ms", "driver p50", "p99 ms", "CPU %", "Signal",
             "Status"});
    for (const auto& b : results) {
        std::string status = !b.opened ? "open failed" : b.failed ? "stream error" : b.frames == 0 ? "no frames"
                             : b.real_rate()                                             ? "ok"
                                                                                         : "low fps";
        const bool iv = b.interval.count > 0;
        const bool dv = b.driver.count > 0;
        t.row({mode_label(b.mode), b.opened ? strf("%.2f", b.fps) : "", iv ? ms(b.interval.p50) : "",
               iv ? ms(b.interval.p95) : "", iv ? ms(b.interval.p99) : "", iv ? ms(b.interval.max) : "",
               dv ? ms(b.driver.p50) : "n/a", dv ? ms(b.driver.p99) : "n/a", b.opened ? strf("%.1f", b.cpu) : "",
               b.opened ? (b.signal ? "picture" : "none") : "", status});
    }
    t.print();
    std::printf("  (interval = frame-to-frame time at our callback; driver = device timestamp -> callback)\n\n");

    // Recommendation: real 60 fps first, then 1080p, then uncompressed, then
    // the lowest p99 frame interval (the jitter the stream will inherit).
    const Bench* best = nullptr;
    auto better = [](const Bench& a, const Bench& b) {
        if (a.real_rate() != b.real_rate()) return a.real_rate();
        const bool a60 = a.nominal_fps >= 59, b60 = b.nominal_fps >= 59;
        if (a60 != b60) return a60;
        const uint64_t apx = uint64_t(a.mode.width) * a.mode.height, bpx = uint64_t(b.mode.width) * b.mode.height;
        if (apx != bpx) return apx > bpx;
        if (uncompressed(a.mode.subtype) != uncompressed(b.mode.subtype)) return uncompressed(a.mode.subtype);
        return a.interval.p99 < b.interval.p99;
    };
    for (const auto& b : results)
        if (b.opened && b.frames > 0 && (!best || better(b, *best))) best = &b;

    std::string pref;
    if (best) {
        // Format preference: the winner's format, then the other measured
        // formats at the same resolution by p99 interval, then the defaults.
        std::vector<std::pair<uint32_t, std::string>> others;
        for (const auto& b : results)
            if (&b != best && b.frames > 0 && b.mode.width == best->mode.width && b.mode.height == best->mode.height)
                others.push_back({uncompressed(b.mode.subtype) ? b.interval.p99 : b.interval.p99 + 1'000'000,
                                  subtype_name(b.mode.subtype)});
        std::sort(others.begin(), others.end());
        std::vector<std::string> order{subtype_name(best->mode.subtype)};
        for (const auto& o : others) order.push_back(o.second);
        for (const char* d : {"NV12", "YUY2", "MJPG"}) order.push_back(d);
        // Fallbacks: uncompressed before MJPEG unless MJPEG itself won.
        std::stable_partition(order.begin() + 1, order.end(), [](const std::string& f) { return f != "MJPG"; });
        for (const auto& f : order) {
            const auto parts = split(pref, ',');
            if (std::find(parts.begin(), parts.end(), f) == parts.end()) pref += (pref.empty() ? "" : ",") + f;
        }

        std::string why;
        if (best->real_rate())
            why = strf("delivers a real %.1f fps", best->fps);
        else
            why = strf("only %.1f of %.0f fps were delivered (check the USB 3 port / source refresh rate)", best->fps,
                       best->nominal_fps);
        why += uncompressed(best->mode.subtype) ? ", uncompressed (no MJPEG decode stage)"
                                                : ", MJPEG (no uncompressed mode sustained the rate; costs a decode)";
        why += strf(", frame-interval p99 %.2f ms", best->interval.p99 / 1000.0);
        if (!best->signal) why += "; no HDMI picture was detected, so re-run with the console awake to confirm";
        std::printf("Best mode: %ux%u@%.0f %s - use format preference %s\n  why: %s\n", best->mode.width,
                    best->mode.height, best->mode.fps(), subtype_name(best->mode.subtype).c_str(), pref.c_str(),
                    why.c_str());
    } else {
        std::printf("No mode delivered frames. Is the card in use by another app (OBS, Camera)?\n");
    }

    j.value("picked", picked);
    j.value("picked_name", dev.name);
    j.value("seconds_per_mode", seconds);
    j.begin_array("benchmarks");
    for (const auto& b : results) {
        j.begin_object()
            .value("mode", mode_label(b.mode))
            .value("width", b.mode.width)
            .value("height", b.mode.height)
            .value("nominal_fps", b.nominal_fps)
            .value("format", subtype_name(b.mode.subtype))
            .value("opened", b.opened)
            .value("stream_error", b.failed)
            .value("fps", b.fps)
            .value("frames", b.frames)
            .value("cpu_percent", b.cpu)
            .value("signal", b.signal)
            .value("mean_luma", static_cast<double>(b.luma));
        j.begin_object("interval_us")
            .value("p50", b.interval.p50)
            .value("p95", b.interval.p95)
            .value("p99", b.interval.p99)
            .value("max", b.interval.max)
            .end_object();
        j.begin_object("driver_us")
            .value("count", b.driver.count)
            .value("p50", b.driver.p50)
            .value("p95", b.driver.p95)
            .value("p99", b.driver.p99)
            .value("max", b.driver.max)
            .end_object();
        j.end_object();
    }
    j.end_array();
    if (best)
        j.begin_object("recommendation")
            .value("mode", mode_label(best->mode))
            .value("format_preference", pref)
            .value("real_rate", best->real_rate())
            .end_object();
    j.end_object();

    if (args.has("csv")) {
        if (t.save_csv(args.get("csv")))
            std::printf("\nSaved CSV to %s\n", args.get("csv").c_str());
        else
            std::fprintf(stderr, "Could not write %s\n", args.get("csv").c_str());
    }
    if (args.has("json")) {
        if (j.save(args.get("json")))
            std::printf("Saved JSON to %s\n", args.get("json").c_str());
        else
            std::fprintf(stderr, "Could not write %s\n", args.get("json").c_str());
    }
    return best ? 0 : 1;
}

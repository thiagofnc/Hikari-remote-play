// hikari-encoder-probe: benchmarks every hardware H.264/HEVC encoder on every
// GPU with the exact low-latency configuration the host uses, and marks the
// one the host would pick (select_best_encoder semantics).

#include <cstdio>
#include <string>
#include <vector>

#include "encoder.h"
#include "hikari/media.h"
#include "hikari/system.h"
#include "tool_common.h"

using namespace hikari;
using namespace hikari::tool;

namespace {

const char* kHelp = R"(hikari-encoder-probe - benchmark hardware video encoders per GPU

Usage: hikari-encoder-probe [options]

For every DXGI adapter, enumerates the hardware H.264 and HEVC encoder MFTs,
encodes synthetic GPU-resident NV12 frames paced at the target frame rate
(exactly like the host pipeline) and reports achieved fps, submit->output
latency p50/p95/p99/max and the average frame size. The encoder the host
would pick is marked with '*': it must sustain >= 95% of the frame rate and
has the lowest p95 + 0.25 * p99 latency.

Options:
  --codec h264|hevc|all   codecs to test (default all)
  --width N --height N    frame size (default 1920x1080)
  --fps N                 frame rate (default 60)
  --bitrate KBPS          CBR bitrate in kbps (default 12000)
  --seconds N             encode time per encoder (default 3; capped at 15)
  --adapter N             only test adapter index N (see list)
  --software              also try software encoder MFTs (they cannot take
                          GPU textures, so the host cannot use them)
  --json FILE             save results as JSON
  --csv FILE              save the result table as CSV
  --verbose               show library log output
  --help                  this text
)";

std::string luid_str(const LUID& l) { return strf("%08lx:%08lx", static_cast<unsigned long>(l.HighPart), l.LowPart); }
bool luid_zero(const LUID& l) { return l.HighPart == 0 && l.LowPart == 0; }
bool luid_eq(const LUID& a, const LUID& b) { return a.HighPart == b.HighPart && a.LowPart == b.LowPart; }

struct Result {
    int adapter = 0;
    std::string adapter_name;
    std::string codec;
    std::string encoder;
    std::string clsid;
    bool hardware = false;
    std::string vendor;
    std::string status;  // "ok", "too slow", "failed", "skipped: ..."
    EncoderBenchmark bench;
    double score = 0;
    bool winner = false;
};

}  // namespace

int main(int argc, char** argv)
{
    Args args(argc, argv, {"codec", "width", "height", "fps", "bitrate", "seconds", "adapter", "json", "csv"},
              {"software", "verbose"});
    if (args.has("help")) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    init_console(args.has("verbose"));

    EncoderConfig cfg;
    cfg.width = static_cast<UINT>(args.num("width", 1920));
    cfg.height = static_cast<UINT>(args.num("height", 1080));
    cfg.fps = static_cast<UINT>(args.num("fps", 60));
    cfg.bitrate_kbps = static_cast<UINT>(args.num("bitrate", 12000));
    const double seconds = std::clamp(args.num("seconds", 3), 0.5, 14.0);
    const int only_adapter = static_cast<int>(args.num("adapter", -1));
    const std::string codec_arg = lower(args.get("codec", "all"));
    if (!args.error().empty() || cfg.width < 64 || cfg.height < 64 || cfg.fps == 0 || (cfg.width % 2) ||
        (cfg.height % 2) || (codec_arg != "all" && codec_arg != "h264" && codec_arg != "hevc")) {
        std::fprintf(stderr, "error: %s\nRun with --help for usage.\n",
                     args.error().empty() ? "invalid arguments" : args.error().c_str());
        return 64;
    }
    const int frames = std::max(30, static_cast<int>(seconds * cfg.fps));

    apply_process_performance_policy();
    set_thread_role(ThreadRole::Video, "encoder-probe");
    MediaRuntime mf;
    if (!mf.ok()) {
        std::fprintf(stderr, "Media Foundation is not available on this system.\n");
        return 1;
    }

    struct Codec {
        const char* name;
        GUID guid;
    };
    std::vector<Codec> codecs;
    if (codec_arg != "hevc") codecs.push_back({"H.264", MFVideoFormat_H264});
    if (codec_arg != "h264") codecs.push_back({"HEVC", MFVideoFormat_HEVC});

    std::printf("Hikari encoder probe: %ux%u @ %u fps, %u kbps CBR, %d frames per encoder\n\n", cfg.width,
                cfg.height, cfg.fps, cfg.bitrate_kbps, frames);

    auto adapters = enumerate_adapters();
    if (adapters.empty()) {
        std::printf("No hardware DXGI adapters found.\n");
        return 1;
    }
    std::printf("GPUs (index 0 is the high-performance adapter the host uses by default):\n");
    for (size_t i = 0; i < adapters.size(); ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapters[i]->GetDesc1(&d);
        std::printf("  [%zu] %s (%s, %llu MB VRAM, LUID %s)\n", i, utf8(d.Description).c_str(),
                    vendor_name(d.VendorId), static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20),
                    luid_str(d.AdapterLuid).c_str());
    }
    std::printf("\n");

    std::vector<Result> results;
    for (size_t ai = 0; ai < adapters.size(); ++ai) {
        if (only_adapter >= 0 && static_cast<size_t>(only_adapter) != ai) continue;
        GpuDevice gpu;
        if (!gpu.create(adapters[ai].Get())) {
            std::printf("  [%zu] could not create a D3D11 device, skipped\n", ai);
            continue;
        }
        for (const auto& codec : codecs) {
            auto encoders = enumerate_encoders(codec.guid, args.has("software"));
            if (encoders.empty()) continue;
            EncoderConfig ccfg = cfg;
            ccfg.codec = codec.guid;
            size_t first_new = results.size();
            for (auto& e : encoders) {
                Result r;
                r.adapter = static_cast<int>(ai);
                r.adapter_name = gpu.adapter_name;
                r.codec = codec.name;
                r.encoder = e.name;
                r.clsid = utf8(e.clsid_str.c_str());
                r.hardware = e.hardware;
                r.vendor = e.vendor_id ? vendor_name(e.vendor_id) : "";
                // A hardware MFT is bound to one GPU; feeding it textures from
                // another adapter fails (or silently copies through system RAM).
                // enumerate_encoders() rarely gets a LUID (MFTEnumEx does not
                // set MFT_ENUM_ADAPTER_LUID), so fall back to the vendor ID.
                const bool other_gpu = !luid_zero(e.adapter_luid) ? !luid_eq(e.adapter_luid, gpu.adapter_luid)
                                                                  : (e.vendor_id && e.vendor_id != gpu.vendor_id);
                if (e.hardware && other_gpu) {
                    r.status = "skipped: other GPU";
                    results.push_back(r);
                    continue;
                }
                if (!e.hardware && ai != 0 && only_adapter < 0) {
                    r.status = "skipped: software (tested on GPU 0)";
                    results.push_back(r);
                    continue;
                }
                std::printf("  benchmarking [%zu] %-6s %s ...\n", ai, codec.name, e.name.c_str());
                std::fflush(stdout);
                r.bench = benchmark_encoder(gpu, e, ccfg, frames, true);
                if (!r.bench.ok)
                    r.status = e.hardware ? "failed" : "failed (no GPU input)";
                else if (r.bench.fps < cfg.fps * 0.95)
                    r.status = "too slow";
                else
                    r.status = "ok";
                r.score = r.bench.latency.p95 + 0.25 * r.bench.latency.p99;
                results.push_back(r);
            }
            // Winner per adapter+codec, same rule as select_best_encoder().
            int best = -1;
            for (size_t i = first_new; i < results.size(); ++i)
                if (results[i].status == "ok" && (best < 0 || results[i].score < results[static_cast<size_t>(best)].score))
                    best = static_cast<int>(i);
            if (best >= 0) results[static_cast<size_t>(best)].winner = true;
        }
    }

    std::printf("\n");
    Table t({"", "GPU", "Codec", "Encoder", "Status", "fps", "p50 ms", "p95 ms", "p99 ms", "max ms", "avg KB/frame"});
    for (const auto& r : results) {
        const bool ran = r.status.rfind("skipped", 0) != 0;
        const bool lat = ran && r.bench.latency.count > 0;
        t.row({r.winner ? "*" : "", strf("%d", r.adapter), r.codec, r.encoder, r.status,
               ran ? strf("%.1f", r.bench.fps) : "", lat ? ms(r.bench.latency.p50) : "",
               lat ? ms(r.bench.latency.p95) : "", lat ? ms(r.bench.latency.p99) : "",
               lat ? ms(r.bench.latency.max) : "",
               ran && r.bench.avg_frame_bytes ? strf("%.1f", r.bench.avg_frame_bytes / 1024.0) : ""});
    }
    if (t.empty())
        std::printf("No video encoders found.\n");
    else
        t.print();

    std::printf("\n");
    for (const auto& codec : codecs) {
        const Result* host_pick = nullptr;
        const Result* overall = nullptr;
        for (const auto& r : results) {
            if (r.codec != codec.name || !r.winner) continue;
            if (r.adapter == 0) host_pick = &r;
            if (!overall || r.score < overall->score) overall = &r;
        }
        if (host_pick)
            std::printf("%-5s host pick (GPU 0): %s - p95 %.2f ms, p99 %.2f ms at %.1f fps\n", codec.name,
                        host_pick->encoder.c_str(), host_pick->bench.latency.p95 / 1000.0,
                        host_pick->bench.latency.p99 / 1000.0, host_pick->bench.fps);
        else
            std::printf("%-5s no encoder on GPU 0 sustains %u fps\n", codec.name, cfg.fps);
        if (overall && overall != host_pick)
            std::printf("      faster option on GPU %d (%s): %s - p95 %.2f ms\n", overall->adapter,
                        overall->adapter_name.c_str(), overall->encoder.c_str(), overall->bench.latency.p95 / 1000.0);
    }

    if (args.has("csv")) {
        if (t.save_csv(args.get("csv")))
            std::printf("\nSaved CSV to %s\n", args.get("csv").c_str());
        else
            std::fprintf(stderr, "Could not write %s\n", args.get("csv").c_str());
    }
    if (args.has("json")) {
        JsonWriter j;
        j.begin_object();
        j.begin_object("config")
            .value("width", cfg.width)
            .value("height", cfg.height)
            .value("fps", cfg.fps)
            .value("bitrate_kbps", cfg.bitrate_kbps)
            .value("frames", frames)
            .end_object();
        j.begin_array("results");
        for (const auto& r : results) {
            j.begin_object()
                .value("adapter", r.adapter)
                .value("adapter_name", r.adapter_name)
                .value("codec", r.codec)
                .value("encoder", r.encoder)
                .value("clsid", r.clsid)
                .value("hardware", r.hardware)
                .value("vendor", r.vendor)
                .value("status", r.status)
                .value("winner", r.winner)
                .value("fps", r.bench.fps)
                .value("avg_frame_bytes", r.bench.avg_frame_bytes);
            j.begin_object("latency_us")
                .value("count", r.bench.latency.count)
                .value("p50", r.bench.latency.p50)
                .value("p95", r.bench.latency.p95)
                .value("p99", r.bench.latency.p99)
                .value("max", r.bench.latency.max)
                .value("mean", r.bench.latency.mean)
                .end_object();
            j.end_object();
        }
        j.end_array();
        j.end_object();
        if (j.save(args.get("json")))
            std::printf("Saved JSON to %s\n", args.get("json").c_str());
        else
            std::fprintf(stderr, "Could not write %s\n", args.get("json").c_str());
    }
    return 0;
}

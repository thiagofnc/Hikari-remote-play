#include "host_app.h"

#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "hikari/clock.h"
#include "hikari/crypto.h"
#include "hikari/invite.h"
#include "hikari/log.h"
#include "hikari/system.h"
#include "hikari/tailscale.h"

namespace hikari {

using proto::ConsoleState;
using proto::HostIssue;
using proto::SessionPhase;

namespace {

constexpr uint64_t kTick = 50'000;
constexpr uint64_t kWakeTimeout = 35'000'000;
constexpr uint64_t kControllerTimeout = 6'000'000;
constexpr uint64_t kEncoderWaitTimeout = 45'000'000;
constexpr uint64_t kClientGrace = 15'000'000;
constexpr uint64_t kVideoStallTimeout = 10'000'000;

// Other threads stamp times after a tick reads `now`; a later stamp is "just happened", not an unsigned wraparound.
uint64_t elapsed_since(uint64_t now, uint64_t t) { return now > t ? now - t : 0; }

std::string to_hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

bool from_hex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        if (std::sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

bool same_luid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
bool zero_luid(const LUID& a) { return a.LowPart == 0 && a.HighPart == 0; }

std::vector<EncoderInfo> encoders_for(const GpuDevice& gpu, const GUID& codec)
{
    std::vector<EncoderInfo> out;
    for (auto& e : enumerate_encoders(codec))
        if (zero_luid(e.adapter_luid) || same_luid(e.adapter_luid, gpu.adapter_luid)) out.push_back(e);
    return out;
}

double spread_for(proto::Preset p)
{
    switch (p) {
    case proto::Preset::MinimumLatency: return 16;
    case proto::Preset::Balanced: return 10;
    case proto::Preset::Quality: return 6;
    }
    return 10;
}

UINT quality_for(proto::Preset p)
{
    switch (p) {
    // Hardware encoders finish 1080p well inside a frame even at their better
    // settings, which buy visibly finer detail at the same bitrate.
    case proto::Preset::MinimumLatency: return 0;
    case proto::Preset::Balanced: return 50;
    case proto::Preset::Quality: return 80;
    }
    return 50;
}

}  // namespace

std::vector<HostAddress> local_addresses()
{
    std::vector<HostAddress> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, aa, &size) != NO_ERROR) return out;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const std::string name = utf8(a->FriendlyName);
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            const uint32_t v = ntohl(sin->sin_addr.s_addr);
            if ((v >> 16) == 0xA9FE) continue;  // 169.254/16 link-local
            HostAddress h;
            h.ip = ip;
            h.adapter = name;
            h.tailscale = is_tailscale_adapter(a->FriendlyName, a->Description) && is_tailscale_ipv4(ip);
            out.push_back(h);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.tailscale > y.tailscale; });
    return out;
}

HostApp::HostApp() = default;

HostApp::~HostApp() { stop(); }

bool HostApp::start(Config& cfg)
{
    cfg_ = &cfg;
    media_owned_ = std::make_unique<MediaRuntime>();
    apply_process_performance_policy();
    // A host is a server: never let the laptop sleep while Hikari runs.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

    pairing_code_ = cfg.get("host", "pairing_code");
    if (crypto::normalize_pairing_code(pairing_code_).size() < 12) {
        pairing_code_ = crypto::generate_pairing_code();
        cfg.set("host", "pairing_code", pairing_code_);
    }
    if (!from_hex(cfg.get("host", "host_id"), host_id_, sizeof(host_id_))) {
        crypto::random_bytes(host_id_, sizeof(host_id_));
        cfg.set("host", "host_id", to_hex(host_id_, sizeof(host_id_)));
    }
    host_name_ = cfg.get("host", "name", computer_name());
    port_ = static_cast<uint16_t>(cfg.get_int("host", "port", proto::kDefaultPort));
    cfg.save();

    apply_preset(proto::Preset::Balanced, 0);
    // The Switch 2 only takes the Pro Controller; "hori" keeps the handshake-free pad for a Switch 1.
    bridge_.start(cfg.get("host", "bridge_port"), static_cast<uint32_t>(cfg.get_int("host", "bridge_baud", HK_LINK_DEFAULT_BAUD)),
                  cfg.get("host", "controller_type", "pro") == "hori" ? HK_PROFILE_HORI_PAD : HK_PROFILE_PRO_CONTROLLER);

    HostNetEvents ev;
    ev.connected = [this](const std::string&, proto::Preset preset, uint32_t max_kbps, uint8_t video_codecs) {
        client_gone_at_ = 0;
        // A client that changes the usable codec (e.g. the phone taking over
        // from a PC) needs the encoder reopened before it gets a frame.
        const bool hevc = (video_codecs & 2) != 0;
        if (client_hevc_.exchange(hevc) != hevc && streaming_) want_reconfigure_ = true;
        apply_preset(preset, max_kbps);
        if (streaming_) {
            encoder_.request_keyframe();  // reconnect inside the grace period: resume instantly
            recovery_next_ = true;
        }
        wake_.set();
    };
    ev.disconnected = [this] {
        bridge_.publish_neutral();
        client_gone_at_ = now_us();
        wake_.set();
    };
    ev.input = [this](const proto::Input& in, uint64_t) {
        // Hot path: net-rx thread straight into the bridge mailbox.
        if (input_enabled_.load(std::memory_order_relaxed))
            bridge_.publish(in.pad, (in.flags & proto::kInputLatencyMark) != 0);
    };
    ev.keyframe_request = [this](uint8_t) {
        const uint64_t now = now_us();
        if (now - last_keyframe_request_us_ < 100'000) return;  // a burst of losses needs one IDR
        last_keyframe_request_us_ = now;
        encoder_.request_keyframe();
        recovery_next_ = true;
    };
    ev.receiver_report = [this](const proto::ReceiverReport& rr) {
        std::lock_guard<std::mutex> lk(abr_mu_);
        const AbrDecision d = abr_.on_report(rr, now_us());
        abr_now_ = d;
        if (d.bitrate_changed) pending_bitrate_ = d.bitrate_kbps;
        net_.set_pacing(spread_for(preset_), d.bitrate_kbps, d.mode.fps);
        if (d.mode_changed) {
            HK_LOGI("ABR: switching to %ux%u@%u", d.mode.width, d.mode.height, d.mode.fps);
            want_reconfigure_ = true;
        }
    };
    ev.play = [this](const proto::Play&) {
        play_requested_ = true;
        wake_.set();
    };
    ev.stop = [this] {
        stop_requested_ = true;
        wake_.set();
    };
    ev.power = [this](const proto::Power& p) {
        (p.action == proto::PowerAction::Sleep ? sleep_requested_ : manual_wake_) = true;
        wake_.set();
    };
    ev.preset = [this](const proto::SetPreset& p) { apply_preset(p.preset, p.max_bitrate_kbps); };
    ev.sender_overflow = [this] {
        encoder_.request_keyframe();
        recovery_next_ = true;
    };
    if (!net_.start(port_, crypto::psk_from_code(pairing_code_), host_name_, host_id_, std::move(ev))) {
        HK_LOGE("Could not open UDP port %u (is another Hikari host running?)", port_);
        return false;
    }

    running_ = true;
    setup_thread_ = std::thread(&HostApp::setup_thread, this);
    encoder_thread_ = std::thread(&HostApp::encoder_thread, this);
    orchestrator_thread_ = std::thread(&HostApp::orchestrator_thread, this);
    sysinfo_thread_ = std::thread(&HostApp::sysinfo_thread, this);

    // A host must survive reboots: start with Windows unless the user opted out.
    if (cfg.get("host", "autostart").empty()) {
        cfg.set_bool("host", "autostart", set_autostart(true));
        cfg.save();
    } else if (cfg.get_bool("host", "autostart", true) && !autostart_enabled()) {
        set_autostart(true);  // Hikari.exe moved since the entry was written
    }
    return true;
}

void HostApp::sysinfo_thread()
{
    // Slow-changing facts the host window shows. Some calls block (Tailscale
    // CLI, firewall COM), so they live here instead of in snapshot().
    set_thread_role(ThreadRole::Background, "hikari-sysinfo");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (running_) {
        const std::string name = tailscale_dns_name();
        const FirewallStatus fw = firewall_status(port_);
        const int lid = lid_close_action();
        {
            std::lock_guard<std::mutex> lk(sysinfo_mu_);
            tailscale_name_ = name;
            firewall_ = fw;
            lid_action_ = lid;
        }
        sysinfo_wake_.wait(15000);
    }
    CoUninitialize();
}

bool HostApp::allow_through_firewall()
{
    const bool ok = run_elevated_setup("firewall", port_);
    sysinfo_wake_.set();
    return ok;
}

bool HostApp::keep_awake_with_lid_closed()
{
    const bool ok = keep_awake_on_lid_close(true);
    sysinfo_wake_.set();
    return ok;
}

bool HostApp::set_start_with_windows(bool on)
{
    const bool ok = set_autostart(on);
    if (ok) {
        cfg_->set_bool("host", "autostart", on);
        cfg_->save();
    }
    return ok;
}

void HostApp::stop()
{
    if (!running_.exchange(false)) return;
    wake_.set();
    enc_wake_.set();
    sysinfo_wake_.set();
    if (setup_thread_.joinable()) setup_thread_.join();
    if (sysinfo_thread_.joinable()) sysinfo_thread_.join();
    if (orchestrator_thread_.joinable()) orchestrator_thread_.join();
    if (encoder_thread_.joinable()) encoder_thread_.join();
    bridge_.publish_neutral();
    Sleep(30);  // let the neutral frame reach the bridge
    audio_.stop();
    capture_.close();
    net_.stop();
    bridge_.stop();
    SetThreadExecutionState(ES_CONTINUOUS);
}

void HostApp::regenerate_pairing_code()
{
    pairing_code_ = crypto::generate_pairing_code();
    cfg_->set("host", "pairing_code", pairing_code_);
    cfg_->save();
    // Takes effect immediately; the connected client keeps its session, new
    // connections need the new code.
    net_.set_psk(crypto::psk_from_code(pairing_code_));
    HK_LOGI("New pairing code generated");
}

void HostApp::redetect_devices()
{
    cfg_->set("host", "encoder", "");
    cfg_->set("host", "encoder_gpu", "");
    cfg_->set("host", "encoder_hevc", "");
    cfg_->set("host", "encoder_hevc_gpu", "");
    cfg_->save();
    capture_ok_ = false;
    last_capture_retry_us_ = 0;
    wake_.set();
}

void HostApp::wake_console()
{
    manual_wake_ = true;
    wake_.set();
}

void HostApp::apply_preset(proto::Preset p, uint32_t max_kbps)
{
    std::lock_guard<std::mutex> lk(abr_mu_);
    preset_ = p;
    if (max_kbps) client_max_kbps_ = max_kbps;
    VideoMode native;
    const CaptureMode& m = capture_.mode();
    if (m.width && m.height) {
        native.width = static_cast<uint16_t>(std::min<UINT32>(m.width, 1920));
        native.height = static_cast<uint16_t>(std::min<UINT32>(m.height, 1080));
        native.fps = static_cast<uint16_t>(std::min(60.0, std::max(30.0, m.fps() + 0.5)));
    }
    const VideoMode before = abr_now_.mode;
    abr_.reset(AdaptiveBitrate::preset_config(p, client_max_kbps_), native);
    abr_now_ = abr_.current();
    pending_bitrate_ = abr_now_.bitrate_kbps;
    net_.set_pacing(spread_for(p), abr_now_.bitrate_kbps, abr_now_.mode.fps);
    if (streaming_ && before != abr_now_.mode) want_reconfigure_ = true;
    HK_LOGI("Preset: %s (start %u kbps, max %u kbps)", proto::to_string(p), abr_now_.bitrate_kbps,
            AdaptiveBitrate::preset_config(p, client_max_kbps_).max_kbps);
}

// ---- device setup ----------------------------------------------------------

bool HostApp::select_encoder()
{
    // H.264 is what every client decodes; HEVC is an extra (select_hevc_encoder).
    const GUID codec = MFVideoFormat_H264;
    const std::string cached = cfg_->get("host", "encoder");
    const std::string cached_gpu = cfg_->get("host", "encoder_gpu");
    auto adapters = enumerate_adapters();

    if (!cached.empty()) {
        for (auto& a : adapters) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (utf8(d.Description) != cached_gpu) continue;
            if (!gpu_.create(a.Get())) break;
            encoders_ = encoders_for(gpu_, codec);
            for (size_t i = 0; i < encoders_.size(); ++i)
                if (encoders_[i].name == cached) encoder_index_ = static_cast<int>(i);
            if (encoder_index_ >= 0) return true;
            break;
        }
        HK_LOGW("Cached encoder \"%s\" not available any more; re-benchmarking", cached.c_str());
    }

    // First run (or hardware changed): measure every hardware encoder on every
    // GPU and keep the one with the best tail latency at 1080p60.
    benchmarking_ = true;
    EncoderConfig bench;
    bench.codec = codec;
    int best_adapter = -1;
    std::string best_name;
    double best_score = 1e18;
    for (size_t ai = 0; ai < adapters.size(); ++ai) {
        GpuDevice g;
        if (!g.create(adapters[ai].Get())) continue;
        auto encs = encoders_for(g, codec);
        std::vector<EncoderBenchmark> results;
        const int idx = select_best_encoder(g, encs, bench, &results);
        if (idx < 0) continue;
        const auto& r = results[static_cast<size_t>(idx)];
        const double score = r.latency.p95 + 0.25 * r.latency.p99;
        if (score < best_score) {
            best_score = score;
            best_adapter = static_cast<int>(ai);
            best_name = r.name;
        }
    }
    benchmarking_ = false;
    if (best_adapter < 0) {
        HK_LOGE("No usable hardware video encoder found");
        gpu_.create(adapters.empty() ? nullptr : adapters.front().Get());
        return false;
    }
    gpu_.create(adapters[static_cast<size_t>(best_adapter)].Get());
    encoders_ = encoders_for(gpu_, codec);
    for (size_t i = 0; i < encoders_.size(); ++i)
        if (encoders_[i].name == best_name) encoder_index_ = static_cast<int>(i);
    cfg_->set("host", "encoder", best_name);
    cfg_->set("host", "encoder_gpu", gpu_.adapter_name);
    cfg_->save();
    HK_LOGI("Selected encoder: %s on %s", best_name.c_str(), gpu_.adapter_name.c_str());
    return encoder_index_ >= 0;
}

// HEVC is a second encoder on the GPU picked for H.264, used only for clients
// that offer it. host.codec = h264 turns it off.
void HostApp::select_hevc_encoder()
{
    hevc_encoders_.clear();
    hevc_index_ = -1;
    if (encoder_index_ < 0 || cfg_->get("host", "codec", "auto") == "h264") return;
    const std::string cached = cfg_->get("host", "encoder_hevc");
    if (!cached.empty() && cfg_->get("host", "encoder_hevc_gpu") == gpu_.adapter_name) {
        if (cached == "none") return;
        hevc_encoders_ = encoders_for(gpu_, MFVideoFormat_HEVC);
        for (size_t i = 0; i < hevc_encoders_.size(); ++i)
            if (hevc_encoders_[i].name == cached) hevc_index_ = static_cast<int>(i);
        if (hevc_index_ >= 0) return;
    }

    benchmarking_ = true;
    EncoderConfig bench;
    bench.codec = MFVideoFormat_HEVC;
    std::string best;
    {
        auto encs = encoders_for(gpu_, MFVideoFormat_HEVC);
        const int idx = select_best_encoder(gpu_, encs, bench, nullptr);
        if (idx >= 0) best = encs[static_cast<size_t>(idx)].name;
    }
    benchmarking_ = false;
    if (!best.empty()) {
        hevc_encoders_ = encoders_for(gpu_, MFVideoFormat_HEVC);
        for (size_t i = 0; i < hevc_encoders_.size(); ++i)
            if (hevc_encoders_[i].name == best) hevc_index_ = static_cast<int>(i);
    }
    cfg_->set("host", "encoder_hevc", hevc_index_ >= 0 ? best : "none");
    cfg_->set("host", "encoder_hevc_gpu", gpu_.adapter_name);
    cfg_->save();
    if (hevc_index_ >= 0)
        HK_LOGI("HEVC encoder: %s (for clients that decode HEVC)", best.c_str());
    else
        HK_LOGI("No hardware HEVC encoder keeps up at 1080p60 on %s; streaming H.264 only", gpu_.adapter_name.c_str());
}

bool HostApp::open_capture()
{
    auto devs = enumerate_capture_devices(true);
    const std::string want = cfg_->get("host", "capture_device");
    const CaptureDeviceInfo* pick = nullptr;
    int best = -1000;
    for (const auto& d : devs) {
        // The remembered device only breaks ties: an earlier run may have
        // saved a webcam while the capture card was unplugged.
        const int s = capture_device_score(d) + (!want.empty() && d.name == want ? 25 : 0);
        if (s > best) {
            best = s;
            pick = &d;
        }
    }
    if (!pick || best < 0) {
        capture_ok_ = false;
        return false;
    }
    CaptureMode mode;
    const std::string pref = cfg_->get("host", "capture_formats", "NV12,YUY2,RGB32,MJPG");
    if (!choose_capture_mode(*pick, 1920, 1080, 60, pref, mode)) {
        HK_LOGW("%s offers no usable video mode", pick->name.c_str());
        capture_ok_ = false;
        return false;
    }
    if (mode.width != 1920 || mode.height != 1080 || mode.fps() < 59) {
        std::string all;
        for (const auto& m : pick->modes) all += "\n    " + m.to_string();
        HK_LOGW("%s has no usable 1920x1080 at 60 fps mode; using %s. Modes offered:%s", pick->name.c_str(),
                mode.to_string().c_str(), all.c_str());
    }
    capture_.set_upload(streaming_.load());
    capture_.set_frame_callback([this](CapturedFrame&& f) { on_captured(std::move(f)); });
    if (!capture_.open(gpu_, *pick, mode)) {
        capture_ok_ = false;
        return false;
    }
    capture_dev_ = *pick;
    capture_ok_ = true;
    cfg_->set("host", "capture_device", pick->name);

    auto eps = enumerate_audio_capture_endpoints();
    int ai = -1;
    const std::string want_audio = cfg_->get("host", "audio_device");
    for (size_t i = 0; i < eps.size(); ++i)
        if (!want_audio.empty() && eps[i].name == want_audio) ai = static_cast<int>(i);
    if (ai < 0) ai = match_audio_endpoint(eps, pick->symlink, pick->name);
    audio_found_ = ai >= 0;
    if (audio_found_) {
        audio_ep_ = eps[static_cast<size_t>(ai)];
        HK_LOGI("Audio endpoint: %s", audio_ep_.name.c_str());
    } else {
        HK_LOGW("No audio endpoint matched the capture card; streaming video only");
    }
    cfg_->save();
    apply_preset(preset_, 0);  // native mode follows the capture mode
    return true;
}

void HostApp::setup_thread()
{
    set_thread_role(ThreadRole::Background, "hikari-setup");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!select_encoder()) encoder_index_ = -1;
    select_hevc_encoder();
    gpu_ready_ = true;
    if (!open_capture()) HK_LOGW("No HDMI capture card found yet; will keep looking");
    last_capture_retry_us_ = now_us();
    CoUninitialize();
}

// ---- video pipeline ---------------------------------------------------------

void HostApp::on_captured(CapturedFrame&& f)
{
    if (!streaming_.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lk(convert_mu_, std::try_to_lock);
    if (!lk.owns_lock() || !converter_ready_) return;
    if (converter_.width() && (f.index & 1) && abr_fps_ <= 30) return;  // 30 fps step: every other frame
    ID3D11Texture2D* nv12 = converter_.convert(f.texture.Get(), f.subresource, f.width, f.height);
    if (!nv12) return;
    converted_frames_.fetch_add(1, std::memory_order_relaxed);
    EncodeJob job;
    job.texture = nv12;
    job.capture_us = f.capture_us;
    job.index = f.index;
    // Depth-1 mailbox: if the encoder is still busy the older frame is
    // replaced (and counted); we never queue stale frames.
    encode_mailbox_.put(std::move(job));
}

void HostApp::on_encoded(EncodedFrame&& f)
{
    // Frames still in flight when an H.264-only client took over.
    if (encoder_.config().codec == MFVideoFormat_HEVC && !client_hevc_.load(std::memory_order_relaxed)) return;
    encode_fps_.add();
    last_encoded_us_.store(now_us(), std::memory_order_relaxed);
    OutgoingFrame o;
    o.frame_id = stream_frame_id_.fetch_add(1) + 1;
    o.keyframe = f.keyframe;
    o.recovery = f.keyframe && recovery_next_.exchange(false);
    o.capture_us = f.capture_us;
    o.encode_us = static_cast<uint16_t>(std::min<uint64_t>(f.done_us - f.submit_us, 0xFFFF));
    o.fec_group = abr_fec_.load(std::memory_order_relaxed);
    o.data = std::move(f.data);
    net_.send_video(std::move(o));
}

// A driver update, crash (TDR) or GPU power switch removes the D3D device, and
// everything made from it fails from then on. Build a fresh device on the same
// adapter and reopen the capture card on it.
bool HostApp::recover_gpu()
{
    std::lock_guard<std::mutex> g(gpu_mu_);
    gpu_ready_ = false;
    if (gpu_.device)
        HK_LOGW("GPU device lost (%s); recreating it", hresult_string(gpu_.device->GetDeviceRemovedReason()).c_str());
    encoder_.close();
    {
        std::lock_guard<std::mutex> lk(convert_mu_);
        converter_.reset();
        converter_ready_ = false;
    }
    capture_.close();
    capture_ok_ = false;

    const std::string encoder_name = encoder_index_ >= 0 ? encoders_[static_cast<size_t>(encoder_index_)].name : cfg_->get("host", "encoder");
    const std::string hevc_name = hevc_index_ >= 0 ? hevc_encoders_[static_cast<size_t>(hevc_index_)].name : std::string();
    encoder_index_ = -1;
    hevc_index_ = -1;
    ComPtr<IDXGIAdapter1> same;
    for (auto& a : enumerate_adapters()) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (utf8(d.Description) == gpu_.adapter_name) same = a;
    }
    if (!same || !gpu_.create(same.Get())) {
        HK_LOGE("Could not recreate the GPU device");
        return false;
    }
    encoders_ = encoders_for(gpu_, MFVideoFormat_H264);
    for (size_t i = 0; i < encoders_.size(); ++i)
        if (encoders_[i].name == encoder_name) encoder_index_ = static_cast<int>(i);
    if (encoder_index_ < 0) {
        HK_LOGE("Encoder \"%s\" is gone after the GPU reset", encoder_name.c_str());
        return false;
    }
    if (!hevc_name.empty()) {
        hevc_encoders_ = encoders_for(gpu_, MFVideoFormat_HEVC);
        for (size_t i = 0; i < hevc_encoders_.size(); ++i)
            if (hevc_encoders_[i].name == hevc_name) hevc_index_ = static_cast<int>(i);
    }
    hevc_failed_ = false;
    last_capture_retry_us_ = 0;  // the orchestrator reopens the capture card right away
    gpu_ready_ = true;
    wake_.set();
    return true;
}

void HostApp::encoder_thread()
{
    set_thread_role(ThreadRole::Video, "hikari-encoder");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failures = 0;
    uint64_t retry_at = 0;
    bool gpu_lost = false;
    uint64_t gpu_retry_at = 0;
    while (running_) {
        HANDLE hs[3] = {encode_mailbox_.event(), encoder_.event(), enc_wake_.handle()};
        WaitForMultipleObjects(3, hs, FALSE, 10);

        if ((gpu_ready_ && FAILED(gpu_.device->GetDeviceRemovedReason())) || (gpu_lost && now_us() >= gpu_retry_at)) {
            gpu_lost = !recover_gpu();
            failures = 0;
            encoder_failures_ = gpu_lost ? 5 : 0;
            if (gpu_lost) gpu_retry_at = now_us() + 5'000'000;  // the driver may still be resetting
            want_reconfigure_ = true;
        }

        const bool want = streaming_.load() && gpu_ready_.load() && encoder_index_ >= 0;
        if (!want) {
            failures = 0;
            if (encoder_.is_open()) {
                encoder_.close();
                std::lock_guard<std::mutex> lk(convert_mu_);
                converter_.reset();
                converter_ready_ = false;
            }
            encode_mailbox_.take();
            continue;
        }

        if (!encoder_.is_open() || want_reconfigure_.load() || encoder_.failed()) {
            if (now_us() < retry_at) continue;
            if (encoder_.failed() && encoder_.config().codec == MFVideoFormat_HEVC && !hevc_failed_) {
                HK_LOGW("HEVC encoder failed mid-stream; using H.264 from now on");
                hevc_failed_ = true;
            }
            want_reconfigure_ = false;
            AbrDecision d;
            proto::Preset preset;
            {
                std::lock_guard<std::mutex> lk(abr_mu_);
                d = abr_now_;
                preset = preset_;
            }
            const bool hevc = client_hevc_ && hevc_index_ >= 0 && !hevc_failed_;
            const EncoderInfo& info = hevc ? hevc_encoders_[static_cast<size_t>(hevc_index_)]
                                           : encoders_[static_cast<size_t>(encoder_index_)];
            EncoderConfig ec;
            ec.codec = hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
            ec.width = d.mode.width;
            ec.height = d.mode.height;
            ec.fps = d.mode.fps;
            ec.bitrate_kbps = d.bitrate_kbps;
            ec.quality_vs_speed = quality_for(preset);
            {
                std::lock_guard<std::mutex> lk(convert_mu_);
                converter_ready_ = converter_.init(gpu_, ec.width, ec.height, ec.fps);
            }
            abr_fps_ = ec.fps;
            const bool ok = converter_ready_ &&
                            encoder_.open(gpu_, info, ec, [this](EncodedFrame&& f) { on_encoded(std::move(f)); });
            if (!ok && hevc) {
                HK_LOGW("HEVC encoder did not open; using H.264 from now on");
                hevc_failed_ = true;
                continue;
            }
            if (!ok) {
                ++failures;
                retry_at = now_us() + 1'000'000;
                encoder_failures_ = failures;
                HK_LOGE("Encoder open failed (%d)", failures);
                continue;
            }
            failures = 0;
            encoder_failures_ = 0;
            recovery_next_ = true;
            proto::StreamConfig sc;
            sc.codec = ec.codec == MFVideoFormat_HEVC ? proto::VideoCodec::HEVC : proto::VideoCodec::H264;
            sc.width = static_cast<uint16_t>(ec.width);
            sc.height = static_cast<uint16_t>(ec.height);
            sc.fps = static_cast<uint16_t>(ec.fps);
            sc.bitrate_kbps = ec.bitrate_kbps;
            sc.audio = audio_.running() ? audio_.codec() : proto::AudioCodec::None;
            sc.fec_group = d.fec_group;
            std::snprintf(sc.encoder_name, sizeof(sc.encoder_name), "%s", encoder_.name().c_str());
            net_.set_stream_config(sc);
        }

        if (const uint32_t br = pending_bitrate_.exchange(0)) encoder_.set_bitrate(br);
        {
            std::lock_guard<std::mutex> lk(abr_mu_);
            abr_fec_ = abr_now_.fec_group;
        }
        encoder_.pump();
        while (encoder_.can_accept()) {
            auto job = encode_mailbox_.take();
            if (!job) break;
            encoder_.submit(job->texture.Get(), job->index, job->capture_us);
            encoder_.pump();
        }
    }
    encoder_.close();
    CoUninitialize();
}

bool HostApp::start_stream()
{
    if (streaming_) return true;
    encoder_failures_ = 0;  // failures from an earlier Play must not end this one
    streaming_ = true;
    want_reconfigure_ = true;
    capture_.set_upload(true);
    if (audio_found_ && !audio_.running()) {
#ifdef HIKARI_WITH_OPUS
        const bool opus = cfg_->get_bool("host", "audio_opus", true);
#else
        const bool opus = false;
#endif
        audio_.start(audio_ep_, opus, [this](const AudioChunk& c) {
            proto::AudioPacket meta;
            meta.seq = audio_seq_.fetch_add(1);
            meta.capture_us = c.capture_us;
            meta.codec = c.codec;
            meta.channels = AudioCapture::kChannels;
            meta.samples = c.samples;
            net_.send_audio(meta, c.data, c.bytes);
        });
    }
    enc_wake_.set();
    HK_LOGI("Streaming started");
    return true;
}

void HostApp::stop_stream()
{
    if (!streaming_.exchange(false)) return;
    input_enabled_ = false;
    capture_.set_upload(false);
    audio_.stop();
    bridge_.publish_neutral();
    enc_wake_.set();
    HK_LOGI("Streaming stopped");
}

// ---- orchestration -------------------------------------------------------------

void HostApp::set_phase(SessionPhase p, const std::string& msg, HostIssue issue)
{
    std::lock_guard<std::mutex> lk(state_mu_);
    if (p != phase_) {
        phase_since_ = now_us();
        HK_LOGI("Phase: %s%s%s", proto::to_string(p), msg.empty() ? "" : " - ", msg.c_str());
    }
    phase_ = p;
    message_ = msg;
    issue_ = issue;
    input_enabled_ = p == SessionPhase::Streaming;
}

void HostApp::update_console_state(uint64_t)
{
    const BridgeStatus b = bridge_.status();
    const bool sig = capture_ok_ && capture_.signal_present();
    ConsoleState c = ConsoleState::Unknown;
    if (b.connected) {
        switch (b.usb_state) {
        case HK_USB_ACTIVE: c = sig ? ConsoleState::Ready : ConsoleState::AwakeNoVideo; break;
        case HK_USB_SUSPENDED: c = sig ? ConsoleState::Ready : ConsoleState::Asleep; break;
        default: c = sig ? ConsoleState::Ready : ConsoleState::Off; break;
        }
    } else if (sig) {
        c = ConsoleState::Ready;
    }
    std::lock_guard<std::mutex> lk(state_mu_);
    if (c != ConsoleState::Ready && (phase_ == SessionPhase::Waking || phase_ == SessionPhase::WaitingForVideo))
        c = ConsoleState::Waking;
    if (c != console_) HK_LOGI("Console: %s", proto::to_string(c));
    console_ = c;
}

void HostApp::tick(uint64_t now)
{
    update_console_state(now);

    // Keep looking for the capture card; recover it if it disappears.
    if (gpu_ready_ && (!capture_ok_ || capture_.failed()) && now - last_capture_retry_us_ > 3'000'000) {
        std::lock_guard<std::mutex> g(gpu_mu_);
        if (gpu_ready_) {
            last_capture_retry_us_ = now;
            if (capture_.failed()) HK_LOGW("Capture failed; reopening");
            open_capture();
        }
    }

    const BridgeStatus b = bridge_.status();
    const uint16_t press_ms = static_cast<uint16_t>(cfg_->get_int("host", "power_press_ms", 150));

    if (stop_requested_.exchange(false)) {
        stop_stream();
        // The client leaves its stream view when we fail; keep the reason on screen.
        bool failed;
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            failed = phase_ == SessionPhase::Failed;
        }
        if (!failed) set_phase(SessionPhase::Lobby, "");
    }
    if (manual_wake_.exchange(false) && b.connected)
        bridge_.request_wake(HK_WAKE_USB_REMOTE_WAKEUP | HK_WAKE_HOME_PRESS | HK_WAKE_BLE_BEACON, press_ms);
    if (sleep_requested_.exchange(false) && b.connected && (b.wake_methods & HK_WAKE_POWER_BUTTON))
        bridge_.request_wake(HK_WAKE_POWER_BUTTON, press_ms);

    SessionPhase phase;
    ConsoleState console;
    uint64_t since;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        phase = phase_;
        console = console_;
        since = phase_since_;
    }

    if (play_requested_.exchange(false)) {
        if (phase == SessionPhase::Streaming) {
            encoder_.request_keyframe();
        } else {
            wake_attempts_ = 0;
            next_wake_at_ = now;
            set_phase(SessionPhase::Waking, "Waking your Switch");
            phase = SessionPhase::Waking;
            since = now;
        }
    }

    if (client_gone_at_ && elapsed_since(now, client_gone_at_) > kClientGrace) {
        client_gone_at_ = 0;
        stop_stream();
        set_phase(SessionPhase::Lobby, "");
        return;
    }

    switch (phase) {
    case SessionPhase::Waking:
    case SessionPhase::WaitingForVideo: {
        if (!capture_ok_) {
            set_phase(SessionPhase::Failed, proto::to_string(HostIssue::NoCapture), HostIssue::NoCapture);
            break;
        }
        if (console == ConsoleState::Ready || capture_.signal_present()) {
            set_phase(SessionPhase::WaitingForController, "Connecting controller");
            break;
        }
        if (!b.connected) {
            set_phase(SessionPhase::Waking, "Waiting for the Switch (controller bridge not connected)",
                      HostIssue::NoBridge);
        } else if (b.usb_state == HK_USB_ACTIVE) {
            // Awake on the bus but no picture: sleep screen / TV mode handoff.
            set_phase(SessionPhase::WaitingForVideo, "Waiting for the Switch's picture");
            if (now >= next_wake_at_ && wake_attempts_ < 4) {
                // The bridge only sends the beacon if the console is not taking input.
                bridge_.request_wake(HK_WAKE_HOME_PRESS | HK_WAKE_BLE_BEACON, press_ms);
                ++wake_attempts_;
                next_wake_at_ = now + 3'000'000;
            }
        } else if (now >= next_wake_at_ && wake_attempts_ < 6) {
            uint8_t methods = HK_WAKE_USB_REMOTE_WAKEUP | HK_WAKE_HOME_PRESS | HK_WAKE_BLE_BEACON;
            // The power button toggles, so it is pressed at most once per Play.
            if (wake_attempts_ == 1 && (b.wake_methods & HK_WAKE_POWER_BUTTON)) methods |= HK_WAKE_POWER_BUTTON;
            bridge_.request_wake(methods, press_ms);
            ++wake_attempts_;
            next_wake_at_ = now + 5'000'000;
            set_phase(SessionPhase::Waking, "Waking your Switch");
        }
        if (now - since > kWakeTimeout) {
            const bool can_press = (b.wake_methods & HK_WAKE_POWER_BUTTON) != 0;
            const bool can_beacon = (b.wake_methods & HK_WAKE_BLE_BEACON) != 0;
            set_phase(SessionPhase::Failed,
                      can_press || can_beacon ? "Your Switch did not wake up. Check it is docked and the dock has power."
                                              : "Your Switch did not wake up. Wake it once with a detached Joy-Con 2 "
                                                "near the controller bridge so it can learn how (see Remote wake in "
                                                "the docs).",
                      HostIssue::WakeFailed);
        }
        break;
    }
    case SessionPhase::WaitingForController:
        if (!b.connected) {
            set_phase(SessionPhase::StartingStream, "Starting stream (controller bridge not connected)",
                      HostIssue::NoBridge);
        } else if (b.usb_state == HK_USB_ACTIVE) {
            set_phase(SessionPhase::StartingStream, "Starting stream");
        } else if (now - since > kControllerTimeout) {
            set_phase(SessionPhase::StartingStream, "Starting stream (the Switch has not accepted the controller yet)",
                      HostIssue::ControllerNotAccepted);
        }
        break;
    case SessionPhase::StartingStream:
        if (!gpu_ready_) {
            set_phase(SessionPhase::StartingStream,
                      benchmarking_ ? "Choosing the fastest video encoder (first run only)" : "Preparing video");
            if (now - since > kEncoderWaitTimeout)
                set_phase(SessionPhase::Failed, proto::to_string(HostIssue::NoEncoder), HostIssue::NoEncoder);
            break;
        }
        if (encoder_index_ < 0) {
            set_phase(SessionPhase::Failed, proto::to_string(HostIssue::NoEncoder), HostIssue::NoEncoder);
            break;
        }
        start_stream();
        {
            HostIssue keep;
            {
                std::lock_guard<std::mutex> lk(state_mu_);
                keep = issue_;
            }
            set_phase(SessionPhase::Streaming, "", keep);
        }
        break;
    case SessionPhase::Streaming:
        if (encoder_failures_ >= 5) {
            stop_stream();
            set_phase(SessionPhase::Failed, "The video encoder stopped working", HostIssue::NoEncoder);
            break;
        }
        // Audio keeps flowing on its own, so a silent video pipeline would
        // otherwise leave the client on a spinner with sound and no picture.
        if (elapsed_since(now, std::max(since, last_encoded_us_.load())) > kVideoStallTimeout) {
            const CaptureStats cs = capture_.stats();
            HK_LOGE("No video encoded for %llu s (capture %.1f fps, %llu frames, signal %d, converted %llu)",
                    static_cast<unsigned long long>(kVideoStallTimeout / 1'000'000), cs.fps,
                    static_cast<unsigned long long>(cs.frames), cs.signal ? 1 : 0,
                    static_cast<unsigned long long>(converted_frames_.load()));
            stop_stream();
            set_phase(SessionPhase::Failed,
                      cs.fps < 1 ? "The capture card stopped sending video. Check the HDMI cable, then try again."
                                 : "The host could not encode video. Try again, or restart Hikari on the host.",
                      cs.fps < 1 ? HostIssue::NoCapture : HostIssue::NoEncoder);
        }
        break;
    case SessionPhase::Lobby:
    case SessionPhase::Failed:
        break;
    }
}

void HostApp::orchestrator_thread()
{
    set_thread_role(ThreadRole::Background, "hikari-orchestrator");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_) {
        WaitForSingleObject(wake_.handle(), static_cast<DWORD>(kTick / 1000));
        if (!running_) break;
        const uint64_t now = now_us();
        tick(now);
        if (now - last_status_us_ >= 250'000) {
            last_status_us_ = now;
            send_status();
        }
        if (now - last_stats_us_ >= 1'000'000) {
            last_stats_us_ = now;
            send_stats();
        }
    }
    CoUninitialize();
}

void HostApp::send_status()
{
    if (!net_.client_connected()) return;
    const BridgeStatus b = bridge_.status();
    proto::HostStatus s;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        s.console = console_;
        s.phase = phase_;
        s.issue = issue_;
        s.phase_elapsed_ms = static_cast<uint16_t>(std::min<uint64_t>((now_us() - phase_since_) / 1000, 0xFFFF));
        std::snprintf(s.message, sizeof(s.message), "%s", message_.c_str());
    }
    if (s.phase == SessionPhase::Lobby && s.issue == HostIssue::None) {
        if (!b.connected)
            s.issue = HostIssue::NoBridge;
        else if (!capture_ok_)
            s.issue = HostIssue::NoCapture;
        else if (gpu_ready_ && encoder_index_ < 0)
            s.issue = HostIssue::NoEncoder;
    }
    s.bridge_ok = b.connected;
    s.capture_ok = capture_ok_;
    s.signal_ok = capture_ok_ && capture_.signal_present();
    s.encoder_ok = gpu_ready_ && encoder_index_ >= 0;
    s.wake_methods = b.wake_methods;
    s.remote_wakeup = b.remote_wakeup_enabled;
    std::snprintf(s.host_name, sizeof(s.host_name), "%s", host_name_.c_str());
    net_.send_status(s);
}

void HostApp::send_stats()
{
    const uint64_t now = now_us();
    const CaptureStats cs = capture_.stats();
    const BridgeStatus b = bridge_.status();
    const HostNetStats ns = net_.stats();
    const LatencySummary enc = encoder_.latency();
    const double efps = encode_fps_.sample(now);

    proto::HostStats h;
    h.capture_fps_x100 = static_cast<uint16_t>(cs.fps * 100);
    h.encode_fps_x100 = static_cast<uint16_t>(efps * 100);
    h.capture_drops = static_cast<uint32_t>(encode_mailbox_.overwrites());
    h.encode_drops = static_cast<uint32_t>(ns.frames_dropped_sender);
    {
        std::lock_guard<std::mutex> lk(abr_mu_);
        h.bitrate_kbps = abr_now_.bitrate_kbps;
        h.width = abr_now_.mode.width;
        h.height = abr_now_.mode.height;
        h.fps = abr_now_.mode.fps;
    }
    auto sat = [](uint32_t v) { return static_cast<uint16_t>(std::min<uint32_t>(v, 0xFFFF)); };
    h.capture_to_send_p50 = sat(ns.capture_to_sent.p50);
    h.capture_to_send_p95 = sat(ns.capture_to_sent.p95);
    h.capture_to_send_p99 = sat(ns.capture_to_sent.p99);
    h.encode_p50 = sat(enc.p50);
    h.encode_p95 = sat(enc.p95);
    h.encode_p99 = sat(enc.p99);
    h.input_owd_p50 = static_cast<int32_t>(ns.input_owd.p50);
    h.input_owd_p95 = static_cast<int32_t>(ns.input_owd.p95);
    h.input_owd_p99 = static_cast<int32_t>(ns.input_owd.p99);
    h.input_host_p50 = sat(b.host_write.p50);
    h.input_host_p99 = sat(b.host_write.p99);
    h.esp_connected = b.connected;
    h.esp_usb_mounted = b.usb_state;
    h.esp_failsafe = b.failsafe;
    h.esp_reports_per_s = b.telemetry.usb_reports_per_s;
    h.esp_rx_to_usb_p50 = b.telemetry.rx_to_submit.p50;
    h.esp_rx_to_usb_p99 = b.telemetry.rx_to_submit.p99;
    h.esp_rx_to_console_p50 = b.telemetry.rx_to_consumed.p50;
    h.esp_rx_to_console_p99 = b.telemetry.rx_to_consumed.p99;
    h.esp_link_rtt_p50 = sat(b.link_rtt.p50);
    h.input_rate = static_cast<uint16_t>(ns.input_rate);
    if (gpu_ready_ && encoder_index_ >= 0)
        std::snprintf(h.encoder_name, sizeof(h.encoder_name), "%s",
                      encoders_[static_cast<size_t>(encoder_index_)].name.c_str());
    if (net_.client_connected()) net_.send_stats(h);

    if (streaming_)
        HK_LOGD("cap %.1f fps | enc %.1f fps %s | cap->sent %s | %.1f Mbps | input owd %s | esp rx->console %u/%u us",
                cs.fps, efps, enc.to_ms_string().c_str(), ns.capture_to_sent.to_ms_string().c_str(), ns.video_mbps,
                ns.input_owd.to_ms_string().c_str(), b.telemetry.rx_to_consumed.p50, b.telemetry.rx_to_consumed.p99);
}

HostSnapshot HostApp::snapshot()
{
    HostSnapshot s;
    s.host_name = host_name_;
    s.pairing_code = pairing_code_;
    s.addresses = local_addresses();
    for (const auto& a : s.addresses)
        if (a.tailscale) {
            s.tailscale_ip = a.ip;
            break;
        }
    net_.set_tailscale_address(s.tailscale_ip);
    s.port = port_;
    {
        std::lock_guard<std::mutex> lk(sysinfo_mu_);
        s.tailscale_name = tailscale_name_;
        s.firewall = firewall_;
        s.lid_action = lid_action_;
    }
    s.autostart = autostart_enabled();
    // Prefer the MagicDNS name (stable across address changes), keep the IP
    // as a fallback; without Tailscale, the first LAN address.
    std::string primary = !s.tailscale_name.empty() ? s.tailscale_name : s.tailscale_ip;
    if (primary.empty() && !s.addresses.empty()) primary = s.addresses.front().ip;
    if (!primary.empty()) s.invite = make_invite(primary, port_, s.tailscale_ip, pairing_code_);
    s.bridge = bridge_.status();
    s.bridge_ok = s.bridge.connected;
    s.bridge_port = s.bridge.port;
    s.usb_state = s.bridge.usb_state;
    s.wake_methods = s.bridge.wake_methods;
    s.remote_wakeup = s.bridge.remote_wakeup_enabled;
    s.capture_ok = capture_ok_;
    if (capture_ok_) {
        s.capture_name = capture_.device_name();
        s.capture_mode = capture_.mode().to_string();
        s.capture = capture_.stats();
        s.signal = s.capture.signal;
    }
    s.audio_ok = audio_found_;
    s.audio_name = audio_ep_.name;
    s.encoder_benchmarking = benchmarking_;
    s.encoder_ok = gpu_ready_ && encoder_index_ >= 0;
    if (s.encoder_ok) s.encoder_name = encoders_[static_cast<size_t>(encoder_index_)].name;
    s.gpu_name = gpu_.adapter_name;
    s.net = net_.stats();
    s.client_connected = s.net.client_connected;
    s.client_name = s.net.client_name;
    s.client_address = s.net.client_address;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        s.console = console_;
        s.phase = phase_;
        s.issue = issue_;
        s.message = message_;
    }
    {
        std::lock_guard<std::mutex> lk(abr_mu_);
        s.mode = abr_now_.mode;
        s.bitrate_kbps = abr_now_.bitrate_kbps;
        s.fec_group = abr_now_.fec_group;
    }
    s.encode_latency = encoder_.latency();
    return s;
}

}  // namespace hikari

#include "audio_player.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "hikari/log.h"
#include "hikari/system.h"
#include "hk_link.h"

#ifdef HIKARI_WITH_OPUS
#include <opus.h>
#endif

using Microsoft::WRL::ComPtr;

namespace hikari {

namespace {
constexpr uint32_t kRate = 48000;
constexpr uint32_t kCh = 2;
const CLSID kMMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
}  // namespace

AudioPlayer::~AudioPlayer() { stop(); }

bool AudioPlayer::start(uint32_t target_ms)
{
    stop();
    target_ms_ = target_ms;
    ring_.assign(kRate * kCh, 0);
    head_ = count_ = 0;
    priming_ = true;
    have_seq_ = false;
#ifdef HIKARI_WITH_OPUS
    int err = 0;
    opus_ = opus_decoder_create(kRate, kCh, &err);
#endif
    running_ = true;
    thread_ = std::thread(&AudioPlayer::run, this);
    return true;
}

void AudioPlayer::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
#ifdef HIKARI_WITH_OPUS
    if (opus_) opus_decoder_destroy(opus_);
#endif
    opus_ = nullptr;
}

void AudioPlayer::write_samples(const int16_t* s, size_t frames)
{
    const size_t cap = ring_.size() / kCh;
    for (size_t i = 0; i < frames; ++i) {
        if (count_ == cap) {  // overflow: drop oldest (should never happen with drift control)
            head_ = (head_ + 1) % cap;
            --count_;
        }
        const size_t pos = (head_ + count_) % cap;
        ring_[pos * kCh] = s[i * kCh];
        ring_[pos * kCh + 1] = s[i * kCh + 1];
        ++count_;
    }
}

void AudioPlayer::push(const proto::AudioPacket& p)
{
    if (!running_) return;
    int16_t pcm[960 * kCh];
    std::lock_guard<std::mutex> lk(mu_);

    // Loss concealment for gaps (Opus synthesises; PCM leaves the gap, which
    // the drift controller then absorbs).
    if (have_seq_ && hk_seq_newer(p.seq, last_seq_ + 1)) {
        const uint32_t missing = std::min<uint32_t>(p.seq - last_seq_ - 1, 4);
#ifdef HIKARI_WITH_OPUS
        if (opus_ && p.codec == proto::AudioCodec::Opus)
            for (uint32_t i = 0; i < missing; ++i) {
                const int n = opus_decode(opus_, nullptr, 0, pcm, std::min<int>(p.samples, 960), 0);
                if (n > 0) write_samples(pcm, static_cast<size_t>(n));
            }
#else
        (void)missing;
#endif
    }
    if (have_seq_ && !hk_seq_newer(p.seq, last_seq_)) return;  // late/duplicate
    last_seq_ = p.seq;
    have_seq_ = true;

    if (p.codec == proto::AudioCodec::PcmS16) {
        const size_t frames = std::min<size_t>(p.payload_len / (sizeof(int16_t) * kCh), 960);
        std::memcpy(pcm, p.payload, frames * kCh * sizeof(int16_t));
        write_samples(pcm, frames);
    }
#ifdef HIKARI_WITH_OPUS
    else if (p.codec == proto::AudioCodec::Opus && opus_) {
        const int n = opus_decode(opus_, p.payload, static_cast<opus_int32>(p.payload_len), pcm, 960, 0);
        if (n > 0) write_samples(pcm, static_cast<size_t>(n));
    }
#endif
}

size_t AudioPlayer::read_samples(int16_t* out, size_t frames)
{
    std::lock_guard<std::mutex> lk(mu_);
    const size_t cap = ring_.size() / kCh;
    const size_t target = target_ms_.load() * kRate / 1000;

    if (priming_) {
        if (count_ < target) return 0;  // build the cushion once, then play
        priming_ = false;
    }
    // Drift control: nudge the fill level toward the target by dropping or
    // repeating one sample every ~500 (0.2%: inaudible, no pitch artefacts).
    const bool too_full = count_ > target + target / 2 + 240;
    const bool too_empty = count_ + 240 < target / 2;
    size_t produced = 0;
    for (size_t i = 0; i < frames; ++i) {
        if (count_ == 0) {
            ++underruns_;
            priming_ = true;  // re-prime after an underrun instead of crackling
            break;
        }
        if (++drift_counter_ >= 500) {
            drift_counter_ = 0;
            if (too_full && count_ > 1) {  // skip one frame
                head_ = (head_ + 1) % cap;
                --count_;
            } else if (too_empty) {  // repeat one frame
                out[i * kCh] = ring_[head_ * kCh];
                out[i * kCh + 1] = ring_[head_ * kCh + 1];
                ++produced;
                continue;
            }
        }
        out[i * kCh] = ring_[head_ * kCh];
        out[i * kCh + 1] = ring_[head_ * kCh + 1];
        head_ = (head_ + 1) % cap;
        --count_;
        ++produced;
    }
    buffered_ms_ = static_cast<uint16_t>(count_ * 1000 / kRate);
    return produced;
}

void AudioPlayer::run()
{
    set_thread_role(ThreadRole::Audio, "hikari-audio-render");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    do {
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) break;
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = kCh;
        fmt.nSamplesPerSec = kRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = kCh * 2;
        fmt.nAvgBytesPerSec = kRate * fmt.nBlockAlign;

        ComPtr<IAudioClient> client;
        ComPtr<IAudioClient3> client3;
        bool low_latency = false;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client3.GetAddressOf())))) {
            UINT32 def = 0, fund = 0, minp = 0, maxp = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(&fmt, &def, &fund, &minp, &maxp)) &&
                SUCCEEDED(client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minp, &fmt, nullptr))) {
                low_latency = true;
                client = client3;
                HK_LOGI("Audio: low-latency engine period %.2f ms", minp * 1000.0 / kRate);
            }
        }
        if (!low_latency) {
            client.Reset();
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(client.GetAddressOf()))))
                break;
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0, &fmt, nullptr))) break;
        }
        client->SetEventHandle(ev);
        ComPtr<IAudioRenderClient> render;
        if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) break;
        UINT32 buffer_frames = 0;
        client->GetBufferSize(&buffer_frames);
        client->Start();

        std::vector<int16_t> tmp(buffer_frames * kCh);
        while (running_) {
            if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            const UINT32 avail = buffer_frames - padding;
            if (!avail) continue;
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(avail, &data))) break;
            auto* out = reinterpret_cast<int16_t*>(data);
            const size_t got = read_samples(tmp.data(), avail);
            const float vol = muted_ ? 0.0f : volume_.load();
            for (size_t i = 0; i < got * kCh; ++i) out[i] = static_cast<int16_t>(tmp[i] * vol);
            std::memset(out + got * kCh, 0, (avail - got) * kCh * sizeof(int16_t));
            render->ReleaseBuffer(avail, 0);
        }
        client->Stop();
    } while (false);
    CloseHandle(ev);
    CoUninitialize();
}

}  // namespace hikari

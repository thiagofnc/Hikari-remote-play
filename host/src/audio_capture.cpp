#include "audio_capture.h"

#include <audioclient.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "hikari/clock.h"
#include "hikari/log.h"
#include "hikari/system.h"

#ifdef HIKARI_WITH_OPUS
#include <opus.h>
#endif

using Microsoft::WRL::ComPtr;

namespace hikari {

namespace {
// Defined locally so this TU does not depend on INITGUID games.
const DEVPROPKEY kDevInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
const DEVPROPKEY kDevContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kPkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const CLSID kMMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

std::vector<AudioEndpointInfo> enumerate_audio_capture_endpoints()
{
    std::vector<AudioEndpointInfo> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        AudioEndpointInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) info.name = utf8(v.pwszVal);
            PropVariantClear(&v);
            if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && v.puuid) info.container = *v.puuid;
            PropVariantClear(&v);
        }
        out.push_back(std::move(info));
    }
    return out;
}

bool container_id_for_symlink(const std::wstring& symlink, GUID& out)
{
    wchar_t instance[512];
    ULONG size = sizeof(instance);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(symlink.c_str(), &kDevInstanceId, &type, reinterpret_cast<PBYTE>(instance),
                                          &size, 0) != CR_SUCCESS)
        return false;
    DEVINST inst = 0;
    if (CM_Locate_DevNodeW(&inst, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    size = sizeof(GUID);
    return CM_Get_DevNode_PropertyW(inst, &kDevContainerId, &type, reinterpret_cast<PBYTE>(&out), &size, 0) ==
           CR_SUCCESS;
}

bool is_builtin_container(const GUID& container)
{
    static const GUID kBuiltin = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
    return IsEqualGUID(container, kBuiltin);
}

int match_audio_endpoint(const std::vector<AudioEndpointInfo>& eps, const std::wstring& video_symlink,
                         const std::string& video_name)
{
    // Built-in devices all share one container, so matching on it would pair
    // a built-in camera with the laptop microphone.
    GUID container{};
    if (!video_symlink.empty() && container_id_for_symlink(video_symlink, container) &&
        !is_builtin_container(container)) {
        for (size_t i = 0; i < eps.size(); ++i)
            if (IsEqualGUID(eps[i].container, container)) return static_cast<int>(i);
    }
    // Fallback: endpoint name containing a distinctive word of the video name.
    const std::string vn = lower(video_name);
    int best = -1;
    size_t best_len = 0;
    size_t start = 0;
    while (start < vn.size()) {
        size_t end = vn.find_first_of(" ()-_", start);
        if (end == std::string::npos) end = vn.size();
        const std::string word = vn.substr(start, end - start);
        if (word.size() >= 3 && word != "usb" && word != "video" && word != "capture" && word != "device") {
            for (size_t i = 0; i < eps.size(); ++i)
                if (lower(eps[i].name).find(word) != std::string::npos && word.size() > best_len) {
                    best = static_cast<int>(i);
                    best_len = word.size();
                }
        }
        start = end + 1;
    }
    if (best >= 0) return best;
    for (size_t i = 0; i < eps.size(); ++i) {
        const std::string n = lower(eps[i].name);
        if (n.find("hdmi") != std::string::npos || n.find("digital audio interface") != std::string::npos ||
            n.find("capture") != std::string::npos)
            return static_cast<int>(i);
    }
    return -1;
}

AudioCapture::~AudioCapture() { stop(); }

bool AudioCapture::start(const AudioEndpointInfo& ep, bool use_opus, ChunkCallback cb)
{
    stop();
    cb_ = std::move(cb);
    want_opus_ = use_opus;
    name_ = ep.name;
    failed_ = false;
    running_ = true;
    thread_ = std::thread(&AudioCapture::run, this, ep.id);
    return true;
}

void AudioCapture::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::run(std::wstring id)
{
    set_thread_role(ThreadRole::Audio, "hikari-audio-capture");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto fail = [&](const char* what, HRESULT hr) {
        HK_LOGE("Audio capture %s: %s", what, hresult_string(hr).c_str());
        failed_ = true;
    };

    do {
        ComPtr<IMMDeviceEnumerator> en;
        HRESULT hr = CoCreateInstance(kMMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (FAILED(hr)) { fail("enumerator", hr); break; }
        ComPtr<IMMDevice> dev;
        hr = en->GetDevice(id.c_str(), &dev);
        if (FAILED(hr)) { fail("GetDevice", hr); break; }
        ComPtr<IAudioClient> client;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) { fail("Activate", hr); break; }

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = kChannels;
        fmt.nSamplesPerSec = kRate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * 2);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        // Let the audio engine convert whatever the card delivers into
        // 48 kHz s16 stereo, with a 10 ms engine buffer.
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 100000, 0, &fmt, nullptr);
        if (FAILED(hr)) { fail("Initialize", hr); break; }
        client->SetEventHandle(ev);
        ComPtr<IAudioCaptureClient> cap;
        hr = client->GetService(IID_PPV_ARGS(&cap));
        if (FAILED(hr)) { fail("GetService", hr); break; }

#ifdef HIKARI_WITH_OPUS
        OpusEncoder* opus = nullptr;
        if (want_opus_) {
            int err = 0;
            opus = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
            if (opus) {
                opus_encoder_ctl(opus, OPUS_SET_BITRATE(160000));
                opus_encoder_ctl(opus, OPUS_SET_COMPLEXITY(6));
                opus_encoder_ctl(opus, OPUS_SET_INBAND_FEC(0));
                opus_encoder_ctl(opus, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
            }
        }
        codec_ = opus ? proto::AudioCodec::Opus : proto::AudioCodec::PcmS16;
#else
        codec_ = proto::AudioCodec::PcmS16;
#endif
        hr = client->Start();
        if (FAILED(hr)) { fail("Start", hr); break; }
        HK_LOGI("Audio capture: %s (%s, 5 ms packets)", name_.c_str(),
                codec_ == proto::AudioCodec::Opus ? "Opus" : "PCM");

        std::vector<int16_t> chunk(kChunk * kChannels);
        size_t fill = 0;
        uint64_t chunk_time = 0;
        uint8_t encoded[1500];

        while (running_) {
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            for (;;) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD bflags = 0;
                UINT64 qpc = 0;
                hr = cap->GetBuffer(&data, &frames, &bflags, nullptr, &qpc);
                if (hr == AUDCLNT_S_BUFFER_EMPTY || frames == 0) {
                    if (SUCCEEDED(hr) && frames == 0 && hr != AUDCLNT_S_BUFFER_EMPTY) cap->ReleaseBuffer(0);
                    break;
                }
                if (FAILED(hr)) {
                    fail("GetBuffer", hr);
                    running_ = false;
                    break;
                }
                const uint64_t base_us = qpc ? hns_to_us(static_cast<int64_t>(qpc)) : now_us();
                const auto* src = reinterpret_cast<const int16_t*>(data);
                for (UINT32 f = 0; f < frames; ++f) {
                    if (fill == 0) chunk_time = base_us + uint64_t(f) * 1'000'000 / kRate;
                    for (UINT32 c = 0; c < kChannels; ++c)
                        chunk[fill * kChannels + c] = (bflags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : src[f * kChannels + c];
                    if (++fill == kChunk) {
                        AudioChunk out{};
                        out.samples = kChunk;
                        out.capture_us = chunk_time;
                        out.codec = codec_;
#ifdef HIKARI_WITH_OPUS
                        if (opus) {
                            const int n = opus_encode(opus, chunk.data(), kChunk, encoded, sizeof(encoded));
                            if (n > 0) {
                                out.data = encoded;
                                out.bytes = static_cast<size_t>(n);
                                cb_(out);
                            }
                        } else
#endif
                        {
                            out.data = reinterpret_cast<const uint8_t*>(chunk.data());
                            out.bytes = chunk.size() * sizeof(int16_t);
                            cb_(out);
                        }
                        fill = 0;
                    }
                }
                cap->ReleaseBuffer(frames);
            }
        }
        client->Stop();
#ifdef HIKARI_WITH_OPUS
        if (opus) opus_encoder_destroy(opus);
#endif
        (void)encoded;
    } while (false);

    CloseHandle(ev);
    CoUninitialize();
    running_ = false;
}

}  // namespace hikari

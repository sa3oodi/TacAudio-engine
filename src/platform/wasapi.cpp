#include "platform/wasapi.h"

#include <windows.h>
// initguid first so the GUIDs/PROPERTYKEYs below are defined in this unit;
// mmdeviceapi (-> propsys) must precede functiondiscoverykeys for PROPERTYKEY.
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tac::wasapi {

namespace {

constexpr REFERENCE_TIME kCaptureBuffer = 1000000;  // 100 ms (capture drains all; size adds no latency)
constexpr REFERENCE_TIME kRenderBuffer = 200000;    // 20 ms shared-mode buffer

std::string hr(const char* what, HRESULT h) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(h));
    return buf;
}
void check(HRESULT h, const char* what) {
    if (FAILED(h)) throw std::runtime_error(hr(what, h));
}

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)),
          "CoCreateInstance(MMDeviceEnumerator)");
    return e;
}

DeviceInfo describe(IMMDevice* dev, bool render, const std::wstring& defaultId) {
    DeviceInfo info;
    info.isRender = render;
    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id))) {
        info.id = id;
        CoTaskMemFree(id);
    }
    info.isDefault = info.id == defaultId;

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) info.name = narrow(v.pwszVal);
        PropVariantClear(&v);
    }

    ComPtr<IAudioClient> client;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            info.channels = mix->nChannels;
            info.sampleRate = mix->nSamplesPerSec;
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
                info.channelMask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
            REFERENCE_TIME defP = 0, minP = 0;
            if (SUCCEEDED(client->GetDevicePeriod(&defP, &minP))) info.minExclusivePeriodMs = minP / 10000.0;
            ComPtr<IAudioClient3> client3;
            if (SUCCEEDED(client.As(&client3))) {
                UINT32 def = 0, fund = 0, mn = 0, mx = 0;
                if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx))) {
                    info.defaultPeriodFrames = def;
                    info.minPeriodFrames = mn;
                }
            }
            CoTaskMemFree(mix);
        }
    }
    return info;
}

ComPtr<IMMDevice> openDevice(const std::wstring& id) {
    ComPtr<IMMDevice> dev;
    check(enumerator()->GetDevice(id.c_str(), &dev), "IMMDeviceEnumerator::GetDevice");
    return dev;
}

WAVEFORMATEXTENSIBLE floatFormat(std::uint32_t rate, std::uint32_t channels, std::uint32_t mask) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = mask;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

std::uint32_t maskFor(std::uint32_t channels, std::uint32_t mask) {
    if (mask) return mask;
    switch (channels) {
        case 1: return KSAUDIO_SPEAKER_MONO;
        case 2: return KSAUDIO_SPEAKER_STEREO;
        case 4: return KSAUDIO_SPEAKER_QUAD;
        case 6: return KSAUDIO_SPEAKER_5POINT1_SURROUND;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
        default: return 0;
    }
}

// Raises the calling thread to MMCSS "Pro Audio" for the lifetime of the object.
struct MmcssScope {
    HANDLE h = nullptr;
    MmcssScope() {
        DWORD idx = 0;
        h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    }
    ~MmcssScope() {
        if (h) AvRevertMmThreadCharacteristics(h);
    }
};

struct EventHandle {
    HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~EventHandle() {
        if (h) CloseHandle(h);
    }
};

}  // namespace

// ------------------------------------------------------------------ COM / devices

ComScope::ComScope() {
    const HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owns_ = SUCCEEDED(h);
    if (FAILED(h) && h != RPC_E_CHANGED_MODE) throw std::runtime_error(hr("CoInitializeEx", h));
}
ComScope::~ComScope() {
    if (owns_) CoUninitialize();
}

std::vector<DeviceInfo> listDevices(bool render) {
    auto e = enumerator();
    const EDataFlow flow = render ? eRender : eCapture;

    std::wstring defaultId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(e->GetDefaultAudioEndpoint(flow, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> col;
    check(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col), "EnumAudioEndpoints");
    UINT count = 0;
    col->GetCount(&count);
    std::vector<DeviceInfo> out;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(col->Item(i, &dev))) out.push_back(describe(dev.Get(), render, defaultId));
    }
    return out;
}

std::optional<DeviceInfo> findDevice(const std::string& query, bool render) {
    const auto devices = listDevices(render);
    if (devices.empty()) return std::nullopt;
    if (query.empty() || query == "default") {
        for (const auto& d : devices)
            if (d.isDefault) return d;
        return devices.front();
    }
    if (std::all_of(query.begin(), query.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const std::size_t idx = std::stoul(query);
        if (idx >= 1 && idx <= devices.size()) return devices[idx - 1];
        return std::nullopt;
    }
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string q = lower(query);
    for (const auto& d : devices)
        if (lower(d.name).find(q) != std::string::npos) return d;
    return std::nullopt;
}

// ------------------------------------------------------------------ shared stream plumbing

struct StreamCore {
    ComPtr<IAudioClient> client;
    EventHandle event;
    std::thread thread;
    std::atomic<bool> stopFlag{false};
    std::mutex errMutex;
    std::string error;
    StreamFormat format;
    // Capture wait timeout: the thread also polls in case a loopback event is late.
    // (Measured: 2 ms and 10 ms give the same loopback delivery delay, ~9 ms.)
    DWORD pollMs = 2;

    // Exclusive render: device sample format (the callback always produces float).
    enum class SampleKind { Float32, Int32, Int24, Int16 } sampleKind = SampleKind::Float32;
    std::vector<float> scratch;

    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lock(errMutex);
        error = e;
    }
    std::string getError() {
        std::lock_guard<std::mutex> lock(errMutex);
        return error;
    }
    void join() {
        stopFlag = true;
        if (event.h) SetEvent(event.h);
        if (thread.joinable()) thread.join();
        if (client) client->Stop();
    }
};

// ------------------------------------------------------------------ capture

struct CaptureStream::Impl : StreamCore {
    ComPtr<IAudioCaptureClient> capture;
};

CaptureStream::CaptureStream() : impl_(std::make_unique<Impl>()) {}
CaptureStream::~CaptureStream() { stop(); }

StreamFormat CaptureStream::open(const DeviceInfo& device, bool loopback, std::uint32_t sampleRate) {
    if (loopback && !device.isRender) throw std::invalid_argument("loopback needs a render (output) device");
    if (!loopback && device.isRender) throw std::invalid_argument("capture needs an input device (or use loopback)");

    auto dev = openDevice(device.id);
    auto& c = *impl_;
    check(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c.client), "Activate(IAudioClient)");

    const std::uint32_t channels = device.channels ? device.channels : 2;
    const std::uint32_t mask = maskFor(channels, device.channelMask);
    WAVEFORMATEXTENSIBLE fmt = floatFormat(sampleRate, channels, mask);

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    check(c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kCaptureBuffer, 0,
                               reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr),
          "IAudioClient::Initialize(capture)");
    check(c.client->SetEventHandle(c.event.h), "SetEventHandle(capture)");
    check(c.client->GetService(IID_PPV_ARGS(&c.capture)), "GetService(IAudioCaptureClient)");

    UINT32 frames = 0;
    c.client->GetBufferSize(&frames);
    c.format = {sampleRate, channels, mask, frames};
    return c.format;
}

void CaptureStream::start(Callback cb) {
    auto& c = *impl_;
    if (!c.client) throw std::logic_error("CaptureStream::start before open");
    c.stopFlag = false;
    check(c.client->Start(), "IAudioClient::Start(capture)");
    c.thread = std::thread([this, &c, cb = std::move(cb)] {
        ComScope com;
        MmcssScope mmcss;
        try {
            while (!c.stopFlag) {
                WaitForSingleObject(c.event.h, c.pollMs);  // also polls, in case loopback events are not signalled
                if (c.stopFlag) break;
                UINT32 packet = 0;
                check(c.capture->GetNextPacketSize(&packet), "GetNextPacketSize");
                while (packet > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    UINT64 qpc = 0;
                    check(c.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc), "IAudioCaptureClient::GetBuffer");
                    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    cb(silent ? nullptr : reinterpret_cast<const float*>(data), frames, silent, qpc);
                    check(c.capture->ReleaseBuffer(frames), "IAudioCaptureClient::ReleaseBuffer");
                    check(c.capture->GetNextPacketSize(&packet), "GetNextPacketSize");
                }
            }
        } catch (const std::exception& e) {
            c.setError(e.what());
            failed_ = true;
        }
    });
}

void CaptureStream::stop() {
    if (impl_) impl_->join();
}
std::string CaptureStream::error() const { return impl_->getError(); }

// ------------------------------------------------------------------ render

struct RenderStream::Impl : StreamCore {
    ComPtr<IAudioRenderClient> render;
};

RenderStream::RenderStream() : impl_(std::make_unique<Impl>()) {}
RenderStream::~RenderStream() { stop(); }

namespace {
// True if the device mix format is 32-bit float at `rate` with `channels` (required by IAudioClient3).
bool mixIsFloat(const WAVEFORMATEX* mix, std::uint32_t rate, std::uint32_t channels) {
    if (!mix || mix->nSamplesPerSec != rate || mix->nChannels != channels || mix->wBitsPerSample != 32) return false;
    if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22 &&
           reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

WAVEFORMATEXTENSIBLE pcmFormat(std::uint32_t rate, std::uint32_t channels, std::uint32_t mask, WORD container,
                               WORD valid, bool isFloat) {
    WAVEFORMATEXTENSIBLE f = floatFormat(rate, channels, mask);
    f.Format.wBitsPerSample = container;
    f.Format.nBlockAlign = static_cast<WORD>(channels * container / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Samples.wValidBitsPerSample = valid;
    f.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return f;
}

std::string exclusiveError(HRESULT h) {
    if (h == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
        return "exclusive mode is disabled for this device (Sound settings > device > Properties > Advanced > "
               "'Allow applications to take exclusive control')";
    if (h == AUDCLNT_E_DEVICE_IN_USE)
        return "the device is in use by another app; exclusive mode needs it to be free";
    return hr("IAudioClient::Initialize(exclusive render)", h);
}
}  // namespace

StreamFormat RenderStream::open(const DeviceInfo& device, std::uint32_t sampleRate, std::uint32_t channels,
                                const RenderOptions& options) {
    if (!device.isRender) throw std::invalid_argument("render needs an output device");
    auto dev = openDevice(device.id);
    auto& c = *impl_;
    const std::uint32_t mask = maskFor(channels, channels == device.channels ? device.channelMask : 0);

    // 0) Exclusive mode: TacAudio owns the device, no Windows mixer, small device period.
    if (options.exclusive) {
        struct Cand {
            Impl::SampleKind kind;
            WORD container, valid;
            bool isFloat;
            const char* name;
        };
        const Cand cands[] = {
            {Impl::SampleKind::Float32, 32, 32, true, "float32"}, {Impl::SampleKind::Int32, 32, 32, false, "int32"},
            {Impl::SampleKind::Int32, 32, 24, false, "int24-in-32"}, {Impl::SampleKind::Int24, 24, 24, false, "int24"},
            {Impl::SampleKind::Int16, 16, 16, false, "int16"}};
        check(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c.client), "Activate(IAudioClient)");
        const Cand* chosen = nullptr;
        WAVEFORMATEXTENSIBLE fmt{};
        for (const auto& k : cands) {
            fmt = pcmFormat(sampleRate, channels, mask, k.container, k.valid, k.isFloat);
            if (c.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr) == S_OK) {
                chosen = &k;
                break;
            }
        }
        if (!chosen)
            throw std::runtime_error("device does not accept " + std::to_string(sampleRate) + " Hz, " +
                                     std::to_string(channels) + " ch in exclusive mode");

        REFERENCE_TIME defP = 0, minP = 0;
        c.client->GetDevicePeriod(&defP, &minP);
        REFERENCE_TIME per = options.exclusivePeriodMs > 0 ? static_cast<REFERENCE_TIME>(options.exclusivePeriodMs * 10000) : minP;
        per = std::max(per, minP);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        HRESULT h = c.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, per, per,
                                         reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (h == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {  // re-initialise with the aligned buffer size
            UINT32 f = 0;
            c.client->GetBufferSize(&f);
            per = static_cast<REFERENCE_TIME>(10000000.0 * f / sampleRate + 0.5);
            c.client.Reset();
            check(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c.client), "Activate(IAudioClient)");
            h = c.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, per, per, reinterpret_cast<WAVEFORMATEX*>(&fmt),
                                     nullptr);
        }
        if (FAILED(h)) throw std::runtime_error(exclusiveError(h));
        check(c.client->SetEventHandle(c.event.h), "SetEventHandle(render)");
        check(c.client->GetService(IID_PPV_ARGS(&c.render)), "GetService(IAudioRenderClient)");
        UINT32 frames = 0;
        c.client->GetBufferSize(&frames);
        c.sampleKind = chosen->kind;
        c.scratch.assign(static_cast<std::size_t>(frames) * channels, 0.0f);
        // Double-buffered: one buffer playing while the next is written.
        c.format = {sampleRate, channels, mask, frames, frames, 2 * frames, false, true, chosen->name};
        return c.format;
    }

    std::uint32_t period = 0;
    bool lowLatency = false;

    // 1) Low-latency shared stream at the minimum engine period (needs the exact mix format).
    if (options.lowLatency) {
        ComPtr<IAudioClient3> c3;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &c3))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(c3->GetMixFormat(&mix)) && mixIsFloat(mix, sampleRate, channels)) {
                UINT32 def = 0, fund = 0, mn = 0, mx = 0;
                if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &def, &fund, &mn, &mx)) && mn > 0 && mn < def &&
                    SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix, nullptr))) {
                    WAVEFORMATEX* cur = nullptr;
                    UINT32 curPeriod = mn;
                    if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&cur, &curPeriod)) && cur) CoTaskMemFree(cur);
                    period = curPeriod;
                    lowLatency = true;
                    c.client = c3;
                }
            }
            if (mix) CoTaskMemFree(mix);
        }
    }

    // 2) Regular shared stream (any format, Windows converts).
    if (!lowLatency) {
        check(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c.client), "Activate(IAudioClient)");
        WAVEFORMATEXTENSIBLE fmt = floatFormat(sampleRate, channels, mask);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        check(c.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kRenderBuffer, 0,
                                   reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr),
              "IAudioClient::Initialize(render)");
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        c.client->GetDevicePeriod(&defPeriod, &minPeriod);
        period = static_cast<std::uint32_t>(defPeriod * sampleRate / 10000000);
    }

    check(c.client->SetEventHandle(c.event.h), "SetEventHandle(render)");
    check(c.client->GetService(IID_PPV_ARGS(&c.render)), "GetService(IAudioRenderClient)");

    UINT32 frames = 0;
    c.client->GetBufferSize(&frames);

    // Keep only ~one period (+ margin) queued instead of the whole buffer: the queue is pure latency.
    std::uint32_t target = 0;
    if (options.targetPaddingMs > 0) {
        target = static_cast<std::uint32_t>(options.targetPaddingMs * 0.001 * sampleRate);
    } else {
        const std::uint32_t margin = std::max<std::uint32_t>(period / 4, sampleRate * 3 / 2000);  // >= 1.5 ms
        target = period + margin;
    }
    target = std::clamp<std::uint32_t>(target, std::min(period, frames), frames);

    c.format = {sampleRate, channels, mask, frames, period, target, lowLatency};
    return c.format;
}

namespace {
// Converts float samples to the device's exclusive-mode sample format.
void convertOut(const float* in, BYTE* out, std::size_t n, int kind) {
    auto clampf = [](float v) { return std::clamp(v, -1.0f, 1.0f); };
    switch (kind) {
        case 0:  // float32
            std::memcpy(out, in, n * sizeof(float));
            break;
        case 1: {  // int32 (also 24-in-32, left-justified)
            auto* o = reinterpret_cast<std::int32_t*>(out);
            for (std::size_t i = 0; i < n; ++i)
                o[i] = static_cast<std::int32_t>(std::lround(static_cast<double>(clampf(in[i])) * 2147483647.0));
            break;
        }
        case 2:  // packed int24
            for (std::size_t i = 0; i < n; ++i) {
                const auto v = static_cast<std::int32_t>(std::lround(clampf(in[i]) * 8388607.0f));
                out[3 * i] = static_cast<BYTE>(v & 0xFF);
                out[3 * i + 1] = static_cast<BYTE>((v >> 8) & 0xFF);
                out[3 * i + 2] = static_cast<BYTE>((v >> 16) & 0xFF);
            }
            break;
        default: {  // int16
            auto* o = reinterpret_cast<std::int16_t*>(out);
            for (std::size_t i = 0; i < n; ++i) o[i] = static_cast<std::int16_t>(std::lround(clampf(in[i]) * 32767.0f));
        }
    }
}
}  // namespace

void RenderStream::start(Callback cb) {
    auto& c = *impl_;
    if (!c.client) throw std::logic_error("RenderStream::start before open");

    // Pre-fill only the target queue with silence (pre-filling the whole buffer would be pure latency).
    // Exclusive mode: exactly one buffer before Start().
    BYTE* data = nullptr;
    const UINT32 prefill = c.format.exclusive ? c.format.bufferFrames : c.format.targetPadding;
    check(c.render->GetBuffer(prefill, &data), "IAudioRenderClient::GetBuffer(prefill)");
    check(c.render->ReleaseBuffer(prefill, AUDCLNT_BUFFERFLAGS_SILENT), "ReleaseBuffer(prefill)");

    c.stopFlag = false;
    check(c.client->Start(), "IAudioClient::Start(render)");
    c.thread = std::thread([this, &c, cb = std::move(cb)] {
        ComScope com;
        MmcssScope mmcss;
        const int kind = static_cast<int>(c.sampleKind);
        LARGE_INTEGER freq, prev{}, now;
        QueryPerformanceFrequency(&freq);
        // A gap of more than 2 periods between wake-ups means at least one deadline was missed.
        const double lateSec = 2.0 * c.format.periodFrames / c.format.sampleRate + 0.0005;
        try {
            while (!c.stopFlag) {
                if (WaitForSingleObject(c.event.h, 200) != WAIT_OBJECT_0) continue;
                if (c.stopFlag) break;
                QueryPerformanceCounter(&now);
                if (prev.QuadPart && static_cast<double>(now.QuadPart - prev.QuadPart) / freq.QuadPart > lateSec)
                    late_.fetch_add(1, std::memory_order_relaxed);
                prev = now;
                if (c.format.exclusive) {  // event = the device wants the next whole buffer
                    const UINT32 n = c.format.bufferFrames;
                    BYTE* buf = nullptr;
                    check(c.render->GetBuffer(n, &buf), "IAudioRenderClient::GetBuffer");
                    cb(c.scratch.data(), n);
                    convertOut(c.scratch.data(), buf, static_cast<std::size_t>(n) * c.format.channels, kind);
                    check(c.render->ReleaseBuffer(n, 0), "IAudioRenderClient::ReleaseBuffer");
                    continue;
                }
                UINT32 padding = 0;
                check(c.client->GetCurrentPadding(&padding), "GetCurrentPadding");
                // Top the queue up to the target only.
                const UINT32 avail = padding >= c.format.targetPadding ? 0 : c.format.targetPadding - padding;
                if (avail == 0) continue;
                BYTE* buf = nullptr;
                check(c.render->GetBuffer(avail, &buf), "IAudioRenderClient::GetBuffer");
                cb(reinterpret_cast<float*>(buf), avail);
                check(c.render->ReleaseBuffer(avail, 0), "IAudioRenderClient::ReleaseBuffer");
            }
        } catch (const std::exception& e) {
            c.setError(e.what());
            failed_ = true;
        }
    });
}

void RenderStream::stop() {
    if (impl_) impl_->join();
}
std::string RenderStream::error() const { return impl_->getError(); }

}  // namespace tac::wasapi

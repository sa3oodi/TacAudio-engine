// TacAudio APO prototype (Audio Processing Object).
//
// A stream effect (SFX) that Windows' audio engine (audiodg.exe) loads onto a playback device.
// Prototype goal: find out whether a game can play 7.1 into a stereo headphone device through it.
//   - It asks the engine for an N-channel input (HKLM\SOFTWARE\TacAudio\Apo "InputChannels",
//     default 8 = 7.1) while the device itself stays stereo, and downmixes to the output.
//   - Every 2 s it logs the peak level of each input channel to
//     C:\ProgramData\TacAudio\apo.log, so we can see whether a game really sends 8 channels.
//
// Real-time rules: APOProcess never allocates, locks or does I/O; the logger thread only reads
// atomics.

#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "apo_engine.h"

namespace {

// {1A057EE8-C9EB-451B-9A59-F6D6FC460F7A}
constexpr CLSID kClsid = {0x1a057ee8, 0xc9eb, 0x451b, {0x9a, 0x59, 0xf6, 0xd6, 0xfc, 0x46, 0x0f, 0x7a}};
constexpr wchar_t kClsidStr[] = L"{1A057EE8-C9EB-451B-9A59-F6D6FC460F7A}";
constexpr wchar_t kName[] = L"TacAudio Spatial Engine (prototype)";
constexpr APO_FLAG kFlags = static_cast<APO_FLAG>(APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH);
constexpr UINT32 kMaxCh = 8;

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0}, g_locks{0};

// ------------------------------------------------------------------ log
std::mutex g_logMutex;

void logLine(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    // The audio service may run with an almost empty environment: ask Windows for ProgramData,
    // and fall back to the service's own temp folder if that is not writable.
    std::wstring file;
    PWSTR pd = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) file = std::wstring(pd) + L"\\TacAudio\\apo.log";
    CoTaskMemFree(pd);
    FILE* f = nullptr;
    if (file.empty() || _wfopen_s(&f, file.c_str(), L"a") || !f) {
        // No file access (the audio service runs with a restricted token): Windows Event Log,
        // Application log, source "TacAudioApo".
        char msg[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        if (HANDLE ev = RegisterEventSourceW(nullptr, L"TacAudioApo")) {
            const char* strings[] = {msg};
            ReportEventA(ev, EVENTLOG_INFORMATION_TYPE, 0, 1, nullptr, 1, 0, strings, nullptr);
            DeregisterEventSource(ev);
        }
        return;
    }
    if (_ftelli64(f) > 2'000'000) {  // keep it small
        fclose(f);
        if (_wfopen_s(&f, file.c_str(), L"w")) f = nullptr;
        if (!f) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d [pid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

}  // namespace

namespace tac::apo {
// Log sink for the engine (apo_engine.cpp).
void apoLog(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    logLine("%s", msg);
}
}  // namespace tac::apo

namespace {

// ------------------------------------------------------------------ status for the app
// Shared by every APO instance in this process (a game usually opens several streams).
// Written about 8x per second to %ProgramData%\TacAudio\apo_status.ini.
std::atomic<float> g_inPeak[8] = {};
std::atomic<float> g_outL{0.0f}, g_outR{0.0f}, g_dynMin{0.0f}, g_dynMax{0.0f};
std::atomic<int> g_streams{0};
std::atomic<unsigned> g_inChannels{0};
std::atomic<unsigned long long> g_lastStatus{0};

void raiseAtomic(std::atomic<float>& a, float v) {
    float c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
    }
}
void lowerAtomic(std::atomic<float>& a, float v) {
    float c = a.load(std::memory_order_relaxed);
    while (v < c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
    }
}
float toDb(float lin) { return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f; }

std::filesystem::path dataPath(const wchar_t* name);

void writeStatusIfDue() {
    const unsigned long long now = GetTickCount64();
    unsigned long long last = g_lastStatus.load();
    if (now - last < 125 || !g_lastStatus.compare_exchange_strong(last, now)) return;
    const unsigned ch = std::min(g_inChannels.load(), 8u);
    char b[512];
    int len = std::snprintf(b, sizeof(b), "time=%llu\nstreams=%d\nchannels=%u\nin=", now, g_streams.load(), ch);
    for (unsigned c = 0; c < ch; ++c) len += std::snprintf(b + len, sizeof(b) - len, "%s%.1f", c ? " " : "", toDb(g_inPeak[c].exchange(0.0f)));
    len += std::snprintf(b + len, sizeof(b) - len, "\noutL=%.1f\noutR=%.1f\ndynMin=%.1f\ndynMax=%.1f\n", toDb(g_outL.exchange(0.0f)),
                         toDb(g_outR.exchange(0.0f)), g_dynMin.exchange(0.0f), g_dynMax.exchange(0.0f));
    const std::filesystem::path tmp = dataPath(L"apo_status.tmp"), dst = dataPath(L"apo_status.ini");
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return;
    fwrite(b, 1, static_cast<size_t>(len), f);
    fclose(f);
    MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);  // fails harmlessly while the app reads
}

std::filesystem::path moduleDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(g_module, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

std::filesystem::path dataPath(const wchar_t* name) {
    PWSTR pd = nullptr;
    std::filesystem::path p = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) p = pd;
    CoTaskMemFree(pd);
    return p / L"TacAudio" / name;
}

std::filesystem::path settingsPath() { return dataPath(L"apo.ini"); }

std::string describe(IAudioMediaType* t) {
    if (!t) return "(none)";
    UNCOMPRESSEDAUDIOFORMAT u{};
    if (FAILED(t->GetUncompressedAudioFormat(&u))) return "(compressed?)";
    char b[160];
    const bool isFloat = u.guidFormatType == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    std::snprintf(b, sizeof(b), "%lu ch, %.0f Hz, %s %lu/%lu bit, mask 0x%lX", u.dwSamplesPerFrame, u.fFramesPerSecond,
                  isFloat ? "float" : "pcm", u.dwValidBitsPerSample, u.dwBytesPerSampleContainer * 8, u.dwChannelMask);
    return b;
}

DWORD configDword(const wchar_t* name, DWORD def) {
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TacAudio\\Apo", name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return def;
    return v;
}

DWORD maskFor(UINT32 ch) {
    switch (ch) {
        case 1: return KSAUDIO_SPEAKER_MONO;
        case 2: return KSAUDIO_SPEAKER_STEREO;
        case 4: return KSAUDIO_SPEAKER_QUAD;
        case 6: return KSAUDIO_SPEAKER_5POINT1;
        case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;  // 0x63F: FL FR FC LFE BL BR SL SR
        default: return 0;
    }
}

// Minimal IAudioMediaType for the formats we suggest (the SDK helper library needs ATL).
class FloatMediaType final : public IAudioMediaType {
public:
    FloatMediaType(float rate, UINT32 ch) {
        wf_ = {};
        wf_.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf_.Format.nChannels = static_cast<WORD>(ch);
        wf_.Format.nSamplesPerSec = static_cast<DWORD>(rate);
        wf_.Format.wBitsPerSample = 32;
        wf_.Format.nBlockAlign = static_cast<WORD>(4 * ch);
        wf_.Format.nAvgBytesPerSec = wf_.Format.nSamplesPerSec * wf_.Format.nBlockAlign;
        wf_.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf_.Samples.wValidBitsPerSample = 32;
        wf_.dwChannelMask = maskFor(ch);
        wf_.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        u_.guidFormatType = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        u_.dwSamplesPerFrame = ch;
        u_.dwBytesPerSampleContainer = 4;
        u_.dwValidBitsPerSample = 32;
        u_.fFramesPerSecond = rate;
        u_.dwChannelMask = wf_.dwChannelMask;
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioMediaType)) {
            *ppv = static_cast<IAudioMediaType*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const long r = --refs_;
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    STDMETHODIMP IsCompressedFormat(BOOL* c) override {
        if (!c) return E_POINTER;
        *c = FALSE;
        return S_OK;
    }
    STDMETHODIMP IsEqual(IAudioMediaType* other, DWORD* flags) override {
        if (!other || !flags) return E_POINTER;
        *flags = 0;
        UNCOMPRESSEDAUDIOFORMAT o{};
        if (FAILED(other->GetUncompressedAudioFormat(&o))) return S_FALSE;
        if (o.guidFormatType == u_.guidFormatType) *flags |= AUDIOMEDIATYPE_EQUAL_FORMAT_TYPES;
        if (o.guidFormatType == u_.guidFormatType && o.dwSamplesPerFrame == u_.dwSamplesPerFrame &&
            o.dwBytesPerSampleContainer == u_.dwBytesPerSampleContainer && o.dwValidBitsPerSample == u_.dwValidBitsPerSample &&
            o.fFramesPerSecond == u_.fFramesPerSecond && o.dwChannelMask == u_.dwChannelMask)
            *flags |= AUDIOMEDIATYPE_EQUAL_FORMAT_DATA | AUDIOMEDIATYPE_EQUAL_FORMAT_USER_DATA;
        return (*flags & AUDIOMEDIATYPE_EQUAL_FORMAT_DATA) ? S_OK : S_FALSE;
    }
    STDMETHODIMP_(const WAVEFORMATEX*) GetAudioFormat() override { return &wf_.Format; }
    STDMETHODIMP GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT* u) override {
        if (!u) return E_POINTER;
        *u = u_;
        return S_OK;
    }

private:
    std::atomic<long> refs_{1};
    WAVEFORMATEXTENSIBLE wf_{};
    UNCOMPRESSEDAUDIOFORMAT u_{};
};

HRESULT makeFloatType(float rate, UINT32 ch, IAudioMediaType** out) {
    *out = new (std::nothrow) FloatMediaType(rate, ch);
    return *out ? S_OK : E_OUTOFMEMORY;
}

bool getFormat(IAudioMediaType* t, UNCOMPRESSEDAUDIOFORMAT& u) { return t && SUCCEEDED(t->GetUncompressedAudioFormat(&u)); }

bool isFloat32(const UNCOMPRESSEDAUDIOFORMAT& u) {
    return u.guidFormatType == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && u.dwBytesPerSampleContainer == 4;
}

// ------------------------------------------------------------------ the APO
class TacApo final : public IAudioProcessingObject,
                     public IAudioProcessingObjectRT,
                     public IAudioProcessingObjectConfiguration,
                     public IAudioSystemEffects {
public:
    // The audio engine creates APOs as aggregated COM objects: `outer` receives the delegating
    // IUnknown calls and holds this object through its non-delegating IUnknown (nd_).
    explicit TacApo(IUnknown* outer) : outer_(outer ? outer : &nd_) {
        nd_.self = this;
        ++g_objects;
    }
    ~TacApo() {
        stopLogger();
        --g_objects;
    }

    // Non-delegating IUnknown: identity and lifetime of the object itself.
    struct NonDelegating final : IUnknown {
        TacApo* self = nullptr;
        STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override { return self->ndQueryInterface(iid, ppv); }
        STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(++self->refs_); }
        STDMETHODIMP_(ULONG) Release() override {
            const long r = --self->refs_;
            if (r == 0) delete self;
            return static_cast<ULONG>(r);
        }
    } nd_;

    // Delegating IUnknown (all interfaces below): forwards to the outer object.
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override { return outer_->QueryInterface(iid, ppv); }
    STDMETHODIMP_(ULONG) AddRef() override { return outer_->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return outer_->Release(); }

    HRESULT ndQueryInterface(REFIID iid, void** ppv) {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (iid == __uuidof(IUnknown)) {
            *ppv = &nd_;
            nd_.AddRef();
            return S_OK;
        }
        if (iid == __uuidof(IAudioProcessingObject))
            *ppv = static_cast<IAudioProcessingObject*>(this);
        else if (iid == __uuidof(IAudioProcessingObjectRT))
            *ppv = static_cast<IAudioProcessingObjectRT*>(this);
        else if (iid == __uuidof(IAudioProcessingObjectConfiguration))
            *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
        else if (iid == __uuidof(IAudioSystemEffects))
            *ppv = static_cast<IAudioSystemEffects*>(this);
        else {
            wchar_t s[64];
            StringFromGUID2(iid, s, 64);
            char a[64];
            WideCharToMultiByte(CP_UTF8, 0, s, -1, a, sizeof(a), nullptr, nullptr);
            logLine("QueryInterface: %s not supported", a);
            return E_NOINTERFACE;
        }
        static_cast<IUnknown*>(*ppv)->AddRef();  // delegating: counts on the outer object
        return S_OK;
    }

    // IAudioProcessingObject
    STDMETHODIMP Reset() override { return S_OK; }
    STDMETHODIMP GetLatency(HNSTIME* t) override {
        if (!t) return E_POINTER;
        *t = 0;
        return S_OK;
    }
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** pp) override {
        if (!pp) return E_POINTER;
        auto* p = static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
        if (!p) return E_OUTOFMEMORY;
        *p = {};
        p->clsid = kClsid;
        p->Flags = kFlags;
        wcscpy_s(p->szFriendlyName, kName);
        wcscpy_s(p->szCopyrightInfo, L"TacAudio");
        p->u32MajorVersion = 0;
        p->u32MinorVersion = 1;
        p->u32MinInputConnections = p->u32MaxInputConnections = 1;
        p->u32MinOutputConnections = p->u32MaxOutputConnections = 1;
        p->u32MaxInstances = 0xffffffff;
        p->u32NumAPOInterfaces = 1;
        p->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
        *pp = p;
        return S_OK;
    }
    STDMETHODIMP Initialize(UINT32 cb, BYTE* data) override {
        wantCh_ = std::clamp<DWORD>(configDword(L"InputChannels", 8), 1, kMaxCh);
        bool discovery = false;
        if (data && cb >= sizeof(APOInitSystemEffects2))
            discovery = reinterpret_cast<APOInitSystemEffects2*>(data)->InitializeForDiscoveryOnly != FALSE;
        logLine("Initialize: %u bytes of init data%s, asking for %lu input channels", cb,
                discovery ? " (discovery only)" : "", wantCh_);
        return S_OK;
    }
    // Input side: ask for float32 with wantCh_ channels at the output's rate.
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* out, IAudioMediaType* req, IAudioMediaType** supported) override {
        if (!supported) return E_POINTER;
        *supported = nullptr;
        UNCOMPRESSEDAUDIOFORMAT r{}, o{};
        const bool haveReq = getFormat(req, r), haveOut = getFormat(out, o);
        HRESULT hr;
        if (haveReq && isFloat32(r) && r.dwSamplesPerFrame == wantCh_ && (!haveOut || r.fFramesPerSecond == o.fFramesPerSecond)) {
            req->AddRef();
            *supported = req;
            hr = S_OK;
        } else {
            const float rate = haveOut ? o.fFramesPerSecond : haveReq ? r.fFramesPerSecond : 48000.0f;
            hr = makeFloatType(rate, wantCh_, supported);
            if (SUCCEEDED(hr)) hr = S_FALSE;
        }
        logLine("IsInputFormatSupported: requested [%s], output [%s] -> %s [%s]", describe(req).c_str(),
                describe(out).c_str(), hr == S_OK ? "OK" : hr == S_FALSE ? "suggest" : "error",
                describe(*supported).c_str());
        return hr;
    }
    // Output side: float32, any channel count the device wants (stereo on headphones).
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* in, IAudioMediaType* req, IAudioMediaType** supported) override {
        if (!supported) return E_POINTER;
        *supported = nullptr;
        UNCOMPRESSEDAUDIOFORMAT r{}, i{};
        const bool haveReq = getFormat(req, r), haveIn = getFormat(in, i);
        HRESULT hr;
        if (haveReq && isFloat32(r) && r.dwSamplesPerFrame >= 1 && r.dwSamplesPerFrame <= kMaxCh &&
            (!haveIn || r.fFramesPerSecond == i.fFramesPerSecond)) {
            req->AddRef();
            *supported = req;
            hr = S_OK;
        } else {
            const float rate = haveIn ? i.fFramesPerSecond : haveReq ? r.fFramesPerSecond : 48000.0f;
            const UINT32 ch = haveReq && r.dwSamplesPerFrame >= 1 && r.dwSamplesPerFrame <= kMaxCh ? r.dwSamplesPerFrame : 2;
            hr = makeFloatType(rate, ch, supported);
            if (SUCCEEDED(hr)) hr = S_FALSE;
        }
        logLine("IsOutputFormatSupported: requested [%s], input [%s] -> %s [%s]", describe(req).c_str(),
                describe(in).c_str(), hr == S_OK ? "OK" : hr == S_FALSE ? "suggest" : "error",
                describe(*supported).c_str());
        return hr;
    }
    STDMETHODIMP GetInputChannelCount(UINT32* n) override {
        if (!n) return E_POINTER;
        *n = inCh_ ? inCh_ : wantCh_;
        return S_OK;
    }

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** in, UINT32 nOut,
                                APO_CONNECTION_DESCRIPTOR** out) override {
        if (nIn != 1 || nOut != 1 || !in || !out || !in[0] || !out[0]) return E_INVALIDARG;
        UNCOMPRESSEDAUDIOFORMAT fi{}, fo{};
        if (!getFormat(in[0]->pFormat, fi) || !getFormat(out[0]->pFormat, fo)) return E_INVALIDARG;
        logLine("LockForProcess: input [%s] -> output [%s], max %u frames", describe(in[0]->pFormat).c_str(),
                describe(out[0]->pFormat).c_str(), in[0]->u32MaxFrameCount);
        if (!isFloat32(fi) || !isFloat32(fo) || fi.dwSamplesPerFrame > kMaxCh || fo.dwSamplesPerFrame > kMaxCh ||
            fi.dwSamplesPerFrame == 0 || fo.dwSamplesPerFrame == 0)
            return APOERR_FORMAT_NOT_SUPPORTED;
        inCh_ = fi.dwSamplesPerFrame;
        outCh_ = fo.dwSamplesPerFrame;
        inMask_ = fi.dwChannelMask;
        silence_.assign(static_cast<size_t>(in[0]->u32MaxFrameCount) * inCh_, 0.0f);
        try {
            engine_ = std::make_unique<tac::apo::ApoEngine>(fi.fFramesPerSecond, inCh_, inMask_, moduleDir(), settingsPath());
        } catch (const std::exception& e) {
            logLine("engine start failed: %s - using plain downmix", e.what());
            engine_.reset();
        }
        locked_ = true;
        ++g_streams;
        g_inChannels = std::max(g_inChannels.load(), inCh_);
        startLogger();
        return S_OK;
    }
    STDMETHODIMP UnlockForProcess() override {
        stopLogger();
        engine_.reset();
        if (locked_) --g_streams;
        locked_ = false;
        writeStatusIfDue();
        logLine("UnlockForProcess (%llu frames processed)", frames_.load());
        return S_OK;
    }

    // IAudioProcessingObjectRT (real-time thread)
    STDMETHODIMP_(void) APOProcess(UINT32 nIn, APO_CONNECTION_PROPERTY** in, UINT32 nOut,
                                   APO_CONNECTION_PROPERTY** out) override {
        if (nIn < 1 || nOut < 1 || !in[0] || !out[0]) return;
        const UINT32 n = in[0]->u32ValidFrameCount;
        auto* dst = reinterpret_cast<float*>(out[0]->pBuffer);
        if (engine_) {  // TacAudio: keeps running on silent input (reverb / filter tails)
            const bool valid = in[0]->u32BufferFlags == BUFFER_VALID;
            const auto* src = valid ? reinterpret_cast<const float*>(in[0]->pBuffer) : silence_.data();
            if (valid) trackPeaks(src, n);
            if (valid || static_cast<size_t>(n) * inCh_ <= silence_.size())
                engine_->process(src, dst, n, outCh_);
            else
                std::memset(dst, 0, static_cast<size_t>(n) * outCh_ * sizeof(float));
            out[0]->u32ValidFrameCount = n;
            out[0]->u32BufferFlags = BUFFER_VALID;
            frames_ += n;
            return;
        }
        if (in[0]->u32BufferFlags != BUFFER_VALID) {
            std::memset(dst, 0, static_cast<size_t>(n) * outCh_ * sizeof(float));
            out[0]->u32ValidFrameCount = n;
            out[0]->u32BufferFlags = BUFFER_SILENT;
            frames_ += n;
            return;
        }
        const auto* src = reinterpret_cast<const float*>(in[0]->pBuffer);
        trackPeaks(src, n);

        if (inCh_ == outCh_) {
            if (dst != src) std::memcpy(dst, src, static_cast<size_t>(n) * inCh_ * sizeof(float));
        } else if (inCh_ == 8 && outCh_ == 2) {
            // Plain 7.1 -> stereo downmix (placeholder for the binaural renderer).
            constexpr float c = 0.7071f, g = 0.5f;
            for (UINT32 i = 0; i < n; ++i) {
                const float* s = src + i * 8;  // FL FR FC LFE BL BR SL SR
                dst[i * 2 + 0] = g * (s[0] + c * s[2] + 0.5f * s[3] + c * s[4] + c * s[6]);
                dst[i * 2 + 1] = g * (s[1] + c * s[2] + 0.5f * s[3] + c * s[5] + c * s[7]);
            }
        } else {
            // Generic: copy the channels both sides have, silence the rest.
            for (UINT32 i = 0; i < n; ++i)
                for (UINT32 o = 0; o < outCh_; ++o) dst[i * outCh_ + o] = o < inCh_ ? src[i * inCh_ + o] : 0.0f;
        }
        out[0]->u32ValidFrameCount = n;
        out[0]->u32BufferFlags = BUFFER_VALID;
        frames_ += n;
    }
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 outFrames) override { return outFrames; }
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 inFrames) override { return inFrames; }

private:
    // Per-channel peaks for the log (real time: atomics only).
    void trackPeaks(const float* src, UINT32 n) noexcept {
        float pk[kMaxCh] = {};
        for (UINT32 i = 0; i < n; ++i)
            for (UINT32 c = 0; c < inCh_; ++c) pk[c] = std::max(pk[c], std::fabs(src[i * inCh_ + c]));
        for (UINT32 c = 0; c < inCh_; ++c) {
            raiseAtomic(peak_[c], pk[c]);
            raiseAtomic(g_inPeak[c], pk[c]);
        }
    }

    void startLogger() {
        stopLogger();
        logStop_ = false;
        logger_ = std::thread([this] {
            using namespace std::chrono;
            auto next = steady_clock::now();
            while (!logStop_) {
                std::this_thread::sleep_for(milliseconds(50));
                if (engine_) {  // feed the shared status the app reads
                    float l, r, mn, mx;
                    engine_->takeStats(l, r, mn, mx);
                    raiseAtomic(g_outL, l);
                    raiseAtomic(g_outR, r);
                    lowerAtomic(g_dynMin, mn);
                    raiseAtomic(g_dynMax, mx);
                }
                writeStatusIfDue();
                if (steady_clock::now() < next) continue;
                next += seconds(10);
                char b[256];
                int len = std::snprintf(b, sizeof(b), "levels (dBFS) over 10 s, %llu frames:", frames_.load());
                for (UINT32 c = 0; c < inCh_ && len < static_cast<int>(sizeof(b)) - 12; ++c) {
                    const float p = peak_[c].exchange(0.0f);
                    len += std::snprintf(b + len, sizeof(b) - len, " %.0f", p > 1e-6f ? 20.0f * std::log10(p) : -120.0f);
                }
                logLine("%s", b);
            }
        });
    }
    void stopLogger() {
        logStop_ = true;
        if (logger_.joinable()) logger_.join();
    }

    IUnknown* outer_;
    std::atomic<long> refs_{1};
    DWORD wantCh_ = 8;
    UINT32 inCh_ = 0, outCh_ = 0;
    DWORD inMask_ = 0;
    bool locked_ = false;
    std::atomic<unsigned long long> frames_{0};
    std::atomic<float> peak_[kMaxCh] = {};
    std::thread logger_;
    std::unique_ptr<tac::apo::ApoEngine> engine_;
    std::vector<float> silence_;
    std::atomic<bool> logStop_{true};
};

// ------------------------------------------------------------------ COM plumbing
class Factory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        // COM rule: an aggregating creator must ask for IUnknown (it gets the non-delegating one).
        if (outer && iid != __uuidof(IUnknown)) return CLASS_E_NOAGGREGATION;
        auto* apo = new (std::nothrow) TacApo(outer);
        if (!apo) return E_OUTOFMEMORY;
        const HRESULT hr = apo->nd_.QueryInterface(iid, ppv);
        apo->nd_.Release();  // drop the creation reference
        logLine("CreateInstance (%s) -> 0x%08lX", outer ? "aggregated" : "standalone", static_cast<unsigned long>(hr));
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};
Factory g_factory;

LSTATUS setString(HKEY k, const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                          static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}
LSTATUS setDword(HKEY k, const wchar_t* name, DWORD v) {
    return RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** ppv) {
    wchar_t c[64], i[64];
    StringFromGUID2(clsid, c, 64);
    StringFromGUID2(iid, i, 64);
    const HRESULT hr = clsid != kClsid ? CLASS_E_CLASSNOTAVAILABLE : g_factory.QueryInterface(iid, ppv);
    logLine("DllGetClassObject(%ls, %ls) -> 0x%08lX", c, i, static_cast<unsigned long>(hr));
    return hr;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

// COM class + audio-engine APO registration (HKLM, needs administrator rights).
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    HKEY k;
    const std::wstring clsKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)) return E_ACCESSDENIED;
    setString(k, nullptr, kName);
    RegCloseKey(k);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, (clsKey + L"\\InprocServer32").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                        nullptr))
        return E_ACCESSDENIED;
    setString(k, nullptr, path);
    setString(k, L"ThreadingModel", L"Both");
    RegCloseKey(k);

    const std::wstring apoKey = std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + kClsidStr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, apoKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)) return E_ACCESSDENIED;
    wchar_t iid[64];
    StringFromGUID2(__uuidof(IAudioProcessingObject), iid, 64);
    setString(k, L"FriendlyName", kName);
    setString(k, L"Copyright", L"TacAudio");
    setDword(k, L"MajorVersion", 0);
    setDword(k, L"MinorVersion", 1);
    setDword(k, L"Flags", kFlags);
    setDword(k, L"MinInputConnections", 1);
    setDword(k, L"MaxInputConnections", 1);
    setDword(k, L"MinOutputConnections", 1);
    setDword(k, L"MaxOutputConnections", 1);
    setDword(k, L"MaxInstances", 0xffffffff);
    setDword(k, L"NumAPOInterfaces", 1);
    setString(k, L"APOInterface0", iid);
    RegCloseKey(k);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                   (std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + kClsidStr).c_str());
    return S_OK;
}

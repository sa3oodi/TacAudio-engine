// tacapo-setup: installs / removes the TacAudio APO prototype on one playback device.
//
//   tacapo-setup status
//   tacapo-setup install --device "Game" [--channels 8] [--log <file>]     (administrator)
//   tacapo-setup uninstall [--log <file>]                                  (administrator)
//   tacapo-setup set-channels <n> [--log <file>]                           (administrator)
//
// install: copies TacAudioApo.dll to %ProgramFiles%\TacAudio\apo, registers it, gives the audio
// service write access to %ProgramData%\TacAudio (log), backs up the device's current stream
// effect (SFX) to HKLM\SOFTWARE\TacAudio\Apo\Backup and puts TacAudio's in its place, then
// restarts the Windows audio service. uninstall puts every backed-up value back exactly.

#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "platform/wasapi.h"

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kClsid[] = L"{1A057EE8-C9EB-451B-9A59-F6D6FC460F7A}";
constexpr wchar_t kSfxValue[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";  // PKEY_FX_StreamEffectClsid
constexpr wchar_t kRenderKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
constexpr wchar_t kConfigKey[] = L"SOFTWARE\\TacAudio\\Apo";
constexpr wchar_t kBackupKey[] = L"SOFTWARE\\TacAudio\\Apo\\Backup";
constexpr wchar_t kNone[] = L"<none>";  // backup marker: the device had no SFX value
constexpr const wchar_t* kSlots[] = {L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1",   // PKEY_FX_PreMixEffectClsid (LFX)
                                     L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"};  // PKEY_FX_StreamEffectClsid (SFX)

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

[[noreturn]] void fail(const std::string& msg) {
    std::printf("ERROR: %s\n", msg.c_str());
    std::fflush(stdout);
    std::exit(1);
}

bool isAdmin() {
    BOOL member = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(nullptr, admins, &member);
        FreeSid(admins);
    }
    return member != FALSE;
}

fs::path exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return fs::path(p).parent_path();
}

fs::path knownDir(const wchar_t* env) {
    wchar_t p[MAX_PATH];
    if (!GetEnvironmentVariableW(env, p, MAX_PATH)) fail("environment variable missing");
    return p;
}

fs::path installDir() { return knownDir(L"ProgramFiles") / L"TacAudio" / L"apo"; }
fs::path logFile() { return knownDir(L"ProgramData") / L"TacAudio" / L"apo.log"; }

std::wstring readString(HKEY root, const std::wstring& key, const wchar_t* name, bool* exists = nullptr) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    const LSTATUS r = RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size);
    if (exists) *exists = r == ERROR_SUCCESS;
    return r == ERROR_SUCCESS ? std::wstring(buf) : std::wstring();
}

void writeString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& v) {
    HKEY k;
    LSTATUS r = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (r == ERROR_ACCESS_DENIED || r == ERROR_FILE_NOT_FOUND)
        r = RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE, &k);  // existing key we may only set values in
    if (r != ERROR_SUCCESS) fail("cannot open registry key " + narrow(key) + " (error " + std::to_string(r) + ")");
    r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) fail("cannot write registry value in " + narrow(key) + " (error " + std::to_string(r) + ")");
}

void deleteValue(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, name);
    RegCloseKey(k);
}

void writeDword(const std::wstring& key, const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr))
        fail("cannot create " + narrow(key));
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
}

std::vector<std::wstring> subkeys(const std::wstring& key) {
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = 256;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(k);
    return out;
}

std::vector<std::wstring> valueNames(const std::wstring& key) {
    std::vector<std::wstring> out;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = 256;
        if (RegEnumValueW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(k);
    return out;
}

std::wstring endpointName(const std::wstring& guid) {
    return readString(HKEY_LOCAL_MACHINE, std::wstring(kRenderKey) + L"\\" + guid + L"\\Properties",
                      L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
}

// Restarts Windows Audio and the Audio Endpoint Builder, which caches each device's effect
// settings (all sound stops for a few seconds; audiodg reloads its APOs afterwards).
void restartAudio() {
    std::printf("restarting the Windows audio services...\n");
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) fail("cannot open the service manager");
    auto open = [&](const wchar_t* name) {
        SC_HANDLE s = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
        if (!s) fail("cannot open service " + narrow(name));
        return s;
    };
    auto waitFor = [](SC_HANDLE s, DWORD state) {
        SERVICE_STATUS st{};
        for (int i = 0; i < 150 && QueryServiceStatus(s, &st) && st.dwCurrentState != state; ++i) Sleep(100);
    };
    SC_HANDLE audio = open(L"Audiosrv"), builder = open(L"AudioEndpointBuilder");
    SERVICE_STATUS st{};
    ControlService(audio, SERVICE_CONTROL_STOP, &st);
    waitFor(audio, SERVICE_STOPPED);
    ControlService(builder, SERVICE_CONTROL_STOP, &st);
    waitFor(builder, SERVICE_STOPPED);
    for (SC_HANDLE s : {builder, audio}) {
        if (!StartServiceW(s, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            fail("cannot start an audio service (error " + std::to_string(GetLastError()) + ")");
        waitFor(s, SERVICE_RUNNING);
    }
    CloseServiceHandle(audio);
    CloseServiceHandle(builder);
    CloseServiceHandle(scm);
    std::printf("audio services running\n");
}

int run(const wchar_t* cmdline) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring c = cmdline;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

void requireAdmin() {
    if (!isAdmin()) fail("this command needs administrator rights (run it from an elevated prompt)");
}

// ------------------------------------------------------------------ commands
int cmdInstall(const std::string& device, DWORD channels) {
    requireAdmin();
    tac::wasapi::ComScope com;
    const auto dev = tac::wasapi::findDevice(device, true);
    if (!dev) fail("no playback device matches '" + device + "'");
    const auto dot = dev->id.rfind(L'.');
    if (dot == std::wstring::npos) fail("unexpected device id");
    const std::wstring guid = dev->id.substr(dot + 1);
    const std::wstring fx = std::wstring(kRenderKey) + L"\\" + guid + L"\\FxProperties";
    std::printf("device   : %s\n           %s\n", dev->name.c_str(), narrow(guid).c_str());

    // 1) DLL into Program Files (the audio service cannot read the user profile).
    const fs::path src = exeDir() / L"TacAudioApo.dll", dstDir = installDir(), dst = dstDir / L"TacAudioApo.dll";
    if (!fs::exists(src)) fail("TacAudioApo.dll not found next to tacapo-setup.exe");
    fs::create_directories(dstDir);
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {  // loaded by audiodg: stop audio, copy, the restart below brings it back
        std::printf("DLL in use - stopping audio to replace it\n");
        restartAudio();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) fail("cannot copy the DLL: " + ec.message());
    }
    std::printf("copied   : %s\n", narrow(dst.wstring()).c_str());

    // 2) COM + APO registration.
    HMODULE m = LoadLibraryW(dst.c_str());
    if (!m) fail("cannot load the installed DLL");
    auto reg = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(m, "DllRegisterServer"));
    const HRESULT hr = reg ? reg() : E_FAIL;
    FreeLibrary(m);
    if (FAILED(hr)) fail("DllRegisterServer failed");
    std::printf("registered APO %s\n", narrow(kClsid).c_str());

    // 3) Log folder writable by the audio service (LOCAL SERVICE, S-1-5-19).
    const fs::path logDir = logFile().parent_path();
    fs::create_directories(logDir);
    run((L"icacls \"" + logDir.wstring() + L"\" /grant *S-1-5-19:(OI)(CI)M /grant *S-1-5-32-545:(OI)(CI)RX").c_str());
    fs::remove(logFile(), ec);

    // 4) Config + backup + attach.
    writeDword(kConfigKey, L"InputChannels", channels);
    for (const wchar_t* slot : kSlots) {
        bool had = false;
        const std::wstring old = readString(HKEY_LOCAL_MACHINE, fx, slot, &had);
        const std::wstring backupName = guid + L"|" + slot;
        bool backedUp = false;
        readString(HKEY_LOCAL_MACHINE, kBackupKey, backupName.c_str(), &backedUp);
        if (old != kClsid && !backedUp) {
            writeString(HKEY_LOCAL_MACHINE, kBackupKey, backupName.c_str(), had ? old : std::wstring(kNone));
            std::printf("backup   : %s was %s\n", narrow(slot).c_str(), had ? narrow(old).c_str() : "(none)");
        }
        writeString(HKEY_LOCAL_MACHINE, fx, slot, kClsid);
    }
    std::printf("attached : TacAudio APO in the pre-mix and stream effect slots (asking for %lu channels)\n", channels);

    restartAudio();
    std::printf("\nInstalled. Check with: tacapo-setup status\n");
    return 0;
}

int cmdUninstall() {
    requireAdmin();
    int restored = 0;
    for (const auto& name : valueNames(kBackupKey)) {  // "<endpoint guid>|<slot>" (or just the guid: SFX slot)
        const std::wstring old = readString(HKEY_LOCAL_MACHINE, kBackupKey, name.c_str());
        const auto bar = name.find(L'|');
        const std::wstring guid = name.substr(0, bar);
        const std::wstring slot = bar == std::wstring::npos ? std::wstring(kSfxValue) : name.substr(bar + 1);
        const std::wstring fx = std::wstring(kRenderKey) + L"\\" + guid + L"\\FxProperties";
        if (old == kNone) deleteValue(HKEY_LOCAL_MACHINE, fx, slot.c_str());
        else writeString(HKEY_LOCAL_MACHINE, fx, slot.c_str(), old);
        std::printf("restored : %s %s -> %s\n", narrow(endpointName(guid)).c_str(), narrow(slot).c_str(),
                    old == kNone ? "(none)" : narrow(old).c_str());
        ++restored;
    }
    // Any slot still pointing at us without a backup (should not happen): remove the value.
    for (const auto& guid : subkeys(kRenderKey)) {
        const std::wstring fx = std::wstring(kRenderKey) + L"\\" + guid + L"\\FxProperties";
        for (const wchar_t* slot : kSlots)
            if (readString(HKEY_LOCAL_MACHINE, fx, slot) == kClsid) {
                deleteValue(HKEY_LOCAL_MACHINE, fx, slot);
                std::printf("detached : %s %s\n", narrow(endpointName(guid)).c_str(), narrow(slot).c_str());
            }
    }
    restartAudio();  // unloads the DLL
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + kClsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TacAudio\\Apo");
    std::error_code ec;
    fs::remove_all(installDir(), ec);
    if (ec) std::printf("note: could not delete %s yet (%s)\n", narrow(installDir().wstring()).c_str(), ec.message().c_str());
    std::printf("\nUninstalled (%d device setting(s) restored).\n", restored);
    return 0;
}

int cmdSetChannels(DWORD channels) {
    requireAdmin();
    writeDword(kConfigKey, L"InputChannels", channels);
    std::printf("input channels -> %lu\n", channels);
    restartAudio();
    return 0;
}

int cmdStatus() {
    bool any = false;
    for (const auto& guid : subkeys(kRenderKey)) {
        const std::wstring fx = std::wstring(kRenderKey) + L"\\" + guid + L"\\FxProperties";
        if (readString(HKEY_LOCAL_MACHINE, fx, kSfxValue) == kClsid) {
            std::printf("attached to : %s\n", narrow(endpointName(guid)).c_str());
            any = true;
        }
    }
    if (!any) std::printf("attached to : (no device)\n");
    std::printf("DLL         : %s\n", fs::exists(installDir() / L"TacAudioApo.dll") ? "installed" : "not installed");

    tac::wasapi::ComScope com;
    std::printf("\nWhat apps see now (shared-mode mix format):\n");
    for (const auto& d : tac::wasapi::listDevices(true))
        std::printf("  %-48s %u ch, mask 0x%X\n", d.name.c_str(), d.channels, d.channelMask);

    std::printf("\nLast log lines (%s):\n", narrow(logFile().wstring()).c_str());
    std::ifstream f(logFile());
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    for (size_t i = lines.size() > 30 ? lines.size() - 30 : 0; i < lines.size(); ++i) std::printf("  %s\n", lines[i].c_str());
    if (lines.empty()) std::printf("  (empty - the APO has not been loaded yet)\n");
    return 0;
}

// Plays a 1 kHz tone on 8 channels at once for `seconds`, channel k at -30 - 3k dBFS, as an
// 8-channel shared stream (Windows converts it to the engine format). If the APO log then shows
// eight different levels (-30, -33, ... -51), the device carries discrete 7.1 into the APO.
int cmdPlay71(const std::string& device, double seconds) {
    tac::wasapi::ComScope com;
    const auto dev = tac::wasapi::findDevice(device, true);
    if (!dev) fail("no playback device matches '" + device + "'");
    constexpr std::uint32_t kRate = 48000, kCh = 8;
    tac::wasapi::RenderStream ren;
    tac::wasapi::RenderOptions o;
    o.lowLatency = false;
    const auto f = ren.open(*dev, kRate, kCh, o);
    std::printf("playing 8-channel test tone on '%s' (stream: %u ch) for %.0f s ...\n", dev->name.c_str(), f.channels, seconds);
    std::atomic<std::uint64_t> pos{0};
    const std::uint64_t total = static_cast<std::uint64_t>(seconds * kRate);
    ren.start([&](float* out, std::size_t n) {
        const std::uint64_t p = pos.load();
        for (std::size_t i = 0; i < n; ++i) {
            const float s = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(p + i) / kRate));
            for (std::uint32_t c = 0; c < kCh; ++c)
                out[i * kCh + c] = p + i < total ? s * std::pow(10.0f, (-30.0f - 3.0f * static_cast<float>(c)) / 20.0f) : 0.0f;
        }
        pos.store(p + n);
    });
    while (pos.load() < total + kRate / 2 && !ren.failed()) Sleep(50);
    ren.stop();
    if (ren.failed()) fail("render: " + ren.error());
    std::printf("done - expected APO levels: -30 -33 -36 -39 -42 -45 -48 -51\n");
    return 0;
}

// Loads TacAudioApo.dll from this folder in-process and drives it like the audio engine would:
// format negotiation, LockForProcess, and processing of valid + silent buffers.
int cmdSelfTest() {
    HMODULE m = LoadLibraryW((exeDir() / L"TacAudioApo.dll").c_str());
    if (!m) fail("cannot load TacAudioApo.dll");
    auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(m, "DllGetClassObject"));
    CLSID clsid;
    CLSIDFromString(kClsid, &clsid);
    IClassFactory* cf = nullptr;
    if (!getClass || FAILED(getClass(clsid, __uuidof(IClassFactory), reinterpret_cast<void**>(&cf)))) fail("no class factory");
    IAudioProcessingObject* apo = nullptr;
    if (FAILED(cf->CreateInstance(nullptr, __uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo)))) fail("CreateInstance");
    IAudioProcessingObjectRT* rt = nullptr;
    IAudioProcessingObjectConfiguration* cfg = nullptr;
    apo->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&rt));
    apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), reinterpret_cast<void**>(&cfg));
    if (!rt || !cfg) fail("missing RT / configuration interface");
    APO_REG_PROPERTIES* props = nullptr;
    if (FAILED(apo->GetRegistrationProperties(&props)) || !props) fail("GetRegistrationProperties");
    std::printf("APO      : %s, flags 0x%X\n", narrow(props->szFriendlyName).c_str(), static_cast<unsigned>(props->Flags));
    CoTaskMemFree(props);
    if (FAILED(apo->Initialize(0, nullptr))) fail("Initialize");

    IAudioMediaType *outT = nullptr, *inT = nullptr;
    HRESULT hr = apo->IsOutputFormatSupported(nullptr, nullptr, &outT);
    if (FAILED(hr) || !outT) fail("IsOutputFormatSupported");
    hr = apo->IsInputFormatSupported(outT, nullptr, &inT);
    if (FAILED(hr) || !inT) fail("IsInputFormatSupported");
    UNCOMPRESSEDAUDIOFORMAT fi{}, fo{};
    inT->GetUncompressedAudioFormat(&fi);
    outT->GetUncompressedAudioFormat(&fo);
    std::printf("formats  : in %lu ch -> out %lu ch @ %.0f Hz\n", fi.dwSamplesPerFrame, fo.dwSamplesPerFrame, fo.fFramesPerSecond);
    IAudioMediaType* again = nullptr;  // asking again with the suggestion must be accepted
    if (apo->IsInputFormatSupported(outT, inT, &again) != S_OK) fail("input suggestion not accepted on re-ask");
    again->Release();

    constexpr UINT32 kFrames = 480;
    const UINT32 ci = fi.dwSamplesPerFrame, co = fo.dwSamplesPerFrame;
    std::vector<float> inBuf(kFrames * ci), outBuf(kFrames * co, 99.0f);
    APO_CONNECTION_DESCRIPTOR din{}, dout{};
    din.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    din.pBuffer = reinterpret_cast<UINT_PTR>(inBuf.data());
    din.u32MaxFrameCount = kFrames;
    din.pFormat = inT;
    din.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    dout = din;
    dout.pBuffer = reinterpret_cast<UINT_PTR>(outBuf.data());
    dout.pFormat = outT;
    APO_CONNECTION_DESCRIPTOR *pi = &din, *po = &dout;
    if (FAILED(cfg->LockForProcess(1, &pi, 1, &po))) fail("LockForProcess");

    APO_CONNECTION_PROPERTY pin{}, pout{};
    pin.pBuffer = din.pBuffer;
    pin.u32ValidFrameCount = kFrames;
    pin.u32BufferFlags = BUFFER_VALID;
    pin.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    pout = pin;
    pout.pBuffer = dout.pBuffer;
    pout.u32BufferFlags = BUFFER_INVALID;
    APO_CONNECTION_PROPERTY *ppi = &pin, *ppo = &pout;
    bool ok = true;
    for (UINT32 c = 0; c < ci; ++c) {  // a unit impulse per input channel -> must reach the output
        std::fill(inBuf.begin(), inBuf.end(), 0.0f);
        inBuf[10 * ci + c] = 1.0f;
        rt->APOProcess(1, &ppi, 1, &ppo);
        float l = outBuf[10 * co], r = co > 1 ? outBuf[10 * co + 1] : 0.0f;
        std::printf("  input ch %u -> out L %.3f  R %.3f\n", c, l, r);
        ok &= pout.u32BufferFlags == BUFFER_VALID && pout.u32ValidFrameCount == kFrames && (l != 0.0f || r != 0.0f);
    }
    pin.u32BufferFlags = BUFFER_SILENT;
    rt->APOProcess(1, &ppi, 1, &ppo);
    ok &= pout.u32BufferFlags == BUFFER_SILENT && outBuf[0] == 0.0f;
    cfg->UnlockForProcess();
    inT->Release();
    outT->Release();
    rt->Release();
    cfg->Release();
    apo->Release();
    cf->Release();
    std::printf("%s\n", ok ? "SELFTEST PASS" : "SELFTEST FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    auto opt = [&](const char* name, const std::string& def) {
        for (size_t i = 0; i + 1 < a.size(); ++i)
            if (a[i] == name) return a[i + 1];
        return def;
    };
    const std::string log = opt("--log", "");
    if (!log.empty()) {
        FILE* f = nullptr;
        freopen_s(&f, log.c_str(), "w", stdout);
    }
    try {
        const std::string cmd = a.empty() ? "" : a[0];
        if (cmd == "status") return cmdStatus();
        if (cmd == "selftest") return cmdSelfTest();
        if (cmd == "play71") return cmdPlay71(opt("--device", "Game"), std::stod(opt("--seconds", "5")));
        if (cmd == "restart-audio") {
            requireAdmin();
            restartAudio();
            return 0;
        }
        if (cmd == "install") return cmdInstall(opt("--device", "Game"), static_cast<DWORD>(std::stoul(opt("--channels", "8"))));
        if (cmd == "uninstall") return cmdUninstall();
        if (cmd == "set-channels" && a.size() > 1) return cmdSetChannels(static_cast<DWORD>(std::stoul(a[1])));
        std::printf("usage: tacapo-setup status | install --device <name> [--channels 8] | uninstall | set-channels <n>\n");
        return 2;
    } catch (const std::exception& e) {
        fail(e.what());
    }
}

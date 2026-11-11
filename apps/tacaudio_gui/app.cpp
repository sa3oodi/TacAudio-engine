#include "app.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <numbers>
#include <thread>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "cli.h"
#include "core/biquad.h"
#include "core/dynamics.h"
#include "core/headphone_eq.h"
#include "core/layout.h"
#include "core/wav.h"
#include "platform/wasapi.h"
#include "ui_widgets.h"

namespace fs = std::filesystem;

namespace tac {

using namespace ui;

// ------------------------------------------------------------------ settings file
namespace {
fs::path settingsPath() {
    PWSTR p = nullptr;
    fs::path dir = fs::temp_directory_path();
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir / "TacAudio" / "settings.ini";
}

const std::vector<std::string> kRooms = {"neutral", "balanced", "spacious"};
const std::vector<std::string> kRoomLabels = {"Neutral", "Balanced", "Spacious"};
const std::vector<std::string> kPresets = {"light", "medium", "strong"};
const std::vector<std::string> kPresetLabels = {"Light", "Medium", "Strong"};

int indexOf(const std::vector<std::string>& v, const std::string& s, int fallback) {
    const auto it = std::find(v.begin(), v.end(), s);
    return it == v.end() ? fallback : static_cast<int>(it - v.begin());
}

float toDb(float lin) { return lin > 1e-6f ? 20.0f * std::log10(lin) : -120.0f; }

void openShellUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

// Wrapped paragraph.
void para(const char* s, ImU32 color = ui::col::Dim, float size = 13.5f) {
    ImGui::PushFont(ui::fonts.regular, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Primary (orange) or secondary button; drawn greyed out and inert when disabled.
bool actionButton(const char* label, bool primary, bool enabled = true, float width = 0.0f) {
    const ImU32 bg = !enabled ? ui::col::Well : primary ? ui::col::Accent : ui::col::CardHi;
    const ImU32 fg = !enabled ? ui::col::Faint : primary ? IM_COL32(20, 14, 8, 255) : ui::col::Text;
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ui::pillButton(label, bg, fg, width);
    ImGui::EndDisabled();
    return clicked && enabled;
}

}  // namespace

AppSettings loadSettings() {
    AppSettings s;
    std::ifstream f(settingsPath());
    // Settings written before the setup assistant existed belong to users who are set up already.
    s.setupDone = f.is_open();
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "source") s.dev.source = v;
            else if (k == "output") s.dev.output = v;
            else if (k == "exclusive") s.dev.exclusive = v == "1";
            else if (k == "head") s.sound.head = v;
            else if (k == "room") s.sound.room = v;
            else if (k == "punch") s.sound.punch = std::stof(v);
            else if (k == "volume") s.sound.volumeDb = std::stof(v);
            else if (k == "3d") s.sound.enabled3d = v == "1";
            else if (k == "dynOn") s.dynOn = v == "1";
            else if (k == "dynPreset") s.dynPreset = v;
            else if (k == "stepsOn") s.stepsOn = v == "1";
            else if (k == "stepsLevel") s.stepsLevel = std::stof(v);
            else if (k == "bass") s.sound.bassDb = std::stof(v);
            else if (k == "stepsSmart") s.stepsSmart = v == "1";
            else if (k == "eqOn") s.eqOn = v == "1";
            else if (k == "eq") s.eqText = v;
            else if (k == "eqName") s.eqName = v;
            else if (k == "autoEngine") s.autoEngine = v == "1";
            else if (k == "closeToTray") s.closeToTray = v == "1";
            else if (k == "hotkeys") s.hotkeys = v == "1";
            else if (k == "setupDone") s.setupDone = v == "1";
            else if (k == "autoProfiles") s.autoProfiles = v == "1";
            else if (k == "checkUpdates") s.checkUpdates = v == "1";
            else if (k == "overlayOn") s.overlayOn = v == "1";
            else if (k == "overlaySize") s.overlaySize = std::clamp(std::stof(v), 40.0f, 300.0f);
            else if (k == "overlayOpacity") s.overlayOpacity = std::clamp(std::stof(v), 10.0f, 100.0f);
            else if (k == "overlayPos") s.overlayPos = std::clamp(std::stoi(v), 0, 4);
            else if (k == "overlayLevel") s.overlayLevel = std::clamp(std::stoi(v), 0, 2);
        } catch (const std::exception&) {
            // malformed value: keep the default
        }
    }
    if (indexOf(kPresets, s.dynPreset, -1) < 0) s.dynPreset = "medium";
    if (indexOf(kRooms, s.sound.room, -1) < 0) s.sound.room = "balanced";
    s.startWithWindows = autostartEnabled();  // the registry is the source of truth
    return s;
}

namespace {
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"TacAudio";
}  // namespace

bool autostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    const bool present = RegQueryValueExW(k, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return present;
}

void setAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --minimized";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

void saveSettings(const AppSettings& s) {
    std::error_code ec;
    fs::create_directories(settingsPath().parent_path(), ec);
    std::ofstream f(settingsPath());
    f << "source=" << s.dev.source << "\noutput=" << s.dev.output << "\nexclusive=" << (s.dev.exclusive ? 1 : 0)
      << "\nhead=" << s.sound.head << "\nroom=" << s.sound.room << "\npunch=" << s.sound.punch
      << "\nvolume=" << s.sound.volumeDb << "\n3d=" << (s.sound.enabled3d ? 1 : 0) << "\ndynOn=" << (s.dynOn ? 1 : 0)
      << "\ndynPreset=" << s.dynPreset << "\nstepsOn=" << (s.stepsOn ? 1 : 0) << "\nstepsLevel=" << s.stepsLevel
      << "\nbass=" << s.sound.bassDb << "\nstepsSmart=" << (s.stepsSmart ? 1 : 0) << "\nautoEngine=" << (s.autoEngine ? 1 : 0)
      << "\ncloseToTray=" << (s.closeToTray ? 1 : 0) << "\nhotkeys=" << (s.hotkeys ? 1 : 0)
      << "\nsetupDone=" << (s.setupDone ? 1 : 0) << "\nautoProfiles=" << (s.autoProfiles ? 1 : 0)
      << "\ncheckUpdates=" << (s.checkUpdates ? 1 : 0) << "\neqOn=" << (s.eqOn ? 1 : 0) << "\neq=" << s.eqText
      << "\neqName=" << s.eqName << "\noverlayOn=" << (s.overlayOn ? 1 : 0) << "\noverlaySize=" << s.overlaySize
      << "\noverlayOpacity=" << s.overlayOpacity << "\noverlayPos=" << s.overlayPos
      << "\noverlayLevel=" << s.overlayLevel << "\n";
}

namespace {
SoundBundle bundleOf(const AppSettings& s) {
    SoundBundle b;
    b.sound = s.sound;
    b.dynOn = s.dynOn;
    b.dynPreset = s.dynPreset;
    b.stepsOn = s.stepsOn;
    b.stepsLevel = s.stepsLevel;
    b.stepsSmart = s.stepsSmart;
    return b;
}
void applyBundle(AppSettings& s, const SoundBundle& b) {
    s.sound = b.sound;
    s.dynOn = b.dynOn;
    s.dynPreset = b.dynPreset;
    s.stepsOn = b.stepsOn;
    s.stepsLevel = b.stepsLevel;
    s.stepsSmart = b.stepsSmart;
}
}  // namespace

// ------------------------------------------------------------------ app
App::App(bool previewRunning) : preview_(previewRunning) {
    cfg_ = loadSettings();
    // Game profiles: start on Default (a game profile is picked once its game is in front).
    profiles_ = loadProfiles(bundleOf(cfg_));
    applyBundle(cfg_, profiles_.front().bundle);
    saved_ = cfg_;
    lastStatus_ = lastSave_ = std::chrono::steady_clock::now();
    {  // dev check of the listening test without sound on the headphones (smoke test only)
        char dev[128] = {};
        if (GetEnvironmentVariableA("TACAUDIO_SMOKE_HEADTEST", dev, sizeof(dev)) && dev[0]) htDevice_ = dev;
    }
    refreshDevices();
    if (!preview_) engineOutdated_ = direct_ && apoctl::engineOutdated();
    for (const auto& h : cli::listHeads()) {
        headNames_.push_back(h.name);
        headDescs_.push_back(h.description);
    }
    if (preview_) {  // plausible live values so the layout can be reviewed in a screenshot
        status_.running = true;
        status_.engine.running = true;
        status_.engine.bufferMs = 12.4;
        status_.engine.targetMs = 10.7;
        status_.engine.cpuLoad = 0.023;
        status_.engine.dynMinGainDb = -11.8f;
        status_.engine.dynMaxGainDb = 5.6f;
        status_.latencyMs = 25.0;
        status_.inGapMs = 10.4;
        status_.source = "CABLE Input (VB-Audio Virtual Cable) (8 ch)";
        status_.output = "Game (TC-HELICON GoXLR Mini) (exclusive 3 ms)";
        for (int i = 0; i < 120; ++i) {
            histLatency_.push_back(24.0f + 2.0f * std::sin(i * 0.2f) + (i % 17 == 0 ? 3.0f : 0.0f));
            histCpu_.push_back(2.1f + 0.4f * std::sin(i * 0.33f));
        }
        meterL_ = -14;
        meterR_ = -19;
        peakL_ = -9;
        peakR_ = -13;

        // Setup assistant: cable installed, 7.1, test passed.
        wasapi::DeviceInfo cable, phones;
        cable.name = "CABLE Input (VB-Audio Virtual Cable)";
        cable.channels = 8;
        cable.sampleRate = 48000;
        cable.channelMask = 0x63F;
        phones.name = "Game (TC-HELICON GoXLR Mini)";
        phones.channels = 2;
        phones.sampleRate = 48000;
        phones.isDefault = true;
        // Dev screenshots of unfinished steps: TACAUDIO_PREVIEW_SETUP=nocable | stereo.
        char demo[16] = {};
        GetEnvironmentVariableA("TACAUDIO_PREVIEW_SETUP", demo, sizeof(demo));
        if (std::string(demo) == "stereo") cable.channels = 2;
        if (std::string(demo) == "nocable") setDeviceList({phones});
        else setDeviceList({cable, phones});
        cfg_.dev.source = cable.name;
        cfg_.dev.output = phones.name;
        if (demo[0]) {
            setupMode_ = 1;  // the cable-step demos
            return;
        }
        CableTestResult t;
        t.pass = true;
        t.message = "All 8 channels arrive separately and unchanged.";
        for (const char* n : {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"}) t.channels.push_back({n, n, 0.0, -96.0, true});
        testResult_ = t;

        // Direct mode (the main mode): engine installed, a game sending 7.1.
        direct_ = true;
        apoctl::Status st;
        st.streams = 2;
        st.channels = 8;
        st.inDb = {-9.0f, -12.0f, -18.0f, -26.0f, -14.0f, -22.0f, -11.0f, -19.0f};
        st.outLDb = -14.0f;
        st.outRDb = -19.0f;
        st.dynMinDb = -11.8f;
        st.dynMaxDb = 5.6f;
        apoStatus_ = st;
        status_.latencyMs = 4.0;
        status_.source = "Game audio: 7.1 surround";
        status_.output = phones.name + " (Direct mode)";

        // Profiles page: a sample library (no covers) with Call of Duty active.
        for (const char* n : {"Apex Legends", "Battlefield 6", "Call of Duty", "Counter-Strike 2", "Escape from Tarkov",
                              "Fortnite", "Hunt: Showdown 1896", "PUBG: Battlegrounds", "Rainbow Six Siege", "The Finals"}) {
            GameEntry g;
            g.name = n;
            g.source = std::string(n) == "Fortnite" ? "Epic Games" : "Steam";
            g.dirLower = std::string("c:\\games\\") + n + "\\";
            library_.push_back(g);
        }
        libraryScanned_ = true;
        selectedGame_ = library_[2].dirLower;
        profiles_ = defaultProfiles(bundleOf(cfg_));
        activeProfile_ = 1;
        {  // Settings page: the footstep ring preview, an enemy walking behind-left
            const SpeakerLayout lay = layoutFromChannelMask(0x63F);
            float pk[8], az[8];
            bool lf[8];
            const float stepDemo[8] = {0, 0, 0, 0, 0.9f, 0, 0.45f, 0};
            for (int c = 0; c < 8; ++c) {
                pk[c] = stepDemo[c];
                az[c] = lay.speakers[static_cast<std::size_t>(c)].azimuthDeg;
                lf[c] = lay.speakers[static_cast<std::size_t>(c)].isLfe;
            }
            for (int i = 0; i < 20; ++i) dirField_.updateWeights(pk, az, lf, 8, 0.1f);
            wchar_t ov[MAX_PATH] = {};  // dev: TACAUDIO_PREVIEW_OVERLAY=<file.bmp> saves the ring bitmap
            if (GetEnvironmentVariableW(L"TACAUDIO_PREVIEW_OVERLAY", ov, MAX_PATH)) overlay_.saveBitmap(dirField_, 140, 0.8f, ov);
        }
        cfg_.eqOn = true;  // sample correction curve for the Devices page
        cfg_.eqName = "Example headphones";
        cfg_.eqText = "-5.8|LSC:105:5.5:0.71|PK:180:-2.1:0.9|PK:2400:3.2:2.2|PK:4300:-3.5:3.5|PK:6200:2.4:4|HSC:10000:-1.5:0.71";
        applyBundle(cfg_, profiles_[1].bundle);
    }
}

App::~App() {
    if (job_.valid()) job_.wait();
    session_.stop();
    if (!preview_) {
        saveSettings(cfg_);
        profiles_[static_cast<std::size_t>(activeProfile_)].bundle = bundleOf(cfg_);
        saveProfiles(profiles_);
    }
    if (gameProcess_) CloseHandle(gameProcess_);
}

bool App::busy() const { return job_.valid() && job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready; }

template <typename F>
void App::launch(const char* label, F fn) {
    jobLabel_ = label;
    job_ = std::async(std::launch::async, [fn]() -> std::string {
        wasapi::ComScope com;
        try {
            fn();
            return {};
        } catch (const std::exception& e) {
            return e.what();
        }
    });
}

void App::refreshDevices() {
    try {
        setDeviceList(wasapi::listDevices(true));
    } catch (const std::exception& e) {
        error_ = e.what();
    }
}

void App::setDeviceList(std::vector<wasapi::DeviceInfo> list) {
    devInfos_ = std::move(list);
    outputs_.clear();
    for (const auto& d : devInfos_) outputs_.push_back(d.name);
    auto resolve = [&](std::string& v) {  // stored short names -> full device names
        for (const auto& o : outputs_)
            if (o == v) return;
        for (const auto& o : outputs_)
            if (o.find(v) != std::string::npos) {
                v = o;
                return;
            }
    };
    resolve(cfg_.dev.source);
    resolve(cfg_.dev.output);
    if (!preview_) {
        const auto* o = outputInfo();
        direct_ = o && apoctl::attachedTo(o->id);
    }
}

const wasapi::DeviceInfo* App::outputInfo() const {
    for (const auto& d : devInfos_)
        if (d.name == cfg_.dev.output) return &d;
    return nullptr;
}

void App::startStop() {
    if (busy()) return;
    error_.clear();
    retries_ = 0;
    if (direct_) {  // the engine lives in Windows audio: just tell it (apo.ini "enabled")
        wantRunning_ = !wantRunning_;
        return;
    }
    if (wantRunning_ || session_.running()) {
        wantRunning_ = false;
        lastJobWasStart_ = false;
        launch("Stopping", [this] { session_.stop(); });
    } else {
        wantRunning_ = true;
        retryAt_ = {};  // start now (supervise() launches it)
    }
}

void App::toggle3d() {
    cfg_.sound.enabled3d = !cfg_.sound.enabled3d;
    session_.set3d(cfg_.sound.enabled3d);
}

void App::autoStartIfEnabled() {
    if (!cfg_.setupDone) {  // first launch: guide the user instead of starting with no cable
        page_ = kPageSetup;
        return;
    }
    apoSync_ = true;  // only a normal launch controls the engine (not --smoke-test / --session-test)
    if (direct_) {    // keep the engine's current state unless "Turn on at launch" is set
        wantRunning_ = cfg_.autoEngine || apoctl::readSetting("enabled", "1") == "1";
        return;
    }
    if (cfg_.autoEngine && !wantRunning_) startStop();
}

std::string App::trayTooltip() const {
    if (!wantRunning_) return "TacAudio - off";
    if (direct_) return std::string("TacAudio - ") + (status_.engine.running ? "playing" : "waiting for game") + ", 3D " +
                        (cfg_.sound.enabled3d ? "on" : "off");
    if (!status_.running) return retries_ > 0 ? "TacAudio - reconnecting..." : "TacAudio - starting...";
    char b[96];
    std::snprintf(b, sizeof(b), "TacAudio - %s, 3D %s, ~%.0f ms", status_.engine.running ? "playing" : "waiting for game",
                  cfg_.sound.enabled3d ? "on" : "off", status_.latencyMs);
    return b;
}

// Keeps the engine running while the user wants it on: (re)starts it, and restarts it after
// a device disappears (GoXLR reset, cable removed, ...), retrying every few seconds.
void App::supervise() {
    if (preview_ || busy()) return;
    if (direct_) {  // no capture session in Direct mode (it would also block the headphones)
        if (session_.running()) launch("Stopping", [this] { session_.stop(); });
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (wantRunning_ && session_.running() && !status_.error.empty()) {
        lastJobWasStart_ = false;
        retryAt_ = now + std::chrono::seconds(2);
        status_ = {};
        launch("Reconnecting", [this] { session_.stop(); });
        return;
    }
    if (wantRunning_ && !session_.running() && now >= retryAt_) {
        const DeviceSettings d = cfg_.dev;
        const SoundSettings s = cfg_.effective();
        lastJobWasStart_ = true;
        retryAt_ = now + std::chrono::seconds(3);
        launch(retries_ > 0 ? "Reconnecting" : "Starting", [this, d, s] { session_.start(d, s); });
        ++retries_;
    }
}

void App::tick() { pollBackground(); }

void App::pollBackground() {
    if (job_.valid() && !busy()) {
        const std::string err = job_.get();
        if (!err.empty()) {
            error_ = err;
            if (lastJobWasStart_ && wantRunning_)
                error_ += "  (retrying every 3 s - turn TacAudio off to stop)";
        } else if (lastJobWasStart_ && session_.running()) {
            error_.clear();
            retries_ = 0;
        }
        lastJobWasStart_ = false;
    }
    pollSetupJobs();
    pollProfiles();
    pollUpdates();
    pollDirect();
    pollOverlay();
    supervise();
    if (wantRebuild_ && !busy() && session_.running()) {
        wantRebuild_ = false;
        const SoundSettings s = cfg_.effective();
        launch("Applying", [this, s] { session_.apply(s); });
    }
    const auto now = std::chrono::steady_clock::now();
    if (!preview_ && !direct_ && now - lastStatus_ > std::chrono::milliseconds(500)) {
        status_ = session_.status();
        if (!status_.error.empty()) error_ = status_.error;
        lastStatus_ = now;
        if (status_.running) {
            histLatency_.push_back(static_cast<float>(status_.latencyMs));
            histCpu_.push_back(static_cast<float>(status_.engine.cpuLoad * 100.0));
            while (histLatency_.size() > 120) histLatency_.pop_front();
            while (histCpu_.size() > 120) histCpu_.pop_front();
        }
    }
    // Meters: fast attack, 30 dB/s fall, 1.2 s peak hold (only while the window is drawn).
    if (!preview_ && ImGui::GetCurrentContext() && ImGui::GetFrameCount() > 0 && ImGui::GetIO().DeltaTime < 0.5f) {
        float l = 0, r = 0;
        if (direct_) {  // ~8 status updates per second from the engine
            if (apoStatus_ && wantRunning_) {
                l = std::pow(10.0f, apoStatus_->outLDb / 20.0f);
                r = std::pow(10.0f, apoStatus_->outRDb / 20.0f);
            }
        } else {
            session_.meters(l, r);
        }
        const float dt = ImGui::GetIO().DeltaTime, t = static_cast<float>(ImGui::GetTime());
        auto upd = [&](float lin, float& level, float& peak, double& hold) {
            const float db = toDb(lin);
            level = std::max(db, level - 30.0f * dt);
            if (db >= peak || t > hold) {
                peak = std::max(db, level);
                hold = t + 1.2;
            }
        };
        upd(l, meterL_, peakL_, peakHoldL_);
        upd(r, meterR_, peakR_, peakHoldR_);
    }
    // Persist (debounced).
    const bool changed = cfg_.dev.source != saved_.dev.source || cfg_.dev.output != saved_.dev.output ||
                         cfg_.dev.exclusive != saved_.dev.exclusive ||
                         SoundSettings::needsRebuild(cfg_.effective(), saved_.effective()) ||
                         cfg_.sound.volumeDb != saved_.sound.volumeDb || cfg_.sound.enabled3d != saved_.sound.enabled3d ||
                         cfg_.dynPreset != saved_.dynPreset || cfg_.stepsLevel != saved_.stepsLevel ||
                         cfg_.autoEngine != saved_.autoEngine || cfg_.closeToTray != saved_.closeToTray ||
                         cfg_.hotkeys != saved_.hotkeys || cfg_.setupDone != saved_.setupDone ||
                         cfg_.autoProfiles != saved_.autoProfiles || cfg_.checkUpdates != saved_.checkUpdates ||
                         cfg_.eqName != saved_.eqName || cfg_.overlayOn != saved_.overlayOn ||
                         cfg_.overlaySize != saved_.overlaySize || cfg_.overlayOpacity != saved_.overlayOpacity ||
                         cfg_.overlayPos != saved_.overlayPos || cfg_.overlayLevel != saved_.overlayLevel;
    if (!preview_ && changed && now - lastSave_ > std::chrono::seconds(1)) {
        saveSettings(cfg_);
        saved_ = cfg_;
        lastSave_ = now;
    }
}

// ------------------------------------------------------------------ Direct mode
std::string App::apoIniText() const {
    char b[512];
    std::snprintf(b, sizeof(b),
                  "enabled=%d\nhead=%s\nroom=%s\npunch=%g\nvolume=%g\n3d=%d\ndynOn=%d\ndynPreset=%s\nstepsOn=%d\n"
                  "stepsLevel=%g\nstepsSmart=%d\nbass=%g\n",
                  wantRunning_ && !headTestBusy() ? 1 : 0, cfg_.sound.head.c_str(), cfg_.sound.room.c_str(), cfg_.sound.punch,
                  cfg_.sound.volumeDb, cfg_.sound.enabled3d ? 1 : 0, cfg_.dynOn ? 1 : 0, cfg_.dynPreset.c_str(),
                  cfg_.stepsOn ? 1 : 0, cfg_.stepsLevel, cfg_.stepsSmart ? 1 : 0, cfg_.sound.bassDb);
    return std::string(b) + "eq=" + cfg_.effective().eq + "\n";
}

// Direct mode: push settings to the engine when they change, and read its status (~8 Hz).
void App::pollDirect() {
    if (apoJob_.valid() && apoJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const int code = apoJob_.get();
        apoMsg_ = code == 0 ? std::string() : code < 0 ? "Cancelled - administrator permission is needed."
                                                       : "Something went wrong - details in %TEMP%\\tacapo_setup.txt";
        refreshDevices();
        engineOutdated_ = direct_ && apoctl::engineOutdated();
        lastApoIni_.clear();  // re-send the settings to a fresh install
        if (code == 0 && direct_ && apoJobLabel_ == "Installing") wantRunning_ = true;
    }
    if (preview_ || !direct_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastApoPoll_ < std::chrono::milliseconds(100)) return;
    lastApoPoll_ = now;
    const std::string ini = apoIniText();
    if (apoSync_ && ini != lastApoIni_ && apoctl::writeSettings(ini)) lastApoIni_ = ini;

    apoStatus_ = apoctl::readStatus();
    status_ = {};
    status_.running = wantRunning_;
    status_.latencyMs = 4.0;  // one 128-frame block + limiter look-ahead
    if (apoStatus_) {
        const auto& in = apoStatus_->inDb;
        bool any = false, surround = false;
        for (unsigned c = 0; c < apoStatus_->channels && c < 8; ++c) {
            any |= in[c] > -80.0f;
            if (c >= 2) surround |= in[c] > -80.0f;
        }
        if (any) audioSeen_ = now;
        if (surround && apoStatus_->channels == 8) surroundSeen_ = now;
        status_.engine.dynMinGainDb = apoStatus_->dynMinDb;
        status_.engine.dynMaxGainDb = apoStatus_->dynMaxDb;
    }
    status_.engine.running = wantRunning_ && now - audioSeen_ < std::chrono::seconds(2);
    status_.source = now - surroundSeen_ < std::chrono::seconds(15) ? "Game audio: 7.1 surround" : "Game audio: stereo";
    status_.output = cfg_.dev.output + " (Direct mode)";
}

void App::runApoSetup(const char* label, std::vector<std::wstring> args) {
    if (apoJob_.valid()) return;
    apoMsg_.clear();
    apoJobLabel_ = label;
    if (session_.running() || wantRunning_) {  // the cable session must not hold the headphones
        wantRunning_ = false;
        session_.stop();
    }
    const auto log = std::filesystem::temp_directory_path() / "tacapo_setup.txt";
    apoJob_ = std::async(std::launch::async, [args, log] { return apoctl::runSetupElevated(args, log); });
}

// ------------------------------------------------------------------ frame / layout
void App::frame() {
    pollBackground();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(16, 18, 25, 255),
                                IM_COL32(14, 16, 22, 255), IM_COL32(11, 12, 17, 255), IM_COL32(13, 15, 21, 255));

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();

    sidebar();
    if (page_ != kPageHeadTest && headTestBusy()) stopHeadTest();  // left the test page
    ImGui::SameLine(0, 0);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(30), S(22)));
    ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (preview_) {  // dev screenshots of long pages: TACAUDIO_PREVIEW_SCROLL=<px>
        char sc[16] = {};
        if (GetEnvironmentVariableA("TACAUDIO_PREVIEW_SCROLL", sc, sizeof(sc))) ImGui::SetScrollY(S(static_cast<float>(atof(sc))));
    }
    switch (page_) {
        case 0: pageSound(); break;
        case 1: pageDevices(); break;
        case 2: pagePerformance(); break;
        case 3: pageSettings(); break;
        case kPageSetup: pageSetup(); break;
        case kPageProfiles: pageProfiles(); break;
        case kPageHeadTest: pageHeadTest(); break;
        default: pageAbout(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();
}

void App::sidebar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Bg1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(22)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(4)));
    ImGui::BeginChild("##sidebar", ImVec2(S(232), 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Logo: a target-like mark of concentric arcs.
    const ImVec2 lp = ImGui::GetCursorScreenPos() + ImVec2(S(8), 0);
    const ImVec2 c(lp.x + S(16), lp.y + S(16));
    for (int i = 0; i < 3; ++i) {
        const float r = S(6.0f + 5.0f * i);
        const float a0 = -std::numbers::pi_v<float> * (0.25f + 0.1f * i);
        dl->PathArcTo(c, r, a0, a0 + std::numbers::pi_v<float> * 1.35f, 24);
        dl->PathStroke(withAlpha(col::Accent, 1.0f - 0.25f * i), ImDrawFlags_None, S(2.6f));
    }
    dl->AddCircleFilled(c, S(3), col::Accent);
    ImGui::SetCursorScreenPos(lp + ImVec2(S(42), S(1)));
    text("TacAudio", col::Text, 21.0f, true);
    ImGui::SetCursorScreenPos(lp + ImVec2(S(43), S(27)));
    textUpper("spatial engine", col::Faint, 10.0f);
    ImGui::SetCursorScreenPos(ImVec2(lp.x - S(8), lp.y + S(66)));
    ImGui::Dummy(ImVec2(0, 0));

    const char* glyphs[] = {icon::Audio, icon::Game, icon::Devices, icon::Setup, icon::Gauge, icon::Settings, icon::Info};
    const char* labels[] = {"Sound", "Profiles", "Devices", "Setup", "Performance", "Settings", "About"};
    const int pages[] = {0, kPageProfiles, 1, kPageSetup, 2, 3, 4};
    for (int i = 0; i < 7; ++i)
        if (navItem(glyphs[i], labels[i], (page_ == kPageHeadTest ? 0 : page_) == pages[i])) page_ = pages[i];

    // Engine status at the bottom of the sidebar.
    const float h = ImGui::GetWindowHeight();
    ImGui::SetCursorPosY(h - S(22) - S(66));
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    const ImVec2 ss(ImGui::GetContentRegionAvail().x, S(66));
    dl->AddRectFilled(sp, sp + ss, col::Card, S(10));
    const bool on = status_.running;
    const ImU32 dot = !on ? col::Faint : status_.engine.running ? col::Green : col::Yellow;
    dl->AddCircleFilled(sp + ImVec2(S(18), S(22)), S(5), dot);
    dl->AddText(fonts.semibold, S(14), sp + ImVec2(S(32), S(13)), col::Text,
                !on ? "Engine off" : status_.engine.running ? "Playing" : "Waiting for game");
    char buf[64];
    if (on) std::snprintf(buf, sizeof(buf), "~%.0f ms  |  %.1f%% cpu", status_.latencyMs, status_.engine.cpuLoad * 100);
    else std::snprintf(buf, sizeof(buf), "Turn on from the top bar");
    dl->AddText(fonts.regular, S(12.5f), sp + ImVec2(S(18), S(38)), col::Dim, buf);
    ImGui::Dummy(ss);

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool App::topBar(const char* title, const char* subtitle, bool showReset) {
    bool resetClicked = false;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    text(title, col::Text, 26.0f, true);
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + S(36)));
    text(subtitle, col::Dim, 13.5f);

    // Right side: status chip + power switch.
    const bool running = (direct_ ? wantRunning_ : session_.running()) || preview_;
    const bool working = busy() && !direct_;
    const float powerW = S(150);
    ImGui::SetCursorScreenPos(ImVec2(start.x + avail - powerW, start.y + S(6)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pp = ImGui::GetCursorScreenPos();
    const ImVec2 ps(powerW, S(42));
    dl->AddRectFilled(pp, pp + ps, col::Card, ps.y * 0.5f);
    if (running) dl->AddRect(pp, pp + ps, withAlpha(col::Accent, 0.55f), ps.y * 0.5f, 0, S(1.2f));
    dl->AddText(fonts.regular, S(17), pp + ImVec2(S(16), S(11)), running ? col::Accent : col::Dim, icon::Power);
    dl->AddText(fonts.semibold, S(14.5f), pp + ImVec2(S(42), S(11)), col::Text, running ? "ON" : "OFF");
    ImGui::SetCursorScreenPos(pp + ImVec2(ps.x - S(56), S(10)));
    bool on = wantRunning_ || preview_;
    ImGui::BeginDisabled(working);
    if (toggle("##power", &on) && !preview_) startStop();
    ImGui::EndDisabled();

    char chipText[96];
    ImU32 dot = col::Faint;
    if (working) {
        std::snprintf(chipText, sizeof(chipText), "%s...", jobLabel_.c_str());
        dot = col::Yellow;
    } else if (!running) {
        std::snprintf(chipText, sizeof(chipText), "Off");
    } else if (!status_.engine.running) {
        std::snprintf(chipText, sizeof(chipText), direct_ ? "Direct  -  waiting for game audio" : "Waiting for game audio");
        dot = col::Yellow;
    } else if (direct_) {
        const bool surround = status_.source.find("7.1") != std::string::npos;
        std::snprintf(chipText, sizeof(chipText), "Direct  -  playing %s", surround ? "7.1" : "stereo");
        dot = col::Green;
    } else {
        std::snprintf(chipText, sizeof(chipText), "Playing  ~%.0f ms", status_.latencyMs);
        dot = col::Green;
    }
    const float chipW = ImGui::CalcTextSize(chipText).x + S(44);
    ImGui::SetCursorScreenPos(ImVec2(pp.x - S(12) - chipW, start.y + S(13)));
    chip(chipText, dot);
    if (showReset) {
        ImGui::SetCursorScreenPos(ImVec2(pp.x - S(24) - chipW - S(112), start.y + S(10)));
        resetClicked = pillButton("Reset all", col::Card, col::Dim, S(112));
        if (ImGui::IsItemHovered()) tooltip("Restore every sound setting to its default (devices are kept)");
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + S(70)));
    if (!error_.empty()) {
        const ImVec2 ep = ImGui::GetCursorScreenPos();
        const float h = S(38);
        dl->AddRectFilled(ep, ep + ImVec2(avail, h), withAlpha(col::Red, 0.14f), S(8));
        dl->AddText(fonts.semibold, S(13.5f), ep + ImVec2(S(14), S(10)), col::Red, error_.c_str());
        ImGui::SetCursorScreenPos(ep + ImVec2(avail - S(34), S(5)));
        if (iconButton("##dismiss", "\xEE\x9C\x91", "Dismiss", true, 28)) error_.clear();  // U+E711 close
        ImGui::SetCursorScreenPos(ImVec2(ep.x, ep.y + h + S(12)));
    }
    if (engineOutdated_ && direct_ && !preview_) {  // app updated, engine in Windows audio not yet
        const ImVec2 up = ImGui::GetCursorScreenPos();
        const float h = S(42);
        dl->AddRectFilled(up, up + ImVec2(avail, h), withAlpha(col::Teal, 0.12f), S(8));
        dl->AddText(fonts.semibold, S(13.5f), up + ImVec2(S(14), S(12)), col::Text,
                    apoJob_.valid() ? "Updating the engine... (approve the Windows prompt; sound pauses for a few seconds)"
                                    : "TacAudio was updated - update its engine in Windows audio to get the new sound features.");
        ImGui::SetCursorScreenPos(up + ImVec2(avail - S(170), S(4)));
        if (actionButton("Update engine", true, !apoJob_.valid(), S(160)))
            runApoSetup("Installing", {L"install", L"--device", fromUtf8(cfg_.dev.output)});
        ImGui::SetCursorScreenPos(ImVec2(up.x, up.y + h + S(12)));
    }
    if (update_ && !updDismissed_) {  // a newer release is out
        const ImVec2 up = ImGui::GetCursorScreenPos();
        const float h = S(42);
        dl->AddRectFilled(up, up + ImVec2(avail, h), withAlpha(col::Accent, 0.14f), S(8));
        dl->AddText(fonts.regular, S(15), up + ImVec2(S(14), S(12)), col::Accent, icon::Refresh);
        char b[160];
        std::snprintf(b, sizeof(b), "TacAudio %s is available - you have %s. Your settings and profiles are kept.",
                      update_->version.c_str(), TAC_VERSION);
        dl->AddText(fonts.semibold, S(13.5f), up + ImVec2(S(40), S(12)), col::Text, b);
        ImGui::SetCursorScreenPos(up + ImVec2(avail - S(34) - S(150), S(4)));
        if (pillButton("Download", col::Accent, IM_COL32(20, 14, 8, 255), S(140)))
            ShellExecuteW(nullptr, L"open", fromUtf8(update_->url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SetCursorScreenPos(up + ImVec2(avail - S(34), S(7)));
        if (iconButton("##updclose", "\xEE\x9C\x91", "Remind me later", true, 28)) updDismissed_ = true;  // U+E711
        ImGui::SetCursorScreenPos(ImVec2(up.x, up.y + h + S(12)));
    }
    ImGui::Dummy(ImVec2(0, 0));
    return resetClicked;
}

// ------------------------------------------------------------------ Sound page
void App::pageSound() {
    const AppSettings def;
    char sub[200];
    std::snprintf(sub, sizeof(sub), "Profile: %s%s  -  every change applies live and is saved to this profile.",
                  profiles_[static_cast<std::size_t>(activeProfile_)].name.c_str(), gameProcess_ ? " (game running)" : "");
    if (topBar("Sound", sub, true)) {
        cfg_.sound.head = def.sound.head;
        cfg_.sound.room = def.sound.room;
        cfg_.sound.punch = def.sound.punch;
        cfg_.sound.volumeDb = def.sound.volumeDb;
        cfg_.sound.bassDb = def.sound.bassDb;
        cfg_.sound.enabled3d = def.sound.enabled3d;
        cfg_.dynOn = def.dynOn;
        cfg_.dynPreset = def.dynPreset;
        cfg_.stepsOn = def.stepsOn;
        cfg_.stepsLevel = def.stepsLevel;
        cfg_.stepsSmart = def.stepsSmart;
        session_.setVolumeDb(cfg_.sound.volumeDb);
        session_.set3d(cfg_.sound.enabled3d);
        requestRebuild();
    }
    const float gap = S(16);
    // --- Spatial audio
    beginCard("##spatial", "Spatial audio", &cfg_.sound.enabled3d, 0,
              "3D positioning through a measured head (HRTF). Turn off for a plain stereo mix to compare.");
    if (cardToggleChanged()) session_.set3d(cfg_.sound.enabled3d);
    if (comboRow("Head", &cfg_.sound.head, headNames_, &headDescs_, def.sound.head,
                 "Different heads fit different listeners. Pick the one with the clearest front/back."))
        requestRebuild();
    int room = indexOf(kRooms, cfg_.sound.room, 1);
    if (segmentedRow("Room", &room, kRoomLabels, indexOf(kRooms, def.sound.room, 1),
                     "Neutral: dry and precise. Balanced: small room, like DTS Balanced. Spacious: bigger room.")) {
        cfg_.sound.room = kRooms[static_cast<std::size_t>(room)];
        requestRebuild();
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(104));
    if (pillButton("Find your head", col::CardHi, col::Text, S(160))) page_ = kPageHeadTest;
    if (ImGui::IsItemHovered()) tooltip("A 2-minute listening test that picks the head that suits your ears best");
    endCard();
    ImGui::Dummy(ImVec2(0, gap - S(10)));

    // --- Two columns: dynamics | footsteps
    const float colW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    beginCard("##dyn", "Combat dynamics", &cfg_.dynOn, colW,
              "Tames loud sounds (gunfire, explosions, killstreaks) and lifts quiet ones (footsteps). "
              "Each channel on its own, before 3D.");
    if (cardToggleChanged()) requestRebuild();
    {
        DynamicsParams p = dynamicsPreset(cfg_.dynPreset);
        p.punch = cfg_.sound.punch;
        const ChannelDynamics d(48000, 1, p);
        GraphAxis ax{-70.0f, 6.0f, {{10.0f / 76.0f, "-60"}, {30.0f / 76.0f, "-40"}, {50.0f / 76.0f, "-20"}, {70.0f / 76.0f, "0"}},
                     {-60.0f, -40.0f, -20.0f, 0.0f}};
        auto inDb = [](float x) { return -70.0f + x * 76.0f; };
        const std::function<float(float)> ref = [inDb](float x) { return inDb(x); };
        graph("##dyncurve", ImVec2(ImGui::GetContentRegionAvail().x, S(130)), ax,
              [&](float x) { const float i = inDb(x); return i + d.curveDb(i) + p.makeupDb; }, col::Accent, cfg_.dynOn,
              -1.0f, &ref, -70.0f);
        ui::text("input level  ->  output level", col::Faint, 11.5f);
    }
    int preset = indexOf(kPresets, cfg_.dynPreset, 1);
    if (segmentedRow("Preset", &preset, kPresetLabels, indexOf(kPresets, def.dynPreset, 1),
                     "Light: gentle. Medium: recommended. Strong: maximum footstep focus.")) {
        cfg_.dynPreset = kPresets[static_cast<std::size_t>(preset)];
        requestRebuild();
    }
    if (sliderRow("Punch", &cfg_.sound.punch, 0.0f, 100.0f, "%.0f", def.sound.punch,
                  "How much of each gunshot's impact passes before taming starts. Higher = punchier guns.") == Edit::Commit)
        requestRebuild();
    endCard();

    ImGui::SameLine(0, gap);
    beginCard("##steps", "Footstep clarity", &cfg_.stepsOn, colW,
              "Lifts the footstep 'crunch' band around 3.5 kHz, only while a channel is quiet - "
              "gun cracks in the same band are not boosted. Smart: only short impacts (steps, landings, reloads), "
              "not rain, wind or engines.");
    if (cardToggleChanged()) requestRebuild();
    {
        const double fs = 48000.0;
        const Biquad bp = Biquad::bandpass(fs, 3500.0, 0.8);
        const float g = std::pow(10.0f, cfg_.stepsLevel / 20.0f) - 1.0f;
        auto freq = [](float x) { return 20.0 * std::pow(1000.0, static_cast<double>(x)); };  // 20 Hz .. 20 kHz
        auto xOf = [](double f) { return static_cast<float>(std::log10(f / 20.0) / 3.0); };
        auto respDb = [&](float x) {
            const double w = 2.0 * std::numbers::pi * freq(x) / fs;
            const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
            const std::complex<double> h =
                (static_cast<double>(bp.b0) + static_cast<double>(bp.b1) * z1 + static_cast<double>(bp.b2) * z2) /
                (1.0 + static_cast<double>(bp.a1) * z1 + static_cast<double>(bp.a2) * z2);
            return static_cast<float>(20.0 * std::log10(std::abs(1.0 + static_cast<double>(g) * h)));
        };
        GraphAxis ax{-3.0f, 13.0f,
                     {{xOf(50), "50"}, {xOf(100), "100"}, {xOf(200), "200"}, {xOf(500), "500"}, {xOf(1000), "1k"},
                      {xOf(2000), "2k"}, {xOf(5000), "5k"}, {xOf(10000), "10k"}},
                     {0.0f, 6.0f, 12.0f}};
        graph("##stepcurve", ImVec2(ImGui::GetContentRegionAvail().x, S(130)), ax, respDb, col::Teal, cfg_.stepsOn,
              xOf(3500.0));
        ui::text("frequency (Hz)  -  boost applies to quiet sounds only", col::Faint, 11.5f);
    }
    if (sliderRow("Clarity", &cfg_.stepsLevel, 0.0f, 12.0f, "+%.1f dB", def.stepsLevel,
                  "Amount of footstep clarity. +3 dB is subtle, +6 dB is clear, +9 dB and more is very strong.",
                  col::Teal) == Edit::Commit)
        requestRebuild();
    if (toggleRow("Smart", &cfg_.stepsSmart, "Impacts only", def.stepsSmart)) requestRebuild();
    if (ImGui::IsItemHovered())
        tooltip("Boost only short impacts in the footstep band. Steady sounds there - rain, wind, engines - are not lifted.");
    endCard();
    ImGui::Dummy(ImVec2(0, gap - S(10)));

    // --- Output
    beginCard("##out", "Output", nullptr, 0, "Overall level after all processing. A peak limiter prevents clipping.");
    const Edit ve = sliderRow("Volume", &cfg_.sound.volumeDb, -30.0f, 6.0f, "%.1f dB", def.sound.volumeDb,
                              "Output volume. The GoXLR fader still works on top of this.");
    if (ve != Edit::None) session_.setVolumeDb(cfg_.sound.volumeDb);
    if (sliderRow("Bass", &cfg_.sound.bassDb, -12.0f, 6.0f, "%+.1f dB", def.sound.bassDb,
                  "Level of the game's subwoofer (LFE) channel. 0 dB = as the game mixed it, no bass boost. "
                  "Lower it if explosions mask footsteps.",
                  col::Accent) == Edit::Commit)
        requestRebuild();
    {
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(104));
        const float mw = (w - S(104) - S(110) - S(20)) * 0.5f;
        meter("L", meterL_, peakL_, mw);
        ImGui::SameLine(0, S(20));
        meter("R", meterR_, peakR_, mw);
    }
    endCard();
}

// ------------------------------------------------------------------ Devices page
void App::pageDevices() {
    topBar("Devices", "Where game audio comes from, and where TacAudio plays it.");
    if (direct_) {
        beginCard("##direct", "Direct mode", nullptr, 0,
                  "TacAudio's engine runs inside Windows audio on your headphones - no virtual cable, ~4 ms delay.");
        char b[256];
        std::snprintf(b, sizeof(b), "Engine installed on: %s", cfg_.dev.output.c_str());
        chip(b, col::Green);
        ui::text("Games: set their output to this device (or leave it as the Windows default) and choose "
                 "7.1 / Surround / Home theater in the game.",
                 col::Dim, 13.0f);
        ui::text(status_.source.c_str(), status_.source.find("7.1") != std::string::npos ? col::Teal : col::Faint, 13.0f, true);
        endCard();
        ImGui::Dummy(ImVec2(0, S(6)));
        eqCard();
        ImGui::Dummy(ImVec2(0, S(6)));
        if (pillButton("Setup assistant", col::Card, col::Text, S(170))) {
            setupMode_ = 0;
            page_ = kPageSetup;
        }
        return;
    }
    const bool locked = wantRunning_ || session_.running() || busy();
    if (locked) {
        ui::text("Turn TacAudio off to change devices.", col::Yellow, 13.5f, true);
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    ImGui::BeginDisabled(locked);
    auto find = [&](const char* part) {
        for (const auto& o : outputs_)
            if (o.find(part) != std::string::npos) return o;
        return std::string();
    };

    beginCard("##src", "Game audio source", nullptr, 0,
              "The virtual 7.1 device your game plays into. TacAudio captures it (loopback).");
    comboRow("Device", &cfg_.dev.source, outputs_, nullptr, find("CABLE Input"));
    ui::text("In Windows: Settings > System > Sound > Volume mixer, set the game's output to this device. "
             "It must be configured as 7.1 Surround.",
             col::Faint, 12.5f);
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    beginCard("##dst", "Headphone output", nullptr, 0, "Where you listen. TacAudio sends its 3D stereo mix here.");
    comboRow("Device", &cfg_.dev.output, outputs_, nullptr, find("Game ("));
    {
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(fonts.regular, S(14.5f), row + ImVec2(0, S(6)), col::Dim, "Exclusive");
        ImGui::SetCursorScreenPos(row + ImVec2(S(104), S(5)));
        toggle("##excl", &cfg_.dev.exclusive);
        ImGui::SameLine(0, S(14));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(2));
        ui::text(cfg_.dev.exclusive ? "Lowest latency (~25 ms). Other apps can't play on this device meanwhile."
                                    : "Shared (~55 ms). Other apps can play on this device too.",
                 col::Faint, 12.5f);
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(36)));
        ImGui::Dummy(ImVec2(0, 0));
    }
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::EndDisabled();
    eqCard();
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::BeginDisabled(locked);
    if (pillButton("Refresh devices", col::Card, col::Text, S(170))) refreshDevices();
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(10));
    if (pillButton("Setup assistant", col::Card, col::Text, S(170))) page_ = kPageSetup;
    if (ImGui::IsItemHovered()) tooltip("Step-by-step check of VB-CABLE, 7.1 and your devices");
}

void App::importEq(const std::string& text, const std::string& name) {
    try {
        const HeadphoneEq eq = HeadphoneEq::parseText(text);
        cfg_.eqText = eq.toCompact();
        cfg_.eqName = name;
        cfg_.eqOn = true;
        eqError_.clear();
        requestRebuild();
    } catch (const std::exception& e) {
        eqError_ = e.what();
    }
}

void App::eqCard() {
    beginCard("##eq", "Headphone EQ", &cfg_.eqOn, 0,
              "Corrects your headphones' own sound so the 3D picture is more accurate. Import a parametric EQ made for "
              "your headphone model (Equalizer APO / AutoEq format). The same for every game profile.");
    if (cardToggleChanged()) requestRebuild();
    const HeadphoneEq eq = HeadphoneEq::fromCompact(cfg_.eqText);
    {
        auto freq = [](float x) { return 20.0 * std::pow(1000.0, static_cast<double>(x)); };  // 20 Hz .. 20 kHz
        auto xOf = [](double f) { return static_cast<float>(std::log10(f / 20.0) / 3.0); };
        const std::vector<Biquad> filters = eq.makeFilters(48000.0);
        auto resp = [&](float x) {
            float db = eq.preampDb;
            for (const auto& f : filters) db += biquadMagnitudeDb(f, freq(x), 48000.0);
            return std::clamp(db, -15.0f, 15.0f);
        };
        GraphAxis ax{-15.0f, 15.0f,
                     {{xOf(50), "50"}, {xOf(100), "100"}, {xOf(200), "200"}, {xOf(500), "500"}, {xOf(1000), "1k"},
                      {xOf(2000), "2k"}, {xOf(5000), "5k"}, {xOf(10000), "10k"}},
                     {-12.0f, -6.0f, 0.0f, 6.0f, 12.0f}};
        graph("##eqcurve", ImVec2(ImGui::GetContentRegionAvail().x, S(120)), ax, resp, col::Teal, cfg_.eqOn && !eq.empty());
    }
    char b[256];
    if (eq.empty()) std::snprintf(b, sizeof(b), "No EQ loaded yet.");
    else std::snprintf(b, sizeof(b), "%s  -  %zu filters, preamp %.1f dB", cfg_.eqName.empty() ? "Custom EQ" : cfg_.eqName.c_str(),
                       eq.bands.size(), eq.preampDb);
    ui::text(b, eq.empty() ? col::Faint : col::Text, 13.0f, !eq.empty());
    ImGui::Dummy(ImVec2(0, S(2)));
    if (actionButton("Import file...", true, true, S(150)) && !preview_) {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetActiveWindow();
        ofn.lpstrFilter = L"Parametric EQ (*.txt)\0*.txt\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Choose a parametric EQ file (Equalizer APO / AutoEq ParametricEQ.txt)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            std::ifstream f(fs::path(file), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string name = toUtf8(fs::path(file).stem().wstring());
            if (const auto p = name.find(" ParametricEQ"); p != std::string::npos) name.erase(p);
            importEq(ss.str(), name);
        }
    }
    ImGui::SameLine(0, S(10));
    if (actionButton("Paste", false, true, S(100)) && !preview_) {
        const char* clip = ImGui::GetClipboardText();
        importEq(clip ? clip : "", "Pasted EQ");
    }
    if (ImGui::IsItemHovered()) tooltip("Paste parametric EQ text copied from a website (Preamp / Filter lines)");
    ImGui::SameLine(0, S(10));
    if (actionButton("Clear", false, !eq.empty(), S(100)) && !preview_) {
        cfg_.eqText.clear();
        cfg_.eqName.clear();
        cfg_.eqOn = false;
        requestRebuild();
    }
    ImGui::SameLine(0, S(10));
    if (actionButton("Find your headphones", false, true, S(200))) openShellUrl(L"https://autoeq.app");
    if (ImGui::IsItemHovered())
        tooltip("AutoEq: free EQ settings for thousands of headphones. Choose your model, the 'Equalizer APO' "
                "parametric format, download, then Import file here.");
    if (!eqError_.empty()) para(eqError_.c_str(), col::Red, 12.5f);
    endCard();
}

// ------------------------------------------------------------------ Performance page
void App::pagePerformance() {
    topBar("Performance", "Live engine health. Glitch counters should stay at 0 after start-up.");
    const auto& e = status_.engine;
    const bool on = status_.running;
    const float gap = S(16);
    const float w = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3.0f;
    const ImVec2 ts(w, S(108));
    char v[48], s[64];
    auto tile = [&](const char* label, const char* value, const char* sub, ImU32 accent, bool last) {
        statTile(label, on ? value : "--", sub, on ? accent : col::Faint, ts);
        if (!last) ImGui::SameLine(0, gap);
    };
    if (direct_) {
        const bool surround = status_.source.find("7.1") != std::string::npos;
        tile("Added latency", "~4 ms", "inside Windows audio, no cable", col::Teal, false);
        tile("Game audio", status_.engine.running ? (surround ? "7.1" : "stereo") : "idle",
             surround ? "surround channels are arriving" : "set the game to 7.1 / Home theater", col::Accent, false);
        std::snprintf(v, sizeof(v), "%d", apoStatus_ ? apoStatus_->streams : 0);
        tile("Streams", v, "game / app streams processed", col::Green, true);
        ImGui::Dummy(ImVec2(0, gap - S(10)));
        std::snprintf(v, sizeof(v), "%.1f dB", e.dynMinGainDb);
        tile("Loud sounds tamed", v, "strongest reduction, last 0.1 s", col::Accent, false);
        std::snprintf(v, sizeof(v), "%+.1f dB", e.dynMaxGainDb);
        tile("Quiet sounds lifted", v, "strongest lift, last 0.1 s", col::Teal, false);
        tile("Mode", "Direct", "TacAudio engine in Windows audio", col::Green, true);
        ImGui::Dummy(ImVec2(0, gap - S(10)));
        beginCard("##chan", "Game channels (input level)", nullptr, 0,
                  "What the game sends, per speaker. Back and side speakers moving = the game outputs 7.1.");
        const char* names[] = {"FL", "FR", "FC", "SW", "BL", "BR", "SL", "SR"};  // SW = subwoofer (LFE)
        const float mw = (ImGui::GetContentRegionAvail().x - S(20)) * 0.5f;
        for (int c = 0; c < 8; ++c) {
            const float db = apoStatus_ && on ? apoStatus_->inDb[static_cast<std::size_t>(c)] : -120.0f;
            meter(names[c], std::max(db, -60.0f), std::max(db, -60.0f), mw);
            if (c % 2 == 0) ImGui::SameLine(0, S(20));
        }
        endCard();
        return;
    }
    std::snprintf(v, sizeof(v), "%.0f ms", status_.latencyMs);
    tile("Added latency", v, "capture + buffer + processing + output", col::Teal, false);
    std::snprintf(v, sizeof(v), "%.1f ms", e.bufferMs);
    std::snprintf(s, sizeof(s), "target %.1f ms (adaptive)", e.targetMs);
    tile("Input buffer", v, s, col::Accent, false);
    std::snprintf(v, sizeof(v), "%.1f %%", e.cpuLoad * 100);
    tile("CPU", v, "one core, audio thread", col::Green, true);
    ImGui::Dummy(ImVec2(0, gap - S(10)));
    std::snprintf(v, sizeof(v), "%llu / %llu", static_cast<unsigned long long>(e.underruns),
                  static_cast<unsigned long long>(status_.outLate));
    tile("Glitches", v, "input underruns / output late", (e.underruns > 3 || status_.outLate) ? col::Red : col::Green, false);
    std::snprintf(v, sizeof(v), "%.1f dB", e.dynMinGainDb);
    tile("Loud sounds tamed", v, "strongest reduction, last 0.5 s", col::Accent, false);
    std::snprintf(v, sizeof(v), "%+.1f dB", e.dynMaxGainDb);
    tile("Quiet sounds lifted", v, "strongest lift, last 0.5 s", col::Teal, true);
    ImGui::Dummy(ImVec2(0, gap - S(10)));

    beginCard("##hist", "History (last minute)", nullptr, 0);
    auto spark = [&](const char* label, const std::deque<float>& h, float maxV, ImU32 c, const char* unit) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(ImGui::GetContentRegionAvail().x, S(70));
        dl->AddRectFilled(p, p + sz, col::Well, S(8));
        dl->AddText(fonts.semibold, S(12), p + ImVec2(S(12), S(8)), col::Dim, label);
        if (!h.empty()) {
            char b[32];
            std::snprintf(b, sizeof(b), "%.1f %s", h.back(), unit);
            const ImVec2 bs = fonts.semibold->CalcTextSizeA(S(13), FLT_MAX, 0, b);
            dl->AddText(fonts.semibold, S(13), p + ImVec2(sz.x - bs.x - S(12), S(7)), c, b);
            std::vector<ImVec2> pts;
            for (std::size_t i = 0; i < h.size(); ++i) {
                const float x = p.x + S(12) + (sz.x - S(24)) * (static_cast<float>(i) / 119.0f);
                const float y = p.y + sz.y - S(8) - (sz.y - S(32)) * std::clamp(h[i] / maxV, 0.0f, 1.0f);
                pts.push_back(ImVec2(x, y));
            }
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), withAlpha(c, 0.2f), ImDrawFlags_None, S(5));
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), c, ImDrawFlags_None, S(1.8f));
        }
        ImGui::Dummy(sz);
    };
    spark("ADDED LATENCY", histLatency_, 80.0f, col::Teal, "ms");
    spark("CPU", histCpu_, 10.0f, col::Green, "%");
    endCard();
    if (on) {
        ImGui::Dummy(ImVec2(0, S(4)));
        char path[256];
        std::snprintf(path, sizeof(path), "%s  ->  %s", status_.source.c_str(), status_.output.c_str());
        ui::text(path, col::Faint, 12.5f);
    }
}

// ------------------------------------------------------------------ Settings page
void App::pageSettings() {
    topBar("Settings", "How TacAudio starts and behaves in the background.");
    const AppSettings def;

    beginCard("##startup", "Startup", nullptr, 0);
    if (toggleRow("Start with Windows", &cfg_.startWithWindows, "Launches TacAudio in the tray when you sign in.",
                  def.startWithWindows))
        setAutostart(cfg_.startWithWindows);
    toggleRow("Turn on at launch", &cfg_.autoEngine, "Starts the 3D engine as soon as TacAudio opens.", def.autoEngine);
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    beginCard("##window", "Window & tray", nullptr, 0);
    toggleRow("Close to tray", &cfg_.closeToTray,
              "The window's X button keeps TacAudio running in the notification area. Right-click the tray icon to exit.",
              def.closeToTray);
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    beginCard("##hotkeys", "Hotkeys", nullptr, 0, "Work everywhere, also while a game is in focus.");
    toggleRow("Enable hotkeys", &cfg_.hotkeys, "Global keyboard shortcuts (see below).", def.hotkeys);
    ImGui::BeginDisabled(!cfg_.hotkeys);
    auto keyRow = [&](const char* keys, const char* what) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ks = fonts.semibold->CalcTextSizeA(S(13), FLT_MAX, 0, keys);
        dl->AddRectFilled(p + ImVec2(S(104), 0), p + ImVec2(S(104) + ks.x + S(24), S(28)), col::Well, S(6));
        dl->AddText(fonts.semibold, S(13), p + ImVec2(S(116), S(6)), cfg_.hotkeys ? col::Text : col::Faint, keys);
        dl->AddText(fonts.regular, S(13.5f), p + ImVec2(S(104) + ks.x + S(40), S(6)), col::Dim, what);
        ImGui::Dummy(ImVec2(0, S(32)));
    };
    keyRow(kHotkeyPower, "Turn TacAudio on / off");
    keyRow(kHotkey3d, "3D sound on / off (instant A/B against plain stereo)");
    ImGui::EndDisabled();
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    beginCard("##updates", "Updates", nullptr, 0);
    toggleRow("Check for updates", &cfg_.checkUpdates,
              "Asks GitHub for a newer TacAudio when it starts and twice a day. Nothing is installed by itself.",
              def.checkUpdates);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(104));
    if (pillButton(updJob_.valid() ? "Checking..." : "Check now", col::CardHi, col::Text, S(130)) && !updJob_.valid())
        checkUpdatesNow();
    ImGui::SameLine(0, S(14));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(8));
    ui::text(updStatus_.empty() ? "Version " TAC_VERSION : updStatus_.c_str(), update_ ? col::Accent : col::Dim, 13.0f);
    if (update_) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(104));
        if (pillButton("Open download page", col::Accent, IM_COL32(20, 14, 8, 255), S(190)))
            ShellExecuteW(nullptr, L"open", fromUtf8(update_->url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    beginCard("##access", "Accessibility", nullptr, 0,
              "For players who can't hear well: shows where enemy footsteps come from. A detector picks out short, "
              "crunchy impacts at footstep level; your own sounds (straight ahead) are ignored. Detection sets how sure it "
              "must be: Sensitive shows every such impact, Balanced needs a second step from the same direction, Strict "
              "needs a steady walking rhythm (ignores reloads and most gunfire, but reacts later). "
              "It only reads the audio TacAudio already processes - nothing inside the game.");
    const ImVec2 cardTop = ImGui::GetCursorScreenPos();
    const float previewW = S(150);
    const float rowW = ImGui::GetContentRegionAvail().x - previewW - S(20);
    ImGui::BeginGroup();
    ImGui::PushItemWidth(rowW);
    ImGui::BeginChild("##accrows", ImVec2(rowW, S(285)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    toggleRow("Footstep ring", &cfg_.overlayOn, "Glows towards enemy footsteps. Your own sounds are ignored.", def.overlayOn);
    ImGui::BeginDisabled(!cfg_.overlayOn);
    {
        static const std::vector<std::string> kPos = {"Top left", "Top right", "Bottom left", "Bottom right", "Center"};
        segmentedRow("Position", &cfg_.overlayPos, kPos, def.overlayPos, "Where the ring sits on your main screen.");
        static const std::vector<std::string> kLvl = {"Sensitive", "Balanced", "Strict"};
        segmentedRow("Detection", &cfg_.overlayLevel, kLvl, def.overlayLevel,
                     "Sensitive: every step-like sound. Balanced: from the 2nd step. Strict: steady walking only.");
    }
    sliderRow("Size", &cfg_.overlaySize, 40.0f, 300.0f, "%.0f px", def.overlaySize, "Radius of the ring.");
    sliderRow("Opacity", &cfg_.overlayOpacity, 10.0f, 100.0f, "%.0f %%", def.overlayOpacity, "How visible the ring is.");
    ImGui::EndDisabled();
    {  // what the detector heard (helps to pick the level, and to report problems)
        const long long shown = cfg_.overlayLevel == 0   ? stepCount_[0] - stepCount_[1]
                                : cfg_.overlayLevel == 1 ? stepCount_[3]
                                                         : stepCount_[4];
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "Heard so far: %lld step-like sounds - %lld your own (ignored), %lld shown at Sensitive, %lld at "
                      "Balanced, %lld at Strict. Shown now: %lld.",
                      stepCount_[0], stepCount_[1], stepCount_[0] - stepCount_[1], stepCount_[3], stepCount_[4], shown);
        para(buf, col::Dim, 12.0f);
        if (ImGui::SmallButton("Reset counters")) stepCount_ = {};
    }
    para("Works over windowed and borderless-fullscreen games (not exclusive fullscreen). Some games and tournaments "
         "don't allow sound-visualisation overlays - check the rules before using it in ranked or competitive play.",
         col::Yellow, 12.0f);
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    {  // live preview of the ring
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = cardTop + ImVec2(rowW + S(20) + previewW * 0.5f, S(80));
        const float R = S(56);
        dl->AddCircleFilled(c, R + S(16), col::Well, 48);
        for (int d = 0; d < 360; d += 3) {
            const float a0 = (static_cast<float>(d) - 90.0f) * std::numbers::pi_v<float> / 180.0f;
            const float a1 = (static_cast<float>(d + 3) - 90.0f) * std::numbers::pi_v<float> / 180.0f;
            const float v = dirField_.at(d);
            dl->PathArcTo(c, R, a0, a1, 2);
            dl->PathStroke(v > 0.02f ? withAlpha(col::Accent, std::min(1.0f, 0.25f + v)) : withAlpha(col::Line, 0.9f),
                           ImDrawFlags_None, S(3.0f + 6.0f * v));
        }
        dl->AddTriangleFilled(c + ImVec2(-S(5), -R - S(10)), c + ImVec2(S(5), -R - S(10)), c + ImVec2(0, -R - S(3)), col::Dim);
        const char* lbl = "live preview";
        const ImVec2 ls = fonts.regular->CalcTextSizeA(S(11.5f), FLT_MAX, 0, lbl);
        dl->AddText(fonts.regular, S(11.5f), ImVec2(c.x - ls.x * 0.5f, c.y + R + S(22)), col::Faint, lbl);
    }
    endCard();
}

// ------------------------------------------------------------------ Setup assistant
namespace {
void openShell(const wchar_t* file, const wchar_t* params = nullptr) {
    ShellExecuteW(nullptr, L"open", file, params, nullptr, SW_SHOWNORMAL);
}

enum class Step { Done, Todo, Warn, Locked };

// "Step n" status line at the top of a step card.
void stepStatus(Step st, const char* label) {
    const ImU32 dot = st == Step::Done ? col::Green : st == Step::Warn ? col::Yellow : st == Step::Todo ? col::Red : col::Faint;
    chip(label, dot);
}
}  // namespace

void App::pollSetupJobs() {
    using namespace std::chrono;
    auto ready = [](auto& f) { return f.valid() && f.wait_for(seconds(0)) == std::future_status::ready; };
    if (ready(scanJob_)) {
        try {
            setDeviceList(scanJob_.get());
        } catch (const std::exception&) {
            // keep the previous list; the next scan retries
        }
    }
    if (ready(testJob_)) {
        try {
            testResult_ = testJob_.get();
        } catch (const std::exception& e) {
            testResult_.reset();
            testError_ = e.what();
        }
        lastScan_ = {};  // rescan right away (the format may have changed)
    }
    // While the assistant is open, rescan devices every 2 s so steps tick off by themselves
    // (e.g. after the user switches the cable to 7.1 in the Sound control panel).
    const auto now = steady_clock::now();
    if (!preview_ && page_ == kPageSetup && !scanJob_.valid() && !testJob_.valid() && now - lastScan_ > seconds(2)) {
        lastScan_ = now;
        scanJob_ = std::async(std::launch::async, [] {
            wasapi::ComScope com;
            return wasapi::listDevices(true);
        });
    }
}

void App::finishSetup(bool turnOn) {
    if (const auto cable = findCable(devInfos_); cable && !direct_) cfg_.dev.source = cable->name;
    cfg_.setupDone = true;
    saveSettings(cfg_);
    saved_ = cfg_;
    page_ = 0;
    if (turnOn && !wantRunning_) startStop();
}

void App::pageSetup() {
    topBar("Setup assistant", "Get your game's 7.1 sound into TacAudio. Steps tick off by themselves as you go.");
    static const std::vector<std::string> kModes = {"Direct (recommended)", "VB-CABLE"};
    segmentedRow("Mode", &setupMode_, kModes, 0,
                 "Direct: TacAudio runs inside Windows audio on your headphones - no cable, lowest delay. "
                 "VB-CABLE: the game plays into a virtual cable and TacAudio picks it up (works everywhere).");
    ImGui::Dummy(ImVec2(0, S(2)));
    if (setupMode_ == 0) setupDirect();
    else setupCable();
}

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
}  // namespace

void App::setupDirect() {
    const bool working = apoJob_.valid();
    const auto now = std::chrono::steady_clock::now();
    char buf[256];

    // --- 1. Headphones
    beginCard("##d1", "1  Choose your headphones", nullptr, 0, "Where you listen. TacAudio's engine is added to this device.");
    std::vector<std::string> phones;
    std::string suggested;
    for (const auto& d : devInfos_) {
        if (d.name.find("CABLE") != std::string::npos) continue;
        phones.push_back(d.name);
        if (d.isDefault) suggested = d.name;
    }
    if (std::find(phones.begin(), phones.end(), cfg_.dev.output) == phones.end() && !suggested.empty()) cfg_.dev.output = suggested;
    ImGui::BeginDisabled(working);
    if (comboRow("Headphones", &cfg_.dev.output, phones, nullptr, std::string()) && !preview_) {
        const auto* o = outputInfo();
        direct_ = o && apoctl::attachedTo(o->id);
    }
    ImGui::EndDisabled();
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    // --- 2. Install the engine
    beginCard("##d2", "2  Install the TacAudio engine", nullptr, 0,
              "Adds TacAudio to Windows' own audio processing for these headphones. No virtual cable, ~4 ms delay.");
    if (working) {
        spinner(S(9), col::Accent);
        ImGui::SameLine(0, S(10));
        std::snprintf(buf, sizeof(buf), "%s... (approve the Windows permission prompt; sound pauses for a few seconds)",
                      apoJobLabel_.c_str());
        ui::text(buf, col::Dim, 13.5f);
    } else if (direct_) {
        std::snprintf(buf, sizeof(buf), "Installed on %s", cfg_.dev.output.c_str());
        stepStatus(Step::Done, buf);
        if (actionButton("Reinstall", false)) runApoSetup("Installing", {L"install", L"--device", widen(cfg_.dev.output)});
        ImGui::SameLine(0, S(10));
        if (actionButton("Remove engine", false)) runApoSetup("Removing", {L"uninstall"});
    } else {
        stepStatus(Step::Todo, apoctl::dllInstalled() ? "Installed on a different device" : "Not installed yet");
        para("Windows asks for administrator permission once. Your sound pauses for a few seconds while Windows audio "
             "restarts. You can remove it again at any time.");
        if (actionButton("Install engine", true, !cfg_.dev.output.empty() && !preview_))
            runApoSetup("Installing", {L"install", L"--device", widen(cfg_.dev.output)});
    }
    if (!apoMsg_.empty()) para(apoMsg_.c_str(), col::Red, 12.5f);
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    // --- 3. Game settings
    beginCard("##d3", "3  Set up your game", nullptr, 0);
    para("1.  Set the game's audio output to your headphones (Windows Settings > System > Sound > Volume mixer), "
         "or leave it on the Windows default if your headphones are the default.\n"
         "2.  In the game's audio settings choose 7.1 / Surround / Home theater, and turn off the game's own "
         "3D-headphone mode.");
    if (actionButton("Open Volume mixer", false)) openShell(L"ms-settings:apps-volume");
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    // --- 4. Live check
    beginCard("##d4", "4  Check", nullptr, 0, "Updates live while your game plays.");
    const bool surround = preview_ || now - surroundSeen_ < std::chrono::seconds(15);
    const bool audio = preview_ || now - audioSeen_ < std::chrono::seconds(5);
    if (!direct_ && !preview_) stepStatus(Step::Locked, "Waiting for step 2");
    else if (surround) stepStatus(Step::Done, "Your game is sending 7.1 - TacAudio is placing it in 3D");
    else if (audio) stepStatus(Step::Warn, "Game audio is stereo - choose 7.1 / Home theater in the game");
    else stepStatus(Step::Locked, "Start your game - this updates by itself");
    endCard();
    ImGui::Dummy(ImVec2(0, S(10)));

    const bool ready = (direct_ || preview_) && !working;
    if (actionButton("Finish and turn on", true, ready, S(200)) && !preview_) finishSetup(true);
    ImGui::SameLine(0, S(10));
    if (actionButton(cfg_.setupDone ? "Close" : "Skip for now", false, true, S(140)) && !preview_) finishSetup(false);
    ImGui::SameLine(0, S(14));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(8));
    ui::text("A game that doesn't work in Direct mode? Use the VB-CABLE mode.", col::Faint, 12.5f);
}

void App::setupCable() {
    const auto cable = findCable(devInfos_);
    const bool has71 = cable && isSevenOne(*cable);
    const bool testing = testJob_.valid();
    const bool engineOn = wantRunning_ || session_.running() || busy();
    char buf[256];

    // --- 1. VB-CABLE installed | 2. Set to 7.1  (side by side)
    const float colW = (ImGui::GetContentRegionAvail().x - S(16)) * 0.5f;
    beginCard("##s1", "1  Install VB-CABLE", nullptr, colW,
              "VB-CABLE is a free virtual audio device by VB-Audio. Your game plays into it and TacAudio "
              "picks the sound up from there.");
    if (cable) {
        std::snprintf(buf, sizeof(buf), "Installed: %s", cable->name.c_str());
        stepStatus(Step::Done, buf);
        para("Free virtual audio device by VB-Audio.", col::Faint, 12.5f);
    } else {
        stepStatus(Step::Todo, "Not installed yet");
        para("Download VB-CABLE, extract the zip, right-click VBCABLE_Setup_x64.exe > Run as administrator > "
             "Install Driver, then restart Windows. TacAudio notices it automatically.");
        if (actionButton("Download VB-CABLE", true)) openShell(L"https://vb-audio.com/Cable/");
        ImGui::SameLine(0, S(10));
        if (actionButton("Check again", false)) lastScan_ = {};
    }
    endCard();
    ImGui::SameLine(0, S(16));

    beginCard("##s2", "2  Set the cable to 7.1", nullptr, colW,
              "Games only send surround sound to devices configured as 7.1.");
    if (!cable) {
        stepStatus(Step::Locked, "Waiting for step 1");
    } else if (has71) {
        std::snprintf(buf, sizeof(buf), "7.1 Surround (8 channels), %u Hz", cable->sampleRate);
        stepStatus(cable->sampleRate == 48000 ? Step::Done : Step::Warn, buf);
        if (cable->sampleRate == 48000) para("Ready for surround sound.", col::Faint, 12.5f);
        else
            para("Works, but 48000 Hz is recommended: CABLE Input > Properties > Advanced > a 48000 Hz format.",
                 col::Yellow, 12.5f);
    } else {
        std::snprintf(buf, sizeof(buf), "Currently %u channels - needs 7.1", cable->channels);
        stepStatus(Step::Todo, buf);
        para("In the Sound window that opens: Playback tab > CABLE Input > Configure > 7.1 Surround > Next > Finish. "
             "Then CABLE Input > Properties > Advanced > choose a 48000 Hz format > OK.");
        if (actionButton("Open Sound settings", true)) openShell(L"rundll32.exe", L"shell32.dll,Control_RunDLL mmsys.cpl,,0");
        ImGui::SameLine(0, S(10));
        if (actionButton("Check again", false)) lastScan_ = {};
    }
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    // --- 3. Test
    beginCard("##s3", "3  Test the 7.1 path", nullptr, 0,
              "Sends a short tone through each of the 8 channels of the cable and checks they all come back "
              "separately. Inaudible - nothing plays on your headphones.");
    if (!has71) {
        stepStatus(Step::Locked, "Waiting for step 2");
    } else {
        if (testing) {
            spinner(S(9), col::Accent);
            ImGui::SameLine(0, S(10));
            ui::text("Testing 8 channels (about 5 s)...", col::Dim, 13.5f);
        } else if (testResult_) {
            stepStatus(testResult_->pass ? Step::Done : Step::Todo, testResult_->message.c_str());
        } else if (!testError_.empty()) {
            stepStatus(Step::Todo, ("Test failed: " + testError_).c_str());
        } else {
            stepStatus(Step::Warn, "Not tested yet (optional, recommended)");
        }
        if (testResult_ && !testResult_->channels.empty() && !testing) {
            for (std::size_t i = 0; i < testResult_->channels.size(); ++i) {
                const auto& c = testResult_->channels[i];
                if (i) ImGui::SameLine(0, S(6));
                std::snprintf(buf, sizeof(buf), "%s %s", c.sent.c_str(), c.pass ? "ok" : "fail");
                chip(buf, c.pass ? col::Green : col::Red);
            }
        }
        if (engineOn) para("Turn TacAudio off (top right) to run the test.", col::Yellow, 12.5f);
        if (actionButton(testResult_ ? "Run again" : "Run test", !testResult_, !testing && !engineOn && !preview_)) {
            testResult_.reset();
            testError_.clear();
            const wasapi::DeviceInfo dev = *cable;
            testJob_ = std::async(std::launch::async, [dev] {
                wasapi::ComScope com;
                return runCableTest(dev);
            });
        }
    }
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));

    // --- 4. Headphones + game routing
    beginCard("##s4", "4  Connect your game and headphones", nullptr, 0);
    std::vector<std::string> phones;
    std::string suggested;
    for (const auto& d : devInfos_) {
        if (d.name.find("CABLE") != std::string::npos) continue;
        phones.push_back(d.name);
        if (d.isDefault) suggested = d.name;
    }
    if (suggested.empty() && !phones.empty()) suggested = phones.front();
    if (std::find(phones.begin(), phones.end(), cfg_.dev.output) == phones.end() && !suggested.empty())
        cfg_.dev.output = suggested;
    ImGui::BeginDisabled(engineOn);
    comboRow("Headphones", &cfg_.dev.output, phones, nullptr, std::string(),
             "Where you listen. TacAudio plays its 3D mix here.");
    ImGui::EndDisabled();
    para("1.  Windows Settings > System > Sound > Volume mixer: set your game's output device to CABLE Input. "
         "Keep Discord and your browser on your headphones.\n"
         "2.  In the game's audio settings choose 7.1 / Surround / Home theater, and turn off the game's own "
         "3D-headphone mode.",
         col::Dim, 13.5f);
    if (actionButton("Open Volume mixer", false)) openShell(L"ms-settings:apps-volume");
    endCard();
    ImGui::Dummy(ImVec2(0, S(10)));

    // --- Finish
    const bool ready = has71 && !cfg_.dev.output.empty() && !testing;
    if (actionButton("Finish and turn on", true, ready, S(200)) && !preview_) finishSetup(true);
    ImGui::SameLine(0, S(10));
    if (actionButton(cfg_.setupDone ? "Close" : "Skip for now", false, true, S(140)) && !preview_) finishSetup(false);
    if (!ready && !testing) {
        ImGui::SameLine(0, S(14));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(8));
        ui::text("Finish steps 1 and 2 first.", col::Faint, 12.5f);
    }
}

// ------------------------------------------------------------------ Sound direction overlay
void App::pollOverlay() {
    using namespace std::chrono;
    if (preview_) return;
    if (!cfg_.overlayOn && page_ != 3) {  // the Settings page shows a live preview even while off
        overlay_.hide();
        return;
    }
    const auto now = steady_clock::now();
    const float dt = duration<float>(now - lastOverlay_).count();
    if (dt < 0.03f) return;
    lastOverlay_ = now;

    // Footsteps only: each channel's footstep detector (in the engine), not its loudness.
    float peaks[8] = {}, steps[8] = {}, az[8] = {};
    bool lfe[8] = {};
    std::size_t n = 0;
    if (direct_) {
        if (const auto st = apoctl::readStatus(); st && wantRunning_ && st->channels > 0) {
            n = std::min<std::size_t>(st->channels, 8);
            const SpeakerLayout lay = layoutFromChannelMask(defaultChannelMask(static_cast<std::uint32_t>(n)));
            n = std::min(n, lay.size());
            if (stepSeenValid_)
                for (std::size_t i = 0; i < 5; ++i)  // audiodg restarted: its counters start over
                    stepCount_[i] += st->stepStat[i] >= stepSeen_[i] ? st->stepStat[i] - stepSeen_[i] : st->stepStat[i];
            stepSeen_ = st->stepStat;
            stepSeenValid_ = true;
            for (std::size_t c = 0; c < n; ++c) {
                steps[c] = st->hasLevels ? st->stepsLevel[static_cast<std::size_t>(cfg_.overlayLevel)][c] : st->steps[c];
                az[c] = lay.speakers[c].azimuthDeg;
                lfe[c] = lay.speakers[c].isLfe;
            }
        }
    } else {
        int d[5] = {};
        n = session_.inputMeters(peaks, az, lfe, steps, cfg_.overlayLevel, d);
        for (std::size_t i = 0; i < 5; ++i) stepCount_[i] += d[i];
    }
    if (n > 0) dirField_.updateWeights(steps, az, lfe, n, std::min(dt, 0.2f));
    else dirField_.decay(std::min(dt, 0.2f));

    if (dirField_.peak() > 0.05f) overlayActive_ = now;
    // Shown while game sound is playing; hidden after 2 s of quiet (no ring on an idle desktop).
    if (cfg_.overlayOn && now - overlayActive_ < seconds(2))
        overlay_.show(dirField_, static_cast<int>(cfg_.overlaySize * ui::dpi), cfg_.overlayOpacity / 100.0f,
                      static_cast<SoundOverlay::Position>(cfg_.overlayPos));
    else
        overlay_.hide();
}

// ------------------------------------------------------------------ Find your head
void App::startHeadTest() {
    if (headTestBusy() || preview_) return;
    htError_.clear();
    htResults_.clear();
    htIndex_ = 0;
    htPicked_ = -1;
    htAdvanceAt_ = -1.0;
    // VB-CABLE mode plays exclusively on the headphones: pause it for the test.
    htResume_ = !direct_ && (wantRunning_ || session_.running());
    if (htResume_) {
        wantRunning_ = false;
        if (job_.valid()) job_.wait();
        session_.stop();
    }
    std::vector<cli::HeadInfo> heads;
    for (const auto& h : cli::listHeads())
        if (heads.size() < 5) heads.push_back(h);  // the first five keep the test short
    headTest_ = std::make_unique<HeadTest>();
    htState_ = 1;  // Direct mode: apo.ini now says enabled=0 (plain stereo) so the test sounds pass unchanged
    HeadTest* t = headTest_.get();
    const std::string out = htDevice_.empty() ? cfg_.dev.output : htDevice_;
    const unsigned seed = static_cast<unsigned>(GetTickCount64());
    htPrep_ = std::async(std::launch::async, [t, heads, out, seed]() -> std::string {
        wasapi::ComScope com;
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // engine switches to plain stereo
            t->prepare(heads, out, seed);
            return {};
        } catch (const std::exception& e) {
            return e.what();
        }
    });
}

void App::stopHeadTest() {
    if (htPrep_.valid()) htPrep_.wait();
    if (headTest_) headTest_->close();
    if (htState_ != 3) htState_ = 0;
    if (htResume_) {
        htResume_ = false;
        wantRunning_ = true;  // supervise() restarts the cable session
        retryAt_ = {};
    }
}

void App::pageHeadTest() {
    topBar("Find your head", "Pick the head (HRTF) that matches your ears - about 2 minutes, headphones on.");
    const double now = ImGui::GetTime();
    if (!htDevice_.empty()) {  // dev self-test: start at once, answer each sound correctly once it has played
        if (htState_ == 0 && htError_.empty()) startHeadTest();
        if (htState_ == 2 && htAdvanceAt_ < 0 && !headTest_->playing()) {
            htPicked_ = headTest_->trial(htIndex_).dir;
            headTest_->answer(htIndex_, htPicked_);
            htAdvanceAt_ = now + 0.05;
        }
    }
    if (htState_ == 1 && htPrep_.valid() && htPrep_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        htError_ = htPrep_.get();
        if (htError_.empty()) {
            htState_ = 2;
            headTest_->play(0);
        } else {
            stopHeadTest();
        }
    }
    if (htState_ == 2 && htAdvanceAt_ > 0 && now >= htAdvanceAt_) {  // next sound
        htAdvanceAt_ = -1.0;
        htPicked_ = -1;
        if (++htIndex_ >= headTest_->size()) {
            htResults_ = headTest_->results();
            htState_ = 3;
            stopHeadTest();
        } else {
            headTest_->play(htIndex_);
        }
    }

    if (htState_ == 0 || htState_ == 1) {
        beginCard("##htintro", "How it works", nullptr, 0);
        char b[512];
        const auto heads = cli::listHeads();
        const std::size_t n = std::min<std::size_t>(heads.size(), 5);
        std::snprintf(b, sizeof(b),
                      "You hear %zu short walks - enemy footsteps, like in a game - one at a time, each from one of 8 "
                      "directions around you. Click where you heard them. The sounds are made with %zu different heads; the head that gives you the most "
                      "accurate directions - and the fewest front/back mix-ups - is the best one for your ears.\n\n"
                      "Use the headphones you play with, at a comfortable volume, in a quiet room. Don't guess too long: "
                      "your first impression is what counts.",
                      n * HeadTest::kDirs, n);
        para(b);
        ImGui::Dummy(ImVec2(0, S(8)));
        if (htState_ == 1) {
            spinner(S(9), col::Accent);
            ImGui::SameLine(0, S(10));
            ui::text("Preparing the sounds...", col::Dim, 13.5f);
        } else {
            if (actionButton("Start the test", true, !preview_ && !cfg_.dev.output.empty(), S(180))) startHeadTest();
            ImGui::SameLine(0, S(10));
            if (actionButton("Back", false, true, S(110))) page_ = 0;
        }
        if (!htError_.empty()) para(("Could not start: " + htError_).c_str(), col::Red, 12.5f);
        endCard();
        return;
    }

    if (htState_ == 3) {
        beginCard("##htres", "Your result", nullptr, 0,
                  "Average error: how far (in degrees) the direction you heard was from the real one. Front/back: "
                  "sounds from the front heard behind you, or the other way round.");
        for (std::size_t i = 0; i < htResults_.size(); ++i) {
            const auto& r = htResults_[i];
            ImGui::PushID(static_cast<int>(i));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const ImVec2 sz(w, S(58));
            dl->AddRectFilled(p, p + sz, i == 0 ? withAlpha(col::Accent, 0.12f) : col::Well, S(10));
            char b[160];
            std::snprintf(b, sizeof(b), "%zu", i + 1);
            dl->AddText(fonts.semibold, S(20), p + ImVec2(S(16), S(16)), i == 0 ? col::Accent : col::Faint, b);
            dl->AddText(fonts.semibold, S(15), p + ImVec2(S(48), S(10)), col::Text, r.name.c_str());
            dl->AddText(fonts.regular, S(12.5f), p + ImVec2(S(48), S(32)), col::Dim, r.description.c_str());
            std::snprintf(b, sizeof(b), "%.0f\xC2\xB0 average error   |   %d front/back", r.meanErrorDeg, r.frontBack);
            const ImVec2 bs = fonts.semibold->CalcTextSizeA(S(13.5f), FLT_MAX, 0, b);
            dl->AddText(fonts.semibold, S(13.5f), p + ImVec2(w - S(150) - bs.x - S(20), S(20)), i == 0 ? col::Text : col::Dim, b);
            const bool current = cfg_.sound.head == r.name;
            ImGui::SetCursorScreenPos(p + ImVec2(w - S(150) - S(12), S(12)));
            if (actionButton(current ? "In use" : (i == 0 ? "Use this head" : "Use"), i == 0 && !current, !current, S(150))) {
                cfg_.sound.head = r.name;
                requestRebuild();
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sz.y + S(8)));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        para("Results close together? Run the test again - a few rounds give a surer answer. The head is saved to the "
             "active game profile.",
             col::Faint, 12.5f);
        endCard();
        ImGui::Dummy(ImVec2(0, S(6)));
        if (actionButton("Run again", false, true, S(140))) {
            htState_ = 0;
            startHeadTest();
        }
        ImGui::SameLine(0, S(10));
        if (actionButton("Done", true, true, S(120))) {
            htState_ = 0;
            page_ = 0;
        }
        return;
    }

    // --- Running: where did it come from?
    const std::size_t total = headTest_->size();
    char b[160];
    std::snprintf(b, sizeof(b), "Sound %zu of %zu  -  where did it come from?", htIndex_ + 1, total);
    beginCard("##htrun", b, nullptr, 0, "Click the direction. The circle is you seen from above - your nose points up.");
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x;
        const float R = S(150);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 c(origin.x + w * 0.5f, origin.y + R + S(64));
        // progress bar
        dl->AddRectFilled(origin, origin + ImVec2(w, S(4)), col::Well, S(2));
        dl->AddRectFilled(origin, origin + ImVec2(w * static_cast<float>(htIndex_) / static_cast<float>(total), S(4)), col::Accent,
                          S(2));
        dl->AddCircle(c, R, col::Line, 64, S(1.5f));
        dl->AddCircle(c, R * 0.55f, withAlpha(col::Line, 0.5f), 64, S(1.0f));
        // the listener: head + nose (front is up) + ears
        dl->AddCircleFilled(c, S(30), col::CardHi);
        dl->AddTriangleFilled(c + ImVec2(-S(9), -S(27)), c + ImVec2(S(9), -S(27)), c + ImVec2(0, -S(42)), col::CardHi);
        dl->AddCircleFilled(c + ImVec2(-S(31), 0), S(7), col::CardHi);
        dl->AddCircleFilled(c + ImVec2(S(31), 0), S(7), col::CardHi);
        const char* labels[HeadTest::kDirs] = {"Front", "Front right", "Right", "Back right", "Back", "Back left", "Left", "Front left"};
        const bool playing = headTest_->playing();
        for (int d = 0; d < HeadTest::kDirs; ++d) {
            const float a = HeadTest::kAzimuth[d] * std::numbers::pi_v<float> / 180.0f;
            const ImVec2 pt = c + ImVec2(std::sin(a), -std::cos(a)) * R;
            const float r = S(24);
            ImGui::SetCursorScreenPos(pt - ImVec2(r, r));
            ImGui::PushID(d);
            const bool clicked = ImGui::InvisibleButton("##dir", ImVec2(2 * r, 2 * r));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool picked = htPicked_ == d;
            dl->AddCircleFilled(pt, r, picked ? col::Accent : hov ? col::CardHi : col::Well);
            dl->AddCircle(pt, r, picked || hov ? col::Accent : col::Line, 32, S(1.5f));
            const ImVec2 ls = fonts.semibold->CalcTextSizeA(S(12), FLT_MAX, 0, labels[d]);
            const ImVec2 lp = pt + ImVec2(std::sin(a), -std::cos(a)) * (r + S(16)) - ls * 0.5f;
            dl->AddText(fonts.semibold, S(12), lp, hov || picked ? col::Text : col::Dim, labels[d]);
            if (clicked && htAdvanceAt_ < 0) {
                htPicked_ = d;
                headTest_->answer(htIndex_, d);
                htAdvanceAt_ = now + 0.45;
            }
        }
        if (playing) {  // pulse while the sound plays
            const float t = static_cast<float>(std::fmod(now, 0.8) / 0.8);
            dl->AddCircle(c, S(34) + t * S(30), withAlpha(col::Accent, 0.6f * (1.0f - t)), 48, S(2));
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, c.y + R + S(52)));
        ImGui::Dummy(ImVec2(0, 0));
    }
    if (actionButton("Play again", false, htAdvanceAt_ < 0, S(140))) headTest_->play(htIndex_);
    ImGui::SameLine(0, S(10));
    if (actionButton("Stop test", false, true, S(120))) {
        stopHeadTest();
        htState_ = 0;
    }
    endCard();
}

// ------------------------------------------------------------------ Update check
void App::checkUpdatesNow() {
    if (preview_ || updJob_.valid()) return;
    updStatus_ = "Checking...";
    updJob_ = std::async(std::launch::async, fetchLatestRelease);
}

void App::pollUpdates() {
    using namespace std::chrono;
    if (updJob_.valid() && updJob_.wait_for(seconds(0)) == std::future_status::ready) {
        try {
            const ReleaseInfo r = updJob_.get();
            if (isNewerVersion(r.version, TAC_VERSION)) {
                if (!update_ || update_->version != r.version) updDismissed_ = false;
                update_ = r;
                updStatus_ = "TacAudio " + r.version + " is available (you have " TAC_VERSION ").";
            } else {
                update_.reset();
                updStatus_ = "You have the latest version (" TAC_VERSION ").";
            }
        } catch (const std::exception& e) {
            updStatus_ = std::string("Could not check: ") + e.what();
        }
    }
    const auto now = steady_clock::now();
    if (cfg_.checkUpdates && !preview_ && now >= nextUpdCheck_) {
        nextUpdCheck_ = now + hours(12);
        checkUpdatesNow();
    }
}

// ------------------------------------------------------------------ Game profiles
int App::findProfile(const std::string& exe, const std::string& path) const {
    for (std::size_t i = 1; i < profiles_.size(); ++i)
        if (profiles_[i].matches(exe, path)) return static_cast<int>(i);
    return 0;
}

int App::profileForGame(const GameEntry& g) const {
    for (std::size_t i = 1; i < profiles_.size(); ++i) {
        const auto& d = profiles_[i].dirs;
        if (!g.dirLower.empty() && std::find(d.begin(), d.end(), g.dirLower) != d.end()) return static_cast<int>(i);
    }
    for (std::size_t i = 1; i < profiles_.size(); ++i)
        if (nameMatches(profiles_[i], g.name)) return static_cast<int>(i);
    return 0;
}

void App::activateProfile(int index) {
    if (index < 0 || index >= static_cast<int>(profiles_.size()) || index == activeProfile_) return;
    profiles_[static_cast<std::size_t>(activeProfile_)].bundle = bundleOf(cfg_);
    activeProfile_ = index;
    applyBundle(cfg_, profiles_[static_cast<std::size_t>(index)].bundle);
    session_.setVolumeDb(cfg_.sound.volumeDb);
    session_.set3d(cfg_.sound.enabled3d);
    requestRebuild();  // Direct mode: apo.ini follows cfg_ on the next poll
    profilesDirty_ = true;
}

void App::pollProfiles() {
    using namespace std::chrono;
    // Library scan (Profiles page) and cover images.
    if (libraryJob_.valid() && libraryJob_.wait_for(seconds(0)) == std::future_status::ready) {
        try {
            library_ = libraryJob_.get();
        } catch (const std::exception&) {
        }
        libraryScanned_ = true;
        // Built-in profiles found in the library by name also match by install folder from now on.
        for (const auto& g : library_) {
            if (g.dirLower.empty()) continue;
            for (std::size_t i = 1; i < profiles_.size(); ++i) {
                auto& d = profiles_[i].dirs;
                if (nameMatches(profiles_[i], g.name) && std::find(d.begin(), d.end(), g.dirLower) == d.end()) {
                    d.push_back(g.dirLower);
                    profilesDirty_ = true;
                }
            }
        }
    }
    if (coverJob_.valid() && coverJob_.wait_for(seconds(0)) == std::future_status::ready) {
        auto [key, img] = coverJob_.get();
        Cover& c = covers_[key];
        if (img.w > 0 && ui::createTexture) c.tex = ui::createTexture(img.rgba.data(), img.w, img.h);
        c.failed = !c.tex;
    }
    if (preview_) return;
    if (!libraryScanned_ && !libraryJob_.valid() && page_ == kPageProfiles) libraryJob_ = std::async(std::launch::async, scanGameLibrary);

    const auto now = steady_clock::now();
    if (now - lastProfilePoll_ < seconds(1)) return;
    lastProfilePoll_ = now;

    // Edits on the Sound page belong to the active profile.
    auto& active = profiles_[static_cast<std::size_t>(activeProfile_)];
    if (!(active.bundle == bundleOf(cfg_))) {
        active.bundle = bundleOf(cfg_);
        profilesDirty_ = true;
    }
    if (profilesDirty_ && now - lastProfileSave_ > seconds(2)) {
        saveProfiles(profiles_);
        profilesDirty_ = false;
        lastProfileSave_ = now;
    }
    if (!cfg_.autoProfiles) return;

    // A game with a profile in front -> use its profile until that game exits.
    std::uint32_t pid = 0;
    std::string path;
    const std::string exe = foregroundExe(&pid, &path);
    const int match = exe.empty() ? 0 : findProfile(exe, path);
    if (match > 0) {
        if (match != activeProfile_ || !gameProcess_) {
            if (gameProcess_) CloseHandle(gameProcess_);
            gameProcess_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
            activateProfile(match);
        }
    } else if (gameProcess_ && WaitForSingleObject(gameProcess_, 0) == WAIT_OBJECT_0) {
        CloseHandle(gameProcess_);
        gameProcess_ = nullptr;
        activateProfile(0);
    }
}

void App::createProfileFor(const GameEntry& g, int startFrom) {
    GameProfile p;
    p.name = g.name;
    p.bundle = profiles_[static_cast<std::size_t>(std::clamp(startFrom, 0, static_cast<int>(profiles_.size()) - 1))].bundle;
    if (!g.dirLower.empty()) p.dirs.push_back(g.dirLower);
    profiles_.push_back(std::move(p));
    profilesDirty_ = true;
}

void App::addGameFromFile() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Games (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose the game's program (.exe)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    const fs::path p(file);
    GameProfile g;
    g.name = toUtf8(p.stem().wstring());
    g.exes = parseExeList(toUtf8(p.filename().wstring()));
    g.bundle = profiles_.front().bundle;
    g.manual = true;
    g.exePath = p.wstring();
    profiles_.push_back(std::move(g));
    profilesDirty_ = true;
    selectedGame_ = "manual:" + profiles_.back().name;
}

ImTextureID App::coverFor(const GameEntry& g) {
    const std::wstring& key = !g.cover.empty() ? g.cover : g.exe;
    if (key.empty()) return 0;
    auto it = covers_.find(key);
    if (it != covers_.end()) return it->second.tex;
    if (!coverJob_.valid()) {  // one image at a time, decoded off the UI thread
        const bool isCover = !g.cover.empty();
        coverJob_ = std::async(std::launch::async, [key, isCover] {
            wasapi::ComScope com;
            return std::make_pair(key, isCover ? loadImageFile(key, 300, 450) : loadExeIcon(key, 128));
        });
    }
    return 0;
}

namespace {
struct Tile {
    GameEntry game;
    int profile = 0;  // 0 = uses Default
    std::string key;
};

ImU32 hueColor(const std::string& s, float v) {  // stable colour per name, for tiles without art
    unsigned h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(static_cast<float>(h % 360) / 360.0f, 0.55f, v, r, g, b);
    return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), 255);
}

std::string initials(const std::string& name) {
    std::string out;
    bool start = true;
    for (unsigned char c : name) {
        if ((std::isupper(c) || std::isdigit(c)) && start && out.size() < 2) out += static_cast<char>(c);
        start = !std::isalnum(c);
    }
    return out.empty() ? "?" : out;
}

// Cover art (or a coloured card with the icon / initials), rounded, filling [p, p + sz].
void drawCover(ImDrawList* dl, ImVec2 p, ImVec2 sz, const GameEntry& g, ImTextureID tex, float rounding) {
    if (tex && !g.cover.empty()) {
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (g.coverIsWide) {  // landscape header art: crop the middle to portrait
            const float keep = (sz.x / sz.y) / (460.0f / 215.0f);
            uv0.x = 0.5f - keep * 0.5f;
            uv1.x = 0.5f + keep * 0.5f;
        }
        dl->AddImageRounded(ImTextureRef(tex), p, p + sz, uv0, uv1, IM_COL32_WHITE, rounding);
        return;
    }
    dl->AddRectFilled(p, p + sz, hueColor(g.name, 0.30f), rounding);
    dl->AddRectFilled(p + ImVec2(0, sz.y * 0.5f), p + sz, withAlpha(IM_COL32(0, 0, 0, 255), 0.25f), rounding,
                      ImDrawFlags_RoundCornersBottom);
    if (tex) {
        const float s = std::min(sz.x, sz.y) * 0.42f;
        const ImVec2 c = p + sz * 0.5f - ImVec2(s, s) * 0.5f - ImVec2(0, sz.y * 0.06f);
        dl->AddImage(ImTextureRef(tex), c, c + ImVec2(s, s));
    } else {
        const std::string in = initials(g.name);
        const float fs = sz.x * 0.30f;
        const ImVec2 ts = fonts.semibold->CalcTextSizeA(fs, FLT_MAX, 0, in.c_str());
        dl->AddText(fonts.semibold, fs, p + (sz - ts) * 0.5f - ImVec2(0, sz.y * 0.06f), IM_COL32(255, 255, 255, 210), in.c_str());
    }
}

std::string ellipsize(const std::string& s, float size, float maxW) {
    if (fonts.semibold->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= maxW) return s;
    std::string t = s;
    while (!t.empty() && fonts.semibold->CalcTextSizeA(size, FLT_MAX, 0, (t + "...").c_str()).x > maxW) t.pop_back();
    return t + "...";
}
}  // namespace

void App::pageProfiles() {
    topBar("Profiles", "Each game gets its own sound. TacAudio switches automatically when the game is in front.");

    // Tiles: library games, games added by hand, and built-in profiles not found in the library.
    std::vector<Tile> tiles;
    std::vector<bool> shown(profiles_.size(), false);
    for (const auto& g : library_) {
        Tile t{g, profileForGame(g), g.dirLower};
        if (t.profile > 0) shown[static_cast<std::size_t>(t.profile)] = true;
        tiles.push_back(std::move(t));
    }
    for (std::size_t i = 1; i < profiles_.size(); ++i) {
        const auto& p = profiles_[i];
        if (shown[i] && !p.manual) continue;
        Tile t;
        t.game.name = p.name;
        t.game.source = p.manual ? "Added manually" : "Built-in profile";
        t.game.exe = p.exePath;
        t.profile = static_cast<int>(i);
        t.key = "manual:" + p.name;
        tiles.push_back(std::move(t));
    }
    auto inFilter = [&](const Tile& t, int f) {
        switch (f) {
            case 0: return t.game.source != "Built-in profile";
            case 1: return t.profile > 0;
            case 2: return t.game.source == "Steam";
            case 3: return t.game.source == "Epic Games";
            default: return t.game.source == "Added manually";
        }
    };
    const char* filterNames[] = {"All games", "With profile", "Steam", "Epic Games", "Added manually"};
    int counts[5] = {};
    for (const auto& t : tiles)
        for (int f = 0; f < 5; ++f) counts[f] += inFilter(t, f) ? 1 : 0;

    const float sideW = S(210);
    const float h = ImGui::GetContentRegionAvail().y;

    // --- Left: active profile + filters
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(14)));
    ImGui::BeginChild("##libside", ImVec2(sideW, h), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        textUpper("active profile", col::Faint, 11.0f);
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(8)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Well);
        ImGui::PushStyleColor(ImGuiCol_Button, col::Well);
        if (ImGui::BeginCombo("##active", profiles_[static_cast<std::size_t>(activeProfile_)].name.c_str())) {
            for (std::size_t i = 0; i < profiles_.size(); ++i)
                if (ImGui::Selectable(profiles_[i].name.c_str(), static_cast<int>(i) == activeProfile_) && !preview_) {
                    if (gameProcess_) {  // a manual choice wins until the next game starts
                        CloseHandle(gameProcess_);
                        gameProcess_ = nullptr;
                    }
                    activateProfile(static_cast<int>(i));
                }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::Dummy(ImVec2(0, S(4)));
        const ImVec2 r = ImGui::GetCursorScreenPos();
        toggle("##autoprof", &cfg_.autoProfiles);
        dl->AddText(fonts.regular, S(13), r + ImVec2(S(54), S(4)), col::Dim, "Switch automatically");
        ImGui::Dummy(ImVec2(0, S(10)));
        dl->AddLine(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(sideW - S(28), 0), col::Line);
        ImGui::Dummy(ImVec2(0, S(10)));

        textUpper("games", col::Faint, 11.0f);
        ImGui::Dummy(ImVec2(0, S(2)));
        for (int f = 0; f < 5; ++f) {
            ImGui::PushID(f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 sz(sideW - S(28), S(34));
            const bool clicked = ImGui::InvisibleButton("##f", sz);
            const bool sel = libFilter_ == f;
            if (sel || ImGui::IsItemHovered())
                dl->AddRectFilled(p, p + sz, sel ? withAlpha(col::Accent, 0.16f) : col::CardHi, S(8));
            dl->AddText(fonts.semibold, S(13.5f), p + ImVec2(S(12), S(8)), sel ? col::Text : col::Dim, filterNames[f]);
            char n[16];
            std::snprintf(n, sizeof(n), "%d", counts[f]);
            const ImVec2 ns = fonts.semibold->CalcTextSizeA(S(12), FLT_MAX, 0, n);
            const ImVec2 bp(p.x + sz.x - ns.x - S(22), p.y + S(7));
            dl->AddRectFilled(bp, bp + ImVec2(ns.x + S(14), S(20)), col::Well, S(5));
            dl->AddText(fonts.semibold, S(12), bp + ImVec2(S(7), S(3)), sel ? col::Accent : col::Faint, n);
            if (clicked) libFilter_ = f;
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, S(10)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(6)));
        ImGui::PushFont(fonts.regular, 12.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col::Faint);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(
            "Profiles are matched by the game's folder or program name. Edits on the Sound page are saved to the "
            "active profile.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, S(16));

    // --- Right: search + add, grid, detail
    ImGui::BeginChild("##libmain", ImVec2(0, h));
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        const float searchW = std::min(S(360), avail - S(320));
        dl->AddRectFilled(sp, sp + ImVec2(searchW, S(36)), col::Card, S(18));
        dl->AddText(fonts.regular, S(14), sp + ImVec2(S(14), S(10)), col::Faint, icon::Search);
        ImGui::SetCursorScreenPos(sp + ImVec2(S(38), S(3)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), S(7)));
        ImGui::SetNextItemWidth(searchW - S(52));
        ImGui::InputTextWithHint("##search", "Search games", search_, sizeof(search_));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(sp.x + avail - S(150), sp.y + S(1)));
        if (pillButton("+  Add game", col::Accent, IM_COL32(20, 14, 8, 255), S(150)) && !preview_) addGameFromFile();
        if (ImGui::IsItemHovered()) tooltip("Pick a game's program (.exe) that is not in your Steam / Epic library");
        ImGui::SetCursorScreenPos(ImVec2(sp.x + avail - S(150) - S(40), sp.y + S(4)));
        if (iconButton("##rescan", icon::Refresh, "Scan Steam and Epic again", !libraryJob_.valid() && !preview_, 28)) {
            libraryScanned_ = false;
            library_.clear();
        }
        ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y + S(50)));

        std::string q = search_;
        std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::vector<const Tile*> view;
        for (const auto& t : tiles) {
            if (!inFilter(t, libFilter_)) continue;
            std::string n = t.game.name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!q.empty() && n.find(q) == std::string::npos) continue;
            view.push_back(&t);
        }
        std::stable_sort(view.begin(), view.end(), [](const Tile* a, const Tile* b) { return (a->profile > 0) > (b->profile > 0); });

        const Tile* sel = nullptr;
        for (const auto* t : view)
            if (t->key == selectedGame_) sel = t;
        const float detailW = sel ? S(270) : 0.0f;
        const float gridW = ImGui::GetContentRegionAvail().x - (sel ? detailW + S(16) : 0.0f);
        const float gridH = ImGui::GetContentRegionAvail().y;

        ImGui::BeginChild("##grid", ImVec2(gridW, gridH));
        if (!libraryScanned_ && library_.empty()) {
            spinner(S(9), col::Accent);
            ImGui::SameLine(0, S(10));
            ui::text("Looking for your games (Steam, Epic Games)...", col::Dim, 13.5f);
        } else if (view.empty()) {
            ui::text(q.empty() ? "No games here yet. Use \"+ Add game\" to add one." : "No game matches your search.", col::Dim,
                     13.5f);
        }
        const float gap = S(14);
        const float wAvail = ImGui::GetContentRegionAvail().x - S(4);
        const int cols = std::max(1, static_cast<int>((wAvail + gap) / (S(118) + gap)));
        const float tw = (wAvail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const ImVec2 tsz(tw, tw * 1.5f);
        ImDrawList* gdl = ImGui::GetWindowDrawList();
        for (std::size_t i = 0; i < view.size(); ++i) {
            const Tile& t = *view[i];
            if (i % static_cast<std::size_t>(cols)) ImGui::SameLine(0, gap);
            ImGui::PushID(t.key.c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(tsz.x, tsz.y + S(30)));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemVisible()) {
                const ImTextureID tex = coverFor(t.game);
                drawCover(gdl, p, tsz, t.game, tex, S(10));
                const bool isSel = t.key == selectedGame_;
                const bool isActive = t.profile > 0 && t.profile == activeProfile_;
                if (isSel || hov)
                    gdl->AddRect(p - ImVec2(S(2), S(2)), p + tsz + ImVec2(S(2), S(2)), isSel ? col::Accent : withAlpha(col::Text, 0.5f),
                                 S(11), 0, S(2));
                if (t.profile > 0) {  // profile badge
                    const char* b = isActive ? "ACTIVE" : "PROFILE";
                    const ImVec2 bs = fonts.semibold->CalcTextSizeA(S(10.5f), FLT_MAX, 0, b);
                    const ImVec2 bp = p + ImVec2(S(8), S(8));
                    gdl->AddRectFilled(bp, bp + bs + ImVec2(S(12), S(6)), isActive ? col::Accent : IM_COL32(10, 12, 16, 210), S(5));
                    gdl->AddText(fonts.semibold, S(10.5f), bp + ImVec2(S(6), S(3)), isActive ? IM_COL32(20, 14, 8, 255) : col::Text, b);
                }
                const std::string label = ellipsize(t.game.name, S(13), tsz.x);
                gdl->AddText(fonts.semibold, S(13), ImVec2(p.x, p.y + tsz.y + S(8)), hov || isSel ? col::Text : col::Dim,
                             label.c_str());
            }
            if (clicked) selectedGame_ = selectedGame_ == t.key ? std::string() : t.key;
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, S(10)));
        ImGui::EndChild();

        if (sel) {
            ImGui::SameLine(0, S(16));
            const Tile t = *sel;  // copy: the detail panel may change profiles_
            ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Card);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(16)));
            ImGui::BeginChild("##detail", ImVec2(detailW, gridH), ImGuiChildFlags_AlwaysUseWindowPadding);
            ImDrawList* ddl = ImGui::GetWindowDrawList();
            const float cw = ImGui::GetContentRegionAvail().x;
            const ImVec2 csz(std::min(cw, S(140)), std::min(cw, S(140)) * 1.5f);
            const ImVec2 cp = ImGui::GetCursorScreenPos() + ImVec2((cw - csz.x) * 0.5f, 0);
            drawCover(ddl, cp, csz, t.game, coverFor(t.game), S(10));
            ImGui::Dummy(ImVec2(cw, csz.y + S(10)));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushFont(fonts.semibold, 18.0f);
            ImGui::TextUnformatted(t.game.name.c_str());
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
            chip(t.game.source.c_str(), col::Faint);
            ImGui::Dummy(ImVec2(0, S(4)));

            if (t.profile > 0) {
                GameProfile& p = profiles_[static_cast<std::size_t>(t.profile)];
                const bool isActive = t.profile == activeProfile_;
                chip(isActive ? "Profile active now" : "Has its own profile", isActive ? col::Green : col::Accent);
                const SoundBundle& b = isActive ? bundleOf(cfg_) : p.bundle;
                char line[160];
                auto row = [&](const char* k, const char* v) {
                    ui::text(k, col::Faint, 12.5f);
                    ImGui::SameLine(S(96));
                    ui::text(v, col::Text, 12.5f, true);
                };
                std::snprintf(line, sizeof(line), "%s%s", b.sound.enabled3d ? "on, head " : "off", b.sound.enabled3d ? b.sound.head.c_str() : "");
                row("3D", line);
                row("Room", b.sound.room.c_str());
                std::snprintf(line, sizeof(line), "%s, punch %.0f", b.dynOn ? b.dynPreset.c_str() : "off", b.sound.punch);
                row("Dynamics", line);
                std::snprintf(line, sizeof(line), b.stepsOn ? (b.stepsSmart ? "+%.1f dB, smart" : "+%.1f dB") : "off",
                              b.stepsLevel);
                row("Footsteps", line);
                std::snprintf(line, sizeof(line), "%+.1f dB", b.sound.bassDb);
                row("Bass", line);
                ImGui::Dummy(ImVec2(0, S(6)));
                if (actionButton("Edit sound", true, true, cw) && !preview_) {
                    if (gameProcess_) {
                        CloseHandle(gameProcess_);
                        gameProcess_ = nullptr;
                    }
                    activateProfile(t.profile);
                    page_ = 0;
                }
                ImGui::Dummy(ImVec2(0, S(2)));
                if (!isActive && actionButton("Use now", false, true, cw) && !preview_) {
                    if (gameProcess_) {
                        CloseHandle(gameProcess_);
                        gameProcess_ = nullptr;
                    }
                    activateProfile(t.profile);
                }
                ImGui::Dummy(ImVec2(0, S(6)));
                ui::text("Also match programs", col::Faint, 12.0f);
                char exes[256];
                std::snprintf(exes, sizeof(exes), "%s", p.exeList().c_str());
                ImGui::SetNextItemWidth(-1);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Well);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(6)));
                if (ImGui::InputTextWithHint("##exes", "e.g. game.exe, game_dx12.exe", exes, sizeof(exes)) && !preview_) {
                    p.exes = parseExeList(exes);
                    profilesDirty_ = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, S(6)));
                if (actionButton("Delete profile", false, true, cw) && !preview_) {
                    if (t.profile == activeProfile_) activateProfile(0);
                    else if (t.profile < activeProfile_) --activeProfile_;
                    profiles_.erase(profiles_.begin() + t.profile);
                    profilesDirty_ = true;
                    if (t.key.rfind("manual:", 0) == 0) selectedGame_.clear();
                }
            } else {
                para("No profile yet - this game uses your Default sound.", col::Dim, 13.0f);
                ImGui::Dummy(ImVec2(0, S(4)));
                ui::text("Start from", col::Faint, 12.0f);
                createFrom_ = std::clamp(createFrom_, 0, static_cast<int>(profiles_.size()) - 1);
                ImGui::SetNextItemWidth(-1);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Well);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(6)));
                if (ImGui::BeginCombo("##from", profiles_[static_cast<std::size_t>(createFrom_)].name.c_str())) {
                    for (std::size_t i = 0; i < profiles_.size(); ++i)
                        if (ImGui::Selectable(profiles_[i].name.c_str(), static_cast<int>(i) == createFrom_)) createFrom_ = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, S(6)));
                if (actionButton("Create profile", true, true, cw) && !preview_) createProfileFor(t.game, createFrom_);
            }
            if (!t.game.dir.empty()) {
                ImGui::Dummy(ImVec2(0, S(8)));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::PushFont(fonts.regular, 11.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, col::Faint);
                ImGui::TextUnformatted(toUtf8(t.game.dir).c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ About page
void App::pageAbout() {
    topBar("About", "TacAudio - a spatial audio engine for competitive gaming.");
    beginCard("##about", "TacAudio " TAC_VERSION, nullptr, 0);
    ui::text("Binaural 3D audio for 7.1 game sound: measured-head HRTFs, diffuse-field equalisation, room "
             "modelling, combat dynamics and footstep clarity.",
             col::Dim, 13.5f);
    endCard();
    ImGui::Dummy(ImVec2(0, S(6)));
    beginCard("##credits", "Credits & licences", nullptr, 0);
    const char* lines[][2] = {
        {"SADIE II HRTF database", "University of York - Apache License 2.0 (data/hrtf/sadie2/LICENSE.txt)"},
        {"MIT KEMAR HRTF", "Bill Gardner & Keith Martin, MIT Media Lab"},
        {"libmysofa", "Christian Hoene / Symonics - BSD 3-Clause"},
        {"zlib", "Jean-loup Gailly & Mark Adler - zlib licence"},
        {"Dear ImGui", "Omar Cornut - MIT"},
        {"Segoe UI, Segoe Fluent Icons", "Windows system fonts, loaded from the system (not redistributed)"},
    };
    for (const auto& l : lines) {
        ui::text(l[0], col::Text, 14.0f, true);
        ImGui::SameLine(S(260));
        ui::text(l[1], col::Dim, 13.0f);
    }
    endCard();
}

}  // namespace tac

#include "app.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "cli.h"
#include "core/biquad.h"
#include "core/dynamics.h"
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
            else if (k == "autoEngine") s.autoEngine = v == "1";
            else if (k == "closeToTray") s.closeToTray = v == "1";
            else if (k == "hotkeys") s.hotkeys = v == "1";
            else if (k == "setupDone") s.setupDone = v == "1";
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
      << "\nbass=" << s.sound.bassDb << "\nautoEngine=" << (s.autoEngine ? 1 : 0)
      << "\ncloseToTray=" << (s.closeToTray ? 1 : 0) << "\nhotkeys=" << (s.hotkeys ? 1 : 0)
      << "\nsetupDone=" << (s.setupDone ? 1 : 0) << "\n";
}

// ------------------------------------------------------------------ app
App::App(bool previewRunning) : preview_(previewRunning) {
    cfg_ = loadSettings();
    saved_ = cfg_;
    lastStatus_ = lastSave_ = std::chrono::steady_clock::now();
    refreshDevices();
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
        if (demo[0]) return;
        CableTestResult t;
        t.pass = true;
        t.message = "All 8 channels arrive separately and unchanged.";
        for (const char* n : {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"}) t.channels.push_back({n, n, 0.0, -96.0, true});
        testResult_ = t;
    }
}

App::~App() {
    if (job_.valid()) job_.wait();
    session_.stop();
    if (!preview_) saveSettings(cfg_);
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
}

void App::startStop() {
    if (busy()) return;
    error_.clear();
    retries_ = 0;
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
    if (cfg_.autoEngine && !wantRunning_) startStop();
}

std::string App::trayTooltip() const {
    if (!wantRunning_) return "TacAudio - off";
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
    supervise();
    if (wantRebuild_ && !busy() && session_.running()) {
        wantRebuild_ = false;
        const SoundSettings s = cfg_.effective();
        launch("Applying", [this, s] { session_.apply(s); });
    }
    const auto now = std::chrono::steady_clock::now();
    if (!preview_ && now - lastStatus_ > std::chrono::milliseconds(500)) {
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
        session_.meters(l, r);
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
                         cfg_.hotkeys != saved_.hotkeys || cfg_.setupDone != saved_.setupDone;
    if (!preview_ && changed && now - lastSave_ > std::chrono::seconds(1)) {
        saveSettings(cfg_);
        saved_ = cfg_;
        lastSave_ = now;
    }
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
    ImGui::SameLine(0, 0);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(30), S(22)));
    ImGui::BeginChild("##main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (page_) {
        case 0: pageSound(); break;
        case 1: pageDevices(); break;
        case 2: pagePerformance(); break;
        case 3: pageSettings(); break;
        case kPageSetup: pageSetup(); break;
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

    const char* glyphs[] = {icon::Audio, icon::Devices, icon::Setup, icon::Gauge, icon::Settings, icon::Info};
    const char* labels[] = {"Sound", "Devices", "Setup", "Performance", "Settings", "About"};
    const int pages[] = {0, 1, kPageSetup, 2, 3, 4};
    for (int i = 0; i < 6; ++i)
        if (navItem(glyphs[i], labels[i], page_ == pages[i])) page_ = pages[i];

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
    const bool running = session_.running() || preview_;
    const bool working = busy();
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
        std::snprintf(chipText, sizeof(chipText), "Waiting for game audio");
        dot = col::Yellow;
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
    ImGui::Dummy(ImVec2(0, 0));
    return resetClicked;
}

// ------------------------------------------------------------------ Sound page
void App::pageSound() {
    const AppSettings def;
    if (topBar("Sound", "Tune how the game sounds - every change applies live while you play.", true)) {
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
              "gun cracks in the same band are not boosted.");
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
    ImGui::Dummy(ImVec2(0, S(37)));  // match the dynamics card height (it has one more row)
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
    if (pillButton("Refresh devices", col::Card, col::Text, S(170))) refreshDevices();
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(10));
    if (pillButton("Setup assistant", col::Card, col::Text, S(170))) page_ = kPageSetup;
    if (ImGui::IsItemHovered()) tooltip("Step-by-step check of VB-CABLE, 7.1 and your devices");
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
}

// ------------------------------------------------------------------ Setup assistant
namespace {
void openShell(const wchar_t* file, const wchar_t* params = nullptr) {
    ShellExecuteW(nullptr, L"open", file, params, nullptr, SW_SHOWNORMAL);
}

// Wrapped paragraph.
void para(const char* s, ImU32 color = col::Dim, float size = 13.5f) {
    ImGui::PushFont(fonts.regular, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Primary (orange) or secondary button; drawn greyed out and inert when disabled.
bool actionButton(const char* label, bool primary, bool enabled = true, float width = 0.0f) {
    const ImU32 bg = !enabled ? col::Well : primary ? col::Accent : col::CardHi;
    const ImU32 fg = !enabled ? col::Faint : primary ? IM_COL32(20, 14, 8, 255) : col::Text;
    ImGui::BeginDisabled(!enabled);
    const bool clicked = pillButton(label, bg, fg, width);
    ImGui::EndDisabled();
    return clicked && enabled;
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
    if (const auto cable = findCable(devInfos_)) cfg_.dev.source = cable->name;
    cfg_.setupDone = true;
    saveSettings(cfg_);
    saved_ = cfg_;
    page_ = 0;
    if (turnOn && !wantRunning_) startStop();
}

void App::pageSetup() {
    topBar("Setup assistant", "Get your game's 7.1 sound into TacAudio. Steps tick off by themselves as you go.");
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

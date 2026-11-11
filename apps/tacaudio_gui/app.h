#pragma once
// TacAudio app UI: state, settings persistence and the pages (Sound, Devices, Setup, Performance,
// Settings, About).

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "apo_control.h"
#include "cable_check.h"
#include "game_library.h"
#include "head_test.h"
#include "imgui.h"
#include "profiles.h"
#include "update_check.h"
#include "session.h"

namespace tac {

struct AppSettings {
    DeviceSettings dev;
    SoundSettings sound;          // head, room, punch, volume, 3D
    bool dynOn = true;
    std::string dynPreset = "medium";
    bool stepsOn = true;
    float stepsLevel = 3.0f;
    bool stepsSmart = true;

    // Headphone EQ (the same for every game profile: it belongs to the headphones).
    bool eqOn = false;
    std::string eqText;   // compact form (core/headphone_eq.h)
    std::string eqName;   // where it came from (file name / headphone model)

    // App behaviour.
    bool startWithWindows = false;  // mirrored from the registry Run key
    bool autoEngine = true;         // turn the engine on when the app starts
    bool closeToTray = true;        // the window's X keeps TacAudio running in the tray
    bool hotkeys = true;            // Ctrl+Alt+F10 power, Ctrl+Alt+F9 3D on/off
    bool setupDone = false;         // setup assistant finished or skipped (shown on first launch)
    bool autoProfiles = true;       // switch the game profile when a game comes to the front
    bool checkUpdates = true;       // ask GitHub for a newer release (at launch, then twice a day)

    SoundSettings effective() const {  // what the engine gets
        SoundSettings s = sound;
        s.dynamics = dynOn ? dynPreset : "off";
        s.footstepsDb = stepsOn ? stepsLevel : 0.0f;
        s.footstepsSmart = stepsSmart;
        s.eq = eqOn ? eqText : std::string();
        return s;
    }
};

AppSettings loadSettings();
void saveSettings(const AppSettings& s);

// Registry "Run" entry so TacAudio starts with Windows (per user, no admin needed).
bool autostartEnabled();
void setAutostart(bool on);

class App {
public:
    explicit App(bool previewRunning = false);  // preview: fake live data (screenshots only)
    ~App();
    void frame();           // build the whole UI for this frame
    void tick();            // background work while the window is hidden (tray mode)
    void setPage(int p) { page_ = p; }

    // Called by the window host (tray menu, hotkeys, launch).
    void togglePower() { startStop(); }
    void toggle3d();
    void autoStartIfEnabled();
    bool engineWanted() const { return wantRunning_; }
    bool enabled3d() const { return cfg_.sound.enabled3d; }
    bool closeToTray() const { return cfg_.closeToTray; }
    bool hotkeysEnabled() const { return cfg_.hotkeys; }
    std::string trayTooltip() const;

    static constexpr const char* kHotkeyPower = "Ctrl + Alt + F10";
    static constexpr const char* kHotkey3d = "Ctrl + Alt + F9";
    static constexpr int kPageSetup = 5;
    static constexpr int kPageProfiles = 6;
    static constexpr int kPageHeadTest = 7;

private:
    void sidebar();
    // Returns true if the optional "Reset all" button (showReset) was clicked.
    bool topBar(const char* title, const char* subtitle, bool showReset = false);
    void pageSound();
    void pageDevices();
    void pagePerformance();
    void pageSettings();
    void pageAbout();
    void pageSetup();
    void eqCard();
    void importEq(const std::string& text, const std::string& name);
    void pageProfiles();
    void pageHeadTest();
    void startHeadTest();
    void stopHeadTest();
    bool headTestBusy() const { return htState_ == 1 || htState_ == 2; }

    // Game profiles.
    void pollProfiles();
    void pollUpdates();
    void checkUpdatesNow();
    void activateProfile(int index);
    int profileForGame(const GameEntry& g) const;
    int findProfile(const std::string& exe, const std::string& path) const;
    void createProfileFor(const GameEntry& g, int startFrom);
    void addGameFromFile();
    ImTextureID coverFor(const GameEntry& g);
    void setupDirect();   // Setup page, Direct mode cards
    void setupCable();    // Setup page, VB-CABLE mode cards
    void finishSetup(bool turnOn);

    void startStop();
    void supervise();  // reconnect after device loss / failed start
    void requestRebuild() { wantRebuild_ = true; }
    void pollBackground();
    void refreshDevices();
    void setDeviceList(std::vector<wasapi::DeviceInfo> list);
    void pollSetupJobs();
    // Direct mode (engine inside Windows audio, apps/apo).
    std::string apoIniText() const;
    void pollDirect();
    void runApoSetup(const char* label, std::vector<std::wstring> args);
    const wasapi::DeviceInfo* outputInfo() const;
    bool busy() const;
    template <typename F>
    void launch(const char* label, F fn);

    AppSettings cfg_, saved_;
    AudioSession session_;
    std::future<std::string> job_;
    std::string jobLabel_, error_;
    bool wantRebuild_ = false;
    bool preview_ = false;
    int page_ = 0;
    bool wantRunning_ = false;  // user intent: engine should be on (reconnects until it is)
    bool lastJobWasStart_ = false;
    std::chrono::steady_clock::time_point retryAt_{};
    int retries_ = 0;

    SessionStatus status_;
    std::chrono::steady_clock::time_point lastStatus_, lastSave_;
    std::deque<float> histLatency_, histCpu_;
    float meterL_ = -60, meterR_ = -60, peakL_ = -60, peakR_ = -60;
    double peakHoldL_ = 0, peakHoldR_ = 0;

    std::vector<std::string> outputs_;
    std::vector<wasapi::DeviceInfo> devInfos_;

    // Setup assistant: background device scans and the cable test.
    std::future<std::vector<wasapi::DeviceInfo>> scanJob_;
    std::chrono::steady_clock::time_point lastScan_{};
    std::future<CableTestResult> testJob_;
    std::optional<CableTestResult> testResult_;
    std::string testError_;
    std::vector<std::string> headNames_, headDescs_;

    // Direct mode state.
    bool direct_ = false;                 // engine attached to the chosen headphone output
    bool apoSync_ = false;                // write apo.ini (set on a normal launch only)
    int setupMode_ = 0;                   // Setup page: 0 = Direct, 1 = VB-CABLE
    std::future<int> apoJob_;
    std::string apoJobLabel_, apoMsg_;
    std::string lastApoIni_;
    std::chrono::steady_clock::time_point lastApoPoll_{}, surroundSeen_{}, audioSeen_{};
    std::optional<apoctl::Status> apoStatus_;
    std::string eqError_;
    bool engineOutdated_ = false;         // Direct mode: installed engine is older than this app's

    // Game profiles and the game library (Profiles page).
    std::vector<GameProfile> profiles_;
    int activeProfile_ = 0;
    bool profilesDirty_ = false;
    std::chrono::steady_clock::time_point lastProfilePoll_{}, lastProfileSave_{};
    void* gameProcess_ = nullptr;          // HANDLE of the game that selected the active profile
    std::vector<GameEntry> library_;
    std::future<std::vector<GameEntry>> libraryJob_;
    bool libraryScanned_ = false;
    int libFilter_ = 0;                    // 0 all, 1 with profile, 2 Steam, 3 Epic, 4 added manually
    char search_[96] = {};
    std::string selectedGame_;             // key of the selected tile ("" = none)
    int createFrom_ = 0;
    struct Cover {
        ImTextureID tex = 0;
        bool failed = false;
    };
    std::map<std::wstring, Cover> covers_;
    std::future<std::pair<std::wstring, Image>> coverJob_;

    // "Find your head" listening test.
    std::unique_ptr<HeadTest> headTest_;
    std::future<std::string> htPrep_;
    int htState_ = 0;                      // 0 intro, 1 preparing, 2 running, 3 results
    std::size_t htIndex_ = 0;
    std::string htError_;
    bool htResume_ = false;                // restart the cable session afterwards
    double htAdvanceAt_ = -1.0;            // ImGui time to move to the next sound
    int htPicked_ = -1;
    std::vector<HeadTest::Result> htResults_;
    std::string htDevice_;                 // dev: TACAUDIO_SMOKE_HEADTEST plays on this device instead

    // Update check.
    std::future<ReleaseInfo> updJob_;
    std::optional<ReleaseInfo> update_;    // a newer release than this build
    std::string updStatus_;
    bool updDismissed_ = false;
    std::chrono::steady_clock::time_point nextUpdCheck_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
};

}  // namespace tac

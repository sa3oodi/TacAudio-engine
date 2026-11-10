#pragma once
// TacAudio app UI: state, settings persistence and the pages (Sound, Devices, Performance, About).

#include <chrono>
#include <deque>
#include <future>
#include <string>
#include <vector>

#include "session.h"

namespace tac {

struct AppSettings {
    DeviceSettings dev;
    SoundSettings sound;          // head, room, punch, volume, 3D
    bool dynOn = true;
    std::string dynPreset = "medium";
    bool stepsOn = true;
    float stepsLevel = 3.0f;

    // App behaviour.
    bool startWithWindows = false;  // mirrored from the registry Run key
    bool autoEngine = true;         // turn the engine on when the app starts
    bool closeToTray = true;        // the window's X keeps TacAudio running in the tray
    bool hotkeys = true;            // Ctrl+Alt+F10 power, Ctrl+Alt+F9 3D on/off

    SoundSettings effective() const {  // what the engine gets
        SoundSettings s = sound;
        s.dynamics = dynOn ? dynPreset : "off";
        s.footstepsDb = stepsOn ? stepsLevel : 0.0f;
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

private:
    void sidebar();
    // Returns true if the optional "Reset all" button (showReset) was clicked.
    bool topBar(const char* title, const char* subtitle, bool showReset = false);
    void pageSound();
    void pageDevices();
    void pagePerformance();
    void pageSettings();
    void pageAbout();

    void startStop();
    void supervise();  // reconnect after device loss / failed start
    void requestRebuild() { wantRebuild_ = true; }
    void pollBackground();
    void refreshDevices();
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
    std::vector<std::string> headNames_, headDescs_;
};

}  // namespace tac

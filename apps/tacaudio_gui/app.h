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

    SoundSettings effective() const {  // what the engine gets
        SoundSettings s = sound;
        s.dynamics = dynOn ? dynPreset : "off";
        s.footstepsDb = stepsOn ? stepsLevel : 0.0f;
        return s;
    }
};

AppSettings loadSettings();
void saveSettings(const AppSettings& s);

class App {
public:
    explicit App(bool previewRunning = false);  // preview: fake live data (screenshots only)
    ~App();
    void frame();           // build the whole UI for this frame
    void setPage(int p) { page_ = p; }

private:
    void sidebar();
    // Returns true if the optional "Reset all" button (showReset) was clicked.
    bool topBar(const char* title, const char* subtitle, bool showReset = false);
    void pageSound();
    void pageDevices();
    void pagePerformance();
    void pageAbout();

    void startStop();
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

    SessionStatus status_;
    std::chrono::steady_clock::time_point lastStatus_, lastSave_;
    std::deque<float> histLatency_, histCpu_;
    float meterL_ = -60, meterR_ = -60, peakL_ = -60, peakR_ = -60;
    double peakHoldL_ = 0, peakHoldR_ = 0;

    std::vector<std::string> outputs_;
    std::vector<std::string> headNames_, headDescs_;
};

}  // namespace tac

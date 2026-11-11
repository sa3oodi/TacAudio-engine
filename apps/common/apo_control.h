#pragma once
// The app's side of "Direct mode": TacAudio's engine running inside Windows' audio engine as an
// APO (apps/apo). Finds where it is installed, writes its settings, reads its live status and
// runs tacapo-setup.exe with administrator rights.

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tac::apoctl {

// Playback devices (endpoint ids as in wasapi::DeviceInfo::id) the engine is attached to.
bool attachedTo(const std::wstring& endpointId);
bool dllInstalled();
// true if the installed engine DLL differs from the one shipped next to this app (the app was
// updated, the engine in Windows audio not yet). Reinstalling copies the new one.
bool engineOutdated();

std::filesystem::path dataDir();  // %ProgramData%\TacAudio

// Replaces apo.ini (the engine re-reads it within ~0.1 s). Returns false if not writable.
bool writeSettings(const std::string& iniText);
// Value of `key` in apo.ini, or `def`.
std::string readSetting(const std::string& key, const std::string& def);

struct Status {
    int streams = 0;               // game / app streams currently processed
    unsigned channels = 0;         // input channels (8 = 7.1)
    std::array<float, 8> inDb{};   // input peak per channel since the last status (dBFS)
    float outLDb = -120, outRDb = -120;
    float dynMinDb = 0, dynMaxDb = 0;
};
// Latest status if the engine wrote one in the last 1.5 s.
std::optional<Status> readStatus();

// Runs tacapo-setup.exe (next to this exe) elevated with `args`; blocks until it exits.
// Returns its exit code (-1: could not start / UAC declined). Output goes to `logFile`.
int runSetupElevated(const std::vector<std::wstring>& args, const std::filesystem::path& logFile);

}  // namespace tac::apoctl

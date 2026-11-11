#pragma once
// Game profiles: a set of sound settings per game, picked automatically when the game is in
// front. Stored in %APPDATA%\TacAudio\profiles.ini. Profile 0 is always "Default" (used when no
// game with a profile is running).

#include <cstdint>
#include <string>
#include <vector>

#include "session.h"

namespace tac {

// The per-profile part of the app settings (everything on the Sound page).
struct SoundBundle {
    SoundSettings sound;
    bool dynOn = true;
    std::string dynPreset = "medium";
    bool stepsOn = true;
    float stepsLevel = 3.0f;
    bool stepsSmart = true;

    bool operator==(const SoundBundle& o) const {
        return sound.head == o.sound.head && sound.room == o.sound.room && sound.punch == o.sound.punch &&
               sound.bassDb == o.sound.bassDb && sound.volumeDb == o.sound.volumeDb &&
               sound.enabled3d == o.sound.enabled3d && dynOn == o.dynOn && dynPreset == o.dynPreset &&
               stepsOn == o.stepsOn && stepsLevel == o.stepsLevel &&
               stepsSmart == o.stepsSmart;
    }
};

struct GameProfile {
    std::string name;
    std::vector<std::string> exes;  // lower-case file names, e.g. "cod.exe"
    SoundBundle bundle;
    std::vector<std::string> dirs;  // install folders (normDir form): any program inside them matches
    bool manual = false;            // added with "Add game" (shown in the library as its own tile)
    std::wstring exePath;           // manual: the chosen executable (for its icon)

    // exeLower: file name; pathLower: full path, lower case.
    bool matches(const std::string& exeLower, const std::string& pathLower = {}) const;
    std::string exeList() const;  // "a.exe, b.exe"
};

// Built-in starting points (Default first). `base` is the user's current sound (kept for Default
// and used for the head / volume of the game profiles).
std::vector<GameProfile> defaultProfiles(const SoundBundle& base);

// Loads profiles.ini; if missing or empty returns defaultProfiles(base). Profile 0 is Default.
std::vector<GameProfile> loadProfiles(const SoundBundle& base);
void saveProfiles(const std::vector<GameProfile>& profiles);

// "a.exe, B.EXE ,c" -> {"a.exe", "b.exe", "c.exe"}
std::vector<std::string> parseExeList(const std::string& text);

// Executable file name (lower case) of the process owning the foreground window, its pid and
// full path (lower case). Empty if unknown.
std::string foregroundExe(std::uint32_t* pid = nullptr, std::string* pathLower = nullptr);

// Built-in profiles that match a library game by name ("Call of Duty", "Battlefield", ...).
bool nameMatches(const GameProfile& p, const std::string& gameName);

}  // namespace tac

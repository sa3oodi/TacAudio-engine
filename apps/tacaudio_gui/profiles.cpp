#include "profiles.h"

#include "game_library.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>

#include <windows.h>
#include <shlobj.h>

namespace fs = std::filesystem;

namespace tac {

namespace {
fs::path profilesPath() {
    PWSTR p = nullptr;
    fs::path dir = fs::temp_directory_path();
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir / "TacAudio" / "profiles.ini";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n\"");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n\"");
    return s.substr(a, b - a + 1);
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(std::max(0, n)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
}  // namespace

bool GameProfile::matches(const std::string& exeLower, const std::string& pathLower) const {
    if (!exeLower.empty() && std::find(exes.begin(), exes.end(), exeLower) != exes.end()) return true;
    for (const auto& d : dirs)
        if (!d.empty() && pathLower.rfind(d, 0) == 0) return true;
    return false;
}

namespace {
std::string key(const std::string& s) {  // letters and digits only, lower case
    std::string k;
    for (unsigned char c : s)
        if (std::isalnum(c)) k += static_cast<char>(std::tolower(c));
    return k;
}
}  // namespace

bool nameMatches(const GameProfile& p, const std::string& gameName) {
    if (p.manual || p.name == "Default") return false;
    const std::string pk = key(p.name), gk = key(gameName);
    return !pk.empty() && gk.find(pk) != std::string::npos;
}

std::string GameProfile::exeList() const {
    std::string s;
    for (const auto& e : exes) s += (s.empty() ? "" : ", ") + e;
    return s;
}

std::vector<std::string> parseExeList(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find_first_of(",;", start);
        std::string item = lower(trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
        if (const auto slash = item.find_last_of("\\/"); slash != std::string::npos) item = item.substr(slash + 1);
        if (!item.empty()) {
            if (item.size() < 4 || item.substr(item.size() - 4) != ".exe") item += ".exe";
            if (std::find(out.begin(), out.end(), item) == out.end()) out.push_back(item);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::vector<GameProfile> defaultProfiles(const SoundBundle& base) {
    std::vector<GameProfile> v;
    v.push_back({"Default", {}, base});
    // Starting points from listening in each game; every value can be changed on the Sound page.
    auto game = [&](const char* name, std::vector<std::string> exes, const char* preset, float punch, float steps,
                    float bass) {
        GameProfile p{name, std::move(exes), base};
        p.bundle.sound.enabled3d = true;
        p.bundle.dynOn = true;
        p.bundle.dynPreset = preset;
        p.bundle.sound.punch = punch;
        p.bundle.stepsOn = true;
        p.bundle.stepsLevel = steps;
        p.bundle.sound.bassDb = bass;
        v.push_back(std::move(p));
    };
    // Call of Duty (Warzone, Black Ops, Modern Warfare run as cod.exe in the CoD launcher).
    game("Call of Duty", {"cod.exe", "blackops6.exe", "modernwarfare.exe", "cod24-cod.exe"}, "medium", 55.0f, 6.0f, -3.0f);
    // Battlefield: loud vehicles and explosions -> stronger taming, less sub.
    game("Battlefield", {"bf6.exe", "bf2042.exe", "bfv.exe", "bf1.exe"}, "strong", 50.0f, 4.5f, -4.0f);
    game("Apex Legends", {"r5apex.exe", "r5apex_dx12.exe"}, "medium", 60.0f, 4.5f, -2.0f);
    game("Fortnite", {"fortniteclient-win64-shipping.exe"}, "light", 65.0f, 3.0f, -2.0f);
    return v;
}

std::vector<GameProfile> loadProfiles(const SoundBundle& base) {
    std::ifstream f(profilesPath());
    if (!f.is_open()) return defaultProfiles(base);
    std::vector<GameProfile> v;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[profile]") {
            v.push_back({"Profile", {}, base});
            continue;
        }
        if (v.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), val = line.substr(eq + 1);
        auto& b = v.back().bundle;
        try {
            if (k == "name") v.back().name = val;
            else if (k == "exes") v.back().exes = parseExeList(val);
            else if (k == "dirs") {
                for (std::size_t a = 0; a < val.size();) {
                    const auto bar = val.find('|', a);
                    const std::string d = val.substr(a, bar == std::string::npos ? std::string::npos : bar - a);
                    if (!d.empty()) v.back().dirs.push_back(d);
                    if (bar == std::string::npos) break;
                    a = bar + 1;
                }
            } else if (k == "manual") v.back().manual = val == "1";
            else if (k == "exePath") v.back().exePath = fromUtf8(val);
            else if (k == "head") b.sound.head = val;
            else if (k == "room") b.sound.room = val;
            else if (k == "punch") b.sound.punch = std::stof(val);
            else if (k == "volume") b.sound.volumeDb = std::stof(val);
            else if (k == "3d") b.sound.enabled3d = val == "1";
            else if (k == "dynOn") b.dynOn = val == "1";
            else if (k == "dynPreset") b.dynPreset = val;
            else if (k == "stepsOn") b.stepsOn = val == "1";
            else if (k == "stepsLevel") b.stepsLevel = std::stof(val);
            else if (k == "bass") b.sound.bassDb = std::stof(val);
            else if (k == "stepsSmart") b.stepsSmart = val == "1";
        } catch (const std::exception&) {
            // malformed value: keep the default
        }
    }
    if (v.empty()) return defaultProfiles(base);
    if (v.front().name != "Default" || !v.front().exes.empty()) v.insert(v.begin(), GameProfile{"Default", {}, base});
    return v;
}

void saveProfiles(const std::vector<GameProfile>& profiles) {
    std::error_code ec;
    fs::create_directories(profilesPath().parent_path(), ec);
    std::ofstream f(profilesPath());
    for (const auto& p : profiles) {
        const auto& b = p.bundle;
        std::string dirs;
        for (const auto& d : p.dirs) dirs += (dirs.empty() ? "" : "|") + d;
        f << "[profile]\nname=" << p.name << "\nexes=" << p.exeList() << "\ndirs=" << dirs
          << "\nmanual=" << (p.manual ? 1 : 0) << "\nexePath=" << toUtf8(p.exePath) << "\nhead=" << b.sound.head
          << "\nroom=" << b.sound.room << "\npunch=" << b.sound.punch << "\nvolume=" << b.sound.volumeDb
          << "\n3d=" << (b.sound.enabled3d ? 1 : 0) << "\ndynOn=" << (b.dynOn ? 1 : 0) << "\ndynPreset=" << b.dynPreset
          << "\nstepsOn=" << (b.stepsOn ? 1 : 0) << "\nstepsLevel=" << b.stepsLevel
          << "\nbass=" << b.sound.bassDb << "\nstepsSmart=" << (b.stepsSmart ? 1 : 0) << "\n\n";
    }
}

std::string foregroundExe(std::uint32_t* pid, std::string* pathLower) {
    HWND w = GetForegroundWindow();
    if (!w) return {};
    DWORD id = 0;
    GetWindowThreadProcessId(w, &id);
    if (pid) *pid = id;
    if (!id) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!h) return {};
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string exe;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        exe = lower(narrow(fs::path(path).filename().c_str()));
        if (pathLower) {
            std::wstring wl(path);
            for (auto& c : wl) c = static_cast<wchar_t>(std::towlower(c));
            *pathLower = narrow(wl.c_str());
        }
    }
    CloseHandle(h);
    return exe;
}

}  // namespace tac

#pragma once
// Installed games for the Profiles page: Steam and Epic Games libraries found on this PC, with
// their cover art from the launchers' local caches (nothing is downloaded or shipped) or, as a
// fallback, the game's own icon.

#include <cstdint>
#include <string>
#include <vector>

namespace tac {

struct GameEntry {
    std::string name;
    std::string source;      // "Steam", "Epic Games", "Added manually"
    std::string dirLower;    // install folder, lower case, ending with '\' ("" if unknown)
    std::wstring dir;        // install folder as on disk
    std::wstring cover;      // portrait cover image (jpg/png) or empty
    bool coverIsWide = false;  // landscape image (header art): shown cropped
    std::wstring exe;        // an executable for the icon fallback (may be empty)
};

// Scans the Steam and Epic libraries (a few hundred ms; call from a worker thread).
std::vector<GameEntry> scanGameLibrary();

// Lower case + '\' at the end, for prefix matching against process paths.
std::string normDir(const std::wstring& dir);
std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);

// Decoded image, RGBA8, top-down.
struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
};
// Decodes a jpg/png scaled down to fit within maxW x maxH (keeps the aspect ratio). Needs COM.
Image loadImageFile(const std::wstring& path, int maxW, int maxH);
// The executable's icon at `size` pixels (largest available, scaled).
Image loadExeIcon(const std::wstring& exe, int size);

}  // namespace tac

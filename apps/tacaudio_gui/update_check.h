#pragma once
// Update check: asks GitHub for the latest TacAudio release (one small HTTPS request, no data
// about this PC is sent beyond what any web request carries).

#include <atomic>
#include <filesystem>
#include <string>

namespace tac {

struct ReleaseInfo {
    std::string version;  // "0.7.0" (tag without the leading v)
    std::string url;      // release page
    std::string installerUrl;     // the release's TacAudio-Setup-*.exe (empty if none)
    std::string installerSha256;  // its SHA-256 as published by GitHub (lowercase hex; empty if none)
    bool canInstall() const { return !installerUrl.empty() && installerSha256.size() == 64; }
    std::string notes;    // release text (Markdown, from CHANGELOG.md)
};

// Latest published release. Throws std::runtime_error when offline / on unexpected answers.
ReleaseInfo fetchLatestRelease();

// true if version a is newer than b ("0.10.0" > "0.9.3"). Non-numeric parts compare as 0.
bool isNewerVersion(const std::string& a, const std::string& b);

// Downloads the release's installer to `file` and checks it against GitHub's SHA-256 (a broken
// or altered download is deleted and refused). progress goes 0..1. Throws std::runtime_error.
void downloadInstaller(const ReleaseInfo& r, const std::filesystem::path& file, std::atomic<float>& progress);

// SHA-256 of a file as lowercase hex ("" if it cannot be read).
std::string sha256File(const std::filesystem::path& file);

}  // namespace tac

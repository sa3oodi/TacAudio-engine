#pragma once
// Update check: asks GitHub for the latest TacAudio release (one small HTTPS request, no data
// about this PC is sent beyond what any web request carries).

#include <string>

namespace tac {

struct ReleaseInfo {
    std::string version;  // "0.7.0" (tag without the leading v)
    std::string url;      // release page
    std::string notes;    // release text (Markdown, from CHANGELOG.md)
};

// Latest published release. Throws std::runtime_error when offline / on unexpected answers.
ReleaseInfo fetchLatestRelease();

// true if version a is newer than b ("0.10.0" > "0.9.3"). Non-numeric parts compare as 0.
bool isNewerVersion(const std::string& a, const std::string& b);

}  // namespace tac

#pragma once
// Patch notes (CHANGELOG.md, built into the app, and GitHub release text): a small Markdown
// subset parsed into versions -> sections -> items.
//
//   ## 0.7.1 - 2026-11-20         a version (text after the version is its date / label)
//   ### New                       a section
//   - Something new.              an item (also "* ")
//
// Lines before the first "## " (a release text from GitHub has none) belong to an unnamed
// version. HTML comments, "# " titles and other headings are skipped. Inline Markdown is
// simplified for display: **bold**, `code` and [text](link) become plain text.

#include <string>
#include <vector>

namespace tac {

struct PatchSection {
    std::string title;               // "New", "Improved", "Fixed" ("" = no heading)
    std::vector<std::string> items;  // one line each; plain paragraphs are items too
};

struct PatchVersion {
    std::string version;  // "0.7.1" ("" = unnamed)
    std::string label;    // "2026-11-20" / "upcoming"
    std::vector<PatchSection> sections;
    bool empty() const noexcept;
};

std::vector<PatchVersion> parsePatchNotes(const std::string& markdown);

// Markdown inline -> plain text.
std::string plainText(const std::string& line);

}  // namespace tac

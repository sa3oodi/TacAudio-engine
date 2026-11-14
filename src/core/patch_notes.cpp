#include "core/patch_notes.h"

#include <cctype>

namespace tac {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

}  // namespace

bool PatchVersion::empty() const noexcept {
    for (const auto& s : sections)
        if (!s.items.empty()) return false;
    return true;
}

std::string plainText(const std::string& line) {
    std::string out;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '*' && i + 1 < line.size() && line[i + 1] == '*') {  // **bold**
            ++i;
            continue;
        }
        if (c == '`') continue;
        if (c == '[') {  // [text](link) -> text
            const auto close = line.find("](", i);
            const auto end = close == std::string::npos ? std::string::npos : line.find(')', close);
            if (end != std::string::npos) {
                out += line.substr(i + 1, close - i - 1);
                i = end;
                continue;
            }
        }
        out += c;
    }
    return trim(out);
}

std::vector<PatchVersion> parsePatchNotes(const std::string& md) {
    std::vector<PatchVersion> out;
    bool inComment = false;
    auto version = [&]() -> PatchVersion& {
        if (out.empty()) out.emplace_back();
        return out.back();
    };
    auto section = [&]() -> PatchSection& {
        auto& v = version();
        if (v.sections.empty()) v.sections.emplace_back();
        return v.sections.back();
    };
    std::size_t pos = 0;
    while (pos <= md.size()) {
        auto nl = md.find('\n', pos);
        if (nl == std::string::npos) nl = md.size();
        std::string line = trim(md.substr(pos, nl - pos));
        pos = nl + 1;
        if (inComment) {
            if (line.find("-->") != std::string::npos) inComment = false;
            continue;
        }
        if (startsWith(line, "<!--")) {
            inComment = line.find("-->") == std::string::npos;
            continue;
        }
        if (line.empty() || line == "---") continue;
        if (startsWith(line, "## ")) {
            PatchVersion v;
            std::string rest = trim(line.substr(3));
            if (!rest.empty() && (rest[0] == 'v' || rest[0] == 'V') && rest.size() > 1 &&
                std::isdigit(static_cast<unsigned char>(rest[1])))
                rest.erase(0, 1);
            const auto sp = rest.find_first_of(" \t");
            v.version = rest.substr(0, sp);
            if (sp != std::string::npos) {
                std::string label = trim(rest.substr(sp));
                while (!label.empty() && (label[0] == '-' || label[0] == '(' || static_cast<unsigned char>(label[0]) >= 0x80))
                    label = trim(label.substr(1));  // "- ", "(", an en dash...
                if (!label.empty() && label.back() == ')') label.pop_back();
                v.label = label;
            }
            out.push_back(std::move(v));
            continue;
        }
        if (startsWith(line, "### ")) {
            PatchSection s;
            s.title = plainText(line.substr(4));
            version().sections.push_back(std::move(s));
            continue;
        }
        if (line[0] == '#') continue;  // "# Title" and deeper headings
        if (startsWith(line, "- ") || startsWith(line, "* ")) line = line.substr(2);
        const std::string t = plainText(line);
        if (!t.empty()) section().items.push_back(t);
    }
    return out;
}

}  // namespace tac

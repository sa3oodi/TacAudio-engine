#include "update_check.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <windows.h>
#include <winhttp.h>

namespace tac {

namespace {
constexpr const wchar_t* kHost = L"api.github.com";
constexpr const wchar_t* kPath = L"/repos/sa3oodi/TacAudio-engine/releases/latest";

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string jsonField(const std::string& j, const std::string& key) {
    const auto k = j.find("\"" + key + "\"");
    if (k == std::string::npos) return {};
    auto q = j.find(':', k);
    if (q == std::string::npos) return {};
    q = j.find('"', q);
    if (q == std::string::npos) return {};
    std::string v;
    for (std::size_t i = q + 1; i < j.size() && j[i] != '"'; ++i) {
        if (j[i] == '\\' && i + 1 < j.size()) ++i;
        v += j[i];
    }
    return v;
}

std::vector<int> parts(const std::string& v) {
    std::vector<int> out;
    int cur = 0;
    bool any = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            out.push_back(cur);
            cur = 0;
            any = false;
        } else if (c == '-' || c == '+') {
            break;  // pre-release / build suffix: ignored
        }
    }
    if (any || !out.empty()) out.push_back(cur);
    return out;
}
}  // namespace

bool isNewerVersion(const std::string& a, const std::string& b) {
    const auto pa = parts(a), pb = parts(b);
    for (std::size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

ReleaseInfo fetchLatestRelease() {
    Handle session(WinHttpOpen(L"TacAudio-update-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) throw std::runtime_error("no internet access");
    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 8000);
    Handle conn(WinHttpConnect(session.h, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn.h) throw std::runtime_error("cannot reach GitHub");
    Handle req(WinHttpOpenRequest(conn.h, L"GET", kPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE));
    if (!req.h) throw std::runtime_error("cannot reach GitHub");
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(req.h, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        throw std::runtime_error("no answer from GitHub (offline?)");
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) throw std::runtime_error("GitHub answered " + std::to_string(status));
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req.h, chunk.data(), avail, &got) || got == 0) break;
        body.append(chunk.data(), got);
        if (body.size() > (1u << 20)) break;  // a release answer is a few kB
    }
    ReleaseInfo r;
    r.version = jsonField(body, "tag_name");
    if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
    r.url = jsonField(body, "html_url");
    if (r.version.empty()) throw std::runtime_error("unexpected answer from GitHub");
    if (r.url.rfind("https://github.com/", 0) != 0) r.url = "https://github.com/sa3oodi/TacAudio-engine/releases/latest";
    return r;
}

}  // namespace tac

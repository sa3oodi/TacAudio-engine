#include "update_check.h"

#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

namespace tac {

namespace {
constexpr const wchar_t* kHost = L"api.github.com";
constexpr const wchar_t* kPath = L"/repos/sa3oodi/TacAudio-engine/releases/latest";
constexpr const char* kDownloadPrefix = "https://github.com/sa3oodi/TacAudio-engine/releases/download/";

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
        if (j[i] != '\\' || i + 1 >= j.size()) {
            v += j[i];
            continue;
        }
        const char e = j[++i];
        switch (e) {
            case 'n': v += '\n'; break;
            case 'r': v += '\r'; break;
            case 't': v += '\t'; break;
            case 'b': case 'f': break;
            case 'u': {  // \uXXXX -> UTF-8 (surrogate pairs combined)
                auto hex = [&](std::size_t at) -> unsigned {
                    if (at + 4 > j.size()) return 0xFFFD;
                    return static_cast<unsigned>(std::strtoul(j.substr(at, 4).c_str(), nullptr, 16));
                };
                unsigned cp = hex(i + 1);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                    const unsigned lo = hex(i + 3);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                if (cp < 0x80) {
                    v += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    v += static_cast<char>(0xC0 | (cp >> 6));
                    v += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    v += static_cast<char>(0xE0 | (cp >> 12));
                    v += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    v += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    v += static_cast<char>(0xF0 | (cp >> 18));
                    v += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    v += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    v += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: v += e; break;  // \" \\ \/
        }
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
    r.notes = jsonField(body, "body");
    if (r.version.empty()) throw std::runtime_error("unexpected answer from GitHub");
    if (r.url.rfind("https://github.com/", 0) != 0) r.url = "https://github.com/sa3oodi/TacAudio-engine/releases/latest";
    // The installer asset (only from this project's own releases) and GitHub's checksum of it.
    const std::string asset = jsonField(body, "browser_download_url");
    if (asset.rfind(kDownloadPrefix, 0) == 0 && asset.size() > 4 && asset.compare(asset.size() - 4, 4, ".exe") == 0)
        r.installerUrl = asset;
    std::string digest = jsonField(body, "digest");
    if (digest.rfind("sha256:", 0) == 0) {
        digest.erase(0, 7);
        std::transform(digest.begin(), digest.end(), digest.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos) r.installerSha256 = digest;
    }
    return r;
}

std::string sha256File(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string out;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 16);
        bool ok = true;
        while (ok && f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = static_cast<ULONG>(f.gcount());
            if (n) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), n, 0) == 0;
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* hex = "0123456789abcdef";
            for (unsigned char b : digest) {
                out += hex[b >> 4];
                out += hex[b & 15];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

void downloadInstaller(const ReleaseInfo& r, const std::filesystem::path& file, std::atomic<float>& progress) {
    if (!r.canInstall()) throw std::runtime_error("this release has no installer to download");
    progress = 0.0f;
    std::wstring url(r.installerUrl.begin(), r.installerUrl.end());  // ASCII (checked prefix)
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
        throw std::runtime_error("unexpected download address");
    Handle session(WinHttpOpen(L"TacAudio-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) throw std::runtime_error("no internet access");
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn.h) throw std::runtime_error("cannot reach GitHub");
    Handle req(WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  WINHTTP_FLAG_SECURE));
    if (!req.h) throw std::runtime_error("cannot reach GitHub");
    // GitHub redirects release downloads to its file servers (HTTPS only - WinHTTP never follows
    // a redirect from HTTPS to HTTP by default).
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        throw std::runtime_error("download failed (offline?)");
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) throw std::runtime_error("download failed (GitHub answered " + std::to_string(status) + ")");
    DWORD total = 0;
    size = sizeof(total);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total,
                        &size, WINHTTP_NO_HEADER_INDEX);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write the download to the temp folder");
        std::vector<char> buf(1 << 16);
        unsigned long long got = 0;
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
                out.close();
                std::filesystem::remove(file, ec);
                throw std::runtime_error("download interrupted");
            }
            if (n == 0) break;
            out.write(buf.data(), n);
            got += n;
            if (got > (512ull << 20)) throw std::runtime_error("download too large");
            if (total) progress = static_cast<float>(static_cast<double>(got) / total) * 0.98f;
        }
        if (!out) throw std::runtime_error("cannot write the download to the temp folder");
    }
    if (sha256File(file) != r.installerSha256) {
        std::filesystem::remove(file, ec);
        throw std::runtime_error("the download is damaged (checksum does not match) - try again");
    }
    progress = 1.0f;
}

}  // namespace tac

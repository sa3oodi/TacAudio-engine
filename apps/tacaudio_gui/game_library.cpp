#include "game_library.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>

namespace fs = std::filesystem;

namespace tac {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string normDir(const std::wstring& dir) {
    if (dir.empty()) return {};
    std::error_code ec;
    std::wstring d = fs::weakly_canonical(fs::path(dir), ec).wstring();
    if (ec || d.empty()) d = dir;
    std::replace(d.begin(), d.end(), L'/', L'\\');
    std::transform(d.begin(), d.end(), d.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (d.back() != L'\\') d += L'\\';
    return toUtf8(d);
}

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Tokens of a Valve KeyValues (.vdf / .acf) file: the quoted strings, unescaped.
std::vector<std::string> vdfTokens(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string t;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            t += text[i];
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::string vdfValue(const std::vector<std::string>& tok, const std::string& key) {
    for (std::size_t i = 0; i + 1 < tok.size(); ++i)
        if (tok[i] == key) return tok[i + 1];
    return {};
}

// "Key": "value" in a JSON file (first occurrence), unescaped.
std::string jsonString(const std::string& text, const std::string& key) {
    const auto k = text.find("\"" + key + "\"");
    if (k == std::string::npos) return {};
    auto q = text.find(':', k);
    if (q == std::string::npos) return {};
    q = text.find('"', q);
    if (q == std::string::npos) return {};
    std::string v;
    for (std::size_t i = q + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        v += text[i];
    }
    return v;
}

bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string lowerA(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Largest .exe in the folder or one level below (skips installers / crash reporters).
std::wstring mainExe(const fs::path& dir) {
    std::wstring best;
    std::uintmax_t bestSize = 0;
    auto consider = [&](const fs::path& p) {
        const std::string n = lowerA(p.filename().string());
        if (p.extension() != ".exe" && p.extension() != ".EXE") return;
        for (const char* bad : {"unins", "crash", "setup", "redist", "report", "helper", "vc_", "dxsetup", "eac", "battleye"})
            if (n.find(bad) != std::string::npos) return;
        std::error_code ec;
        const auto sz = fs::file_size(p, ec);
        if (!ec && sz > bestSize) {
            bestSize = sz;
            best = p.wstring();
        }
    };
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (e.is_regular_file(ec)) consider(e.path());
        else if (e.is_directory(ec))
            for (const auto& s : fs::directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec))
                if (s.is_regular_file(ec)) consider(s.path());
    }
    return best;
}

// Steam's locally cached library art for an app: portrait cover first, header (landscape) second.
void steamCover(const fs::path& cache, const std::string& appid, GameEntry& g) {
    const fs::path flat = cache / (appid + "_library_600x900.jpg");
    if (isFile(flat)) {
        g.cover = flat.wstring();
        return;
    }
    const fs::path sub = cache / appid;
    std::wstring header;
    if (isDir(sub)) {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(sub, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file(ec)) continue;
            const std::string n = lowerA(e.path().filename().string());
            if (n.rfind("library_600x900", 0) == 0 || n.rfind("library_capsule", 0) == 0) {
                g.cover = e.path().wstring();
                return;
            }
            if ((n.rfind("header", 0) == 0 || n.rfind("library_header", 0) == 0) && header.empty()) header = e.path().wstring();
        }
    }
    if (header.empty() && isFile(cache / (appid + "_header.jpg"))) header = (cache / (appid + "_header.jpg")).wstring();
    if (!header.empty()) {
        g.cover = header;
        g.coverIsWide = true;
    }
}

bool skipSteamApp(const std::string& name, const std::string& appid) {
    if (appid == "228980") return true;  // Steamworks Common Redistributables
    const std::string n = lowerA(name);
    for (const char* bad : {"redistributable", "proton", "steam linux runtime", "soundtrack", "dedicated server", " sdk",
                            "steamvr", "wallpaper engine"})
        if (n.find(bad) != std::string::npos) return true;
    return false;
}

void scanSteam(std::vector<GameEntry>& out) {
    HKEY k;
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return;
    const bool ok = RegQueryValueExW(k, L"SteamPath", nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS;
    RegCloseKey(k);
    if (!ok) return;
    const fs::path steam(buf);
    const fs::path cache = steam / "appcache" / "librarycache";

    std::vector<fs::path> libs = {steam};
    const auto tok = vdfTokens(readFile(steam / "steamapps" / "libraryfolders.vdf"));
    for (std::size_t i = 0; i + 1 < tok.size(); ++i)
        if (tok[i] == "path") {
            const fs::path p(fromUtf8(tok[i + 1]));
            if (std::find(libs.begin(), libs.end(), p) == libs.end()) libs.push_back(p);
        }

    for (const auto& lib : libs) {
        const fs::path apps = lib / "steamapps";
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(apps, fs::directory_options::skip_permission_denied, ec)) {
            const std::string fn = e.path().filename().string();
            if (fn.rfind("appmanifest_", 0) != 0 || e.path().extension() != ".acf") continue;
            const auto t = vdfTokens(readFile(e.path()));
            const std::string appid = vdfValue(t, "appid"), name = vdfValue(t, "name"), dir = vdfValue(t, "installdir");
            if (appid.empty() || name.empty() || dir.empty() || skipSteamApp(name, appid)) continue;
            const fs::path install = apps / "common" / fromUtf8(dir);
            if (!isDir(install)) continue;
            GameEntry g;
            g.name = name;
            g.source = "Steam";
            g.dir = install.wstring();
            g.dirLower = normDir(g.dir);
            steamCover(cache, appid, g);
            if (g.cover.empty()) g.exe = mainExe(install);
            out.push_back(std::move(g));
        }
    }
}

void scanEpic(std::vector<GameEntry>& out) {
    PWSTR pd = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) base = pd;
    CoTaskMemFree(pd);
    const fs::path manifests = base / "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(manifests, fs::directory_options::skip_permission_denied, ec)) {
        if (e.path().extension() != ".item") continue;
        const std::string j = readFile(e.path());
        const std::string name = jsonString(j, "DisplayName"), loc = jsonString(j, "InstallLocation"),
                          exe = jsonString(j, "LaunchExecutable");
        if (name.empty() || loc.empty()) continue;
        if (j.find("\"AppCategories\"") != std::string::npos && j.find("\"games\"") == std::string::npos) continue;
        if (lowerA(name).rfind("unreal engine", 0) == 0) continue;
        const fs::path install(fromUtf8(loc));
        if (!isDir(install)) continue;
        GameEntry g;
        g.name = name;
        g.source = "Epic Games";
        g.dir = install.wstring();
        g.dirLower = normDir(g.dir);
        if (!exe.empty() && isFile(install / fromUtf8(exe))) g.exe = (install / fromUtf8(exe)).wstring();
        else g.exe = mainExe(install);
        out.push_back(std::move(g));
    }
}

}  // namespace

std::vector<GameEntry> scanGameLibrary() {
    std::vector<GameEntry> v;
    try {
        scanSteam(v);
    } catch (const std::exception&) {
    }
    try {
        scanEpic(v);
    } catch (const std::exception&) {
    }
    // The same Steam library can be listed twice (different spellings of one folder).
    std::vector<GameEntry> unique;
    for (auto& g : v)
        if (std::none_of(unique.begin(), unique.end(), [&](const GameEntry& u) { return u.dirLower == g.dirLower; }))
            unique.push_back(std::move(g));
    std::sort(unique.begin(), unique.end(), [](const GameEntry& a, const GameEntry& b) { return lowerA(a.name) < lowerA(b.name); });
    return unique;
}

// ------------------------------------------------------------------ images
Image loadImageFile(const std::wstring& path, int maxW, int maxH) {
    Image img;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return img;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h) {
        const double s = std::min({1.0, static_cast<double>(maxW) / w, static_cast<double>(maxH) / h});
        const UINT tw = std::max(1u, static_cast<UINT>(w * s)), th = std::max(1u, static_cast<UINT>(h * s));
        if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            img.w = static_cast<int>(tw);
            img.h = static_cast<int>(th);
            img.rgba.resize(static_cast<std::size_t>(tw) * th * 4);
            if (FAILED(conv->CopyPixels(nullptr, tw * 4, static_cast<UINT>(img.rgba.size()), img.rgba.data()))) img = {};
        }
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    f->Release();
    return img;
}

Image loadExeIcon(const std::wstring& exe, int size) {
    Image img;
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(exe.c_str(), 0, 0, &icon, nullptr, static_cast<UINT>(MAKELONG(size, 16)))) || !icon)
        return img;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        std::memset(bits, 0, static_cast<std::size_t>(size) * size * 4);
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        GdiFlush();
        img.w = img.h = size;
        img.rgba.resize(static_cast<std::size_t>(size) * size * 4);
        const auto* src = static_cast<const std::uint8_t*>(bits);
        bool anyAlpha = false;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4) anyAlpha |= src[i + 3] != 0;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
            img.rgba[i] = src[i + 2];
            img.rgba[i + 1] = src[i + 1];
            img.rgba[i + 2] = src[i];
            img.rgba[i + 3] = anyAlpha ? src[i + 3] : ((src[i] | src[i + 1] | src[i + 2]) ? 255 : 0);
        }
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(icon);
    return img;
}

}  // namespace tac

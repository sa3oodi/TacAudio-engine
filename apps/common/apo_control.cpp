#include "apo_control.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "cli.h"

namespace tac::apoctl {

namespace {
constexpr wchar_t kClsid[] = L"{1A057EE8-C9EB-451B-9A59-F6D6FC460F7A}";
constexpr const wchar_t* kSlots[] = {L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"};

std::wstring readString(const std::wstring& key, const wchar_t* name) {
    wchar_t buf[256];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}
}  // namespace

bool attachedTo(const std::wstring& endpointId) {
    const auto dot = endpointId.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring fx = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" +
                            endpointId.substr(dot + 1) + L"\\FxProperties";
    for (const wchar_t* slot : kSlots)
        if (_wcsicmp(readString(fx, slot).c_str(), kClsid) == 0) return true;
    return false;
}

bool dllInstalled() {
    HKEY k;
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

std::filesystem::path dataDir() {
    PWSTR pd = nullptr;
    std::filesystem::path p = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) p = pd;
    CoTaskMemFree(pd);
    return p / L"TacAudio";
}

bool writeSettings(const std::string& iniText) {
    const auto tmp = dataDir() / L"apo.ini.tmp", dst = dataDir() / L"apo.ini";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << iniText;
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string readSetting(const std::string& key, const std::string& def) {
    std::ifstream f(dataDir() / L"apo.ini");
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
    }
    return def;
}

std::optional<Status> readStatus() {
    std::ifstream f(dataDir() / L"apo_status.ini");
    if (!f) return std::nullopt;
    Status s;
    unsigned long long t = 0;
    try {
        for (std::string line; std::getline(f, line);) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "time") t = std::stoull(v);
            else if (k == "streams") s.streams = std::stoi(v);
            else if (k == "channels") s.channels = static_cast<unsigned>(std::stoul(v));
            else if (k == "outL") s.outLDb = std::stof(v);
            else if (k == "outR") s.outRDb = std::stof(v);
            else if (k == "dynMin") s.dynMinDb = std::stof(v);
            else if (k == "dynMax") s.dynMaxDb = std::stof(v);
            else if (k == "in") {
                std::istringstream is(v);
                s.inDb.fill(-120.0f);
                for (std::size_t c = 0; c < 8 && is >> s.inDb[c]; ++c) {
                }
            }
        }
    } catch (const std::exception&) {
        return std::nullopt;  // caught mid-write
    }
    const unsigned long long now = GetTickCount64();
    if (t == 0 || now < t || now - t > 1500) return std::nullopt;
    return s;
}

int runSetupElevated(const std::vector<std::wstring>& args, const std::filesystem::path& logFile) {
    const std::filesystem::path exe = cli::exeDir() / L"tacapo-setup.exe";
    std::wstring params;
    for (const auto& a : args) params += L"\"" + a + L"\" ";
    params += L"--log \"" + logFile.wstring() + L"\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(code);
}

}  // namespace tac::apoctl

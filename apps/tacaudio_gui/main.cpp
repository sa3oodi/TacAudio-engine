// TacAudio GUI — Win32 + Direct3D 11 host for the Dear ImGui app (see app.cpp / ui_widgets.cpp).
//
//   tacaudio_gui.exe                                   normal start
//   tacaudio_gui.exe --smoke-test [--page N] [--preview]
//                                                      hidden window, render, save
//                                                      %TEMP%\tacaudio_gui_smoke.bmp, exit (no screen needed)
//   tacaudio_gui.exe --session-test "<output device>"  headless audio session check
//                                                      (writes %TEMP%\tacaudio_session_test.txt)

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dwmapi.h>
#include <windows.h>

#include "app.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "platform/wasapi.h"
#include "session.h"
#include "ui_widgets.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;
using namespace tac;

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resizeW = 0, g_resizeH = 0;

void createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}
void cleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}
bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swapChain, &g_device, &got, &g_context);
    if (FAILED(hr)) return false;
    createRenderTarget();
    return true;
}
void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
}

// ------------------------------------------------------------------ tray, hotkeys, single instance
constexpr UINT WM_TRAY = WM_APP + 1;      // tray icon callback
constexpr UINT WM_SHOWAPP = WM_APP + 2;   // sent by a second instance: bring the window forward
constexpr int kHotPower = 1, kHot3d = 2;
enum : UINT { kMenuOpen = 100, kMenuPower, kMenu3d, kMenuExit };

App* g_app = nullptr;
HWND g_hwnd = nullptr;
bool g_visible = true;
bool g_quitting = false;
bool g_hotkeysOn = false;
NOTIFYICONDATAW g_tray{};

void showWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    g_visible = true;
}
void hideWindow() {
    ShowWindow(g_hwnd, SW_HIDE);
    g_visible = false;
}

void trayAdd(HINSTANCE inst) {
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                 GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
    wcscpy_s(g_tray.szTip, L"TacAudio");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}
void traySetTip(const std::string& tip) {
    wchar_t w[128];
    MultiByteToWideChar(CP_UTF8, 0, tip.c_str(), -1, w, 128);
    if (wcscmp(w, g_tray.szTip) == 0) return;
    wcscpy_s(g_tray.szTip, w);
    g_tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}
void trayBalloon(const wchar_t* title, const wchar_t* text) {
    g_tray.uFlags = NIF_INFO;
    wcscpy_s(g_tray.szInfoTitle, title);
    wcscpy_s(g_tray.szInfo, text);
    g_tray.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}
void trayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kMenuOpen, L"Open TacAudio");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_app && g_app->engineWanted() ? MF_CHECKED : 0), kMenuPower, L"TacAudio on");
    AppendMenuW(m, MF_STRING | (g_app && g_app->enabled3d() ? MF_CHECKED : 0), kMenu3d, L"3D sound");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuExit, L"Exit");
    SetMenuDefaultItem(m, kMenuOpen, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);  // so the menu closes when clicking elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}
void syncHotkeys() {
    const bool want = g_app && g_app->hotkeysEnabled();
    if (want == g_hotkeysOn) return;
    if (want) {
        RegisterHotKey(g_hwnd, kHotPower, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F10);
        RegisterHotKey(g_hwnd, kHot3d, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F9);
    } else {
        UnregisterHotKey(g_hwnd, kHotPower);
        UnregisterHotKey(g_hwnd, kHot3d);
    }
    g_hotkeysOn = want;
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) showWindow();
            else if (LOWORD(lParam) == WM_RBUTTONUP) trayMenu();
            return 0;
        case WM_SHOWAPP:
            showWindow();
            return 0;
        case WM_HOTKEY:
            if (g_app && wParam == kHotPower) g_app->togglePower();
            if (g_app && wParam == kHot3d) g_app->toggle3d();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kMenuOpen: showWindow(); break;
                case kMenuPower: if (g_app) g_app->togglePower(); break;
                case kMenu3d: if (g_app) g_app->toggle3d(); break;
                case kMenuExit:
                    g_quitting = true;
                    DestroyWindow(hWnd);
                    break;
            }
            return 0;
        case WM_CLOSE:
            if (!g_quitting && g_app && g_app->closeToTray()) {
                static bool told = false;
                hideWindow();
                if (!told) {
                    trayBalloon(L"TacAudio is still running", L"It keeps working in the tray. Right-click the icon to exit.");
                    told = true;
                }
                return 0;
            }
            DestroyWindow(hWnd);
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeW = LOWORD(lParam);
            g_resizeH = HIWORD(lParam);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            mm->ptMinTrackSize.x = static_cast<LONG>(1020 * ui::dpi);
            mm->ptMinTrackSize.y = static_cast<LONG>(720 * ui::dpi);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Copies the back buffer to a 32-bit BMP (lets the layout be reviewed without a screen).
void saveScreenshot(const fs::path& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &staging))) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = static_cast<LONG>(d.Width);
            ih.biHeight = -static_cast<LONG>(d.Height);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            const DWORD bytes = d.Width * d.Height * 4;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + bytes;
            std::ofstream f(path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
            f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
            std::vector<unsigned char> row(d.Width * 4);
            for (UINT y = 0; y < d.Height; ++y) {
                const auto* src = static_cast<const unsigned char*>(m.pData) + y * m.RowPitch;
                for (UINT x = 0; x < d.Width; ++x) {
                    row[x * 4 + 0] = src[x * 4 + 2];
                    row[x * 4 + 1] = src[x * 4 + 1];
                    row[x * 4 + 2] = src[x * 4 + 0];
                    row[x * 4 + 3] = 255;
                }
                f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
            }
            g_context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

std::string narrow(const std::wstring& w) {
    std::string s(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// Headless check of the audio session the GUI drives: start, live rebuilds, status, stop.
int sessionTest(const std::wstring& cmd) {
    std::wstring dev = cmd.substr(cmd.find(L"--session-test") + 14);
    dev.erase(0, dev.find_first_not_of(L" \""));
    dev.erase(dev.find_last_not_of(L" \"") + 1);
    std::ofstream log(fs::temp_directory_path() / "tacaudio_session_test.txt");
    wasapi::ComScope com;
    AudioSession s;
    DeviceSettings d;
    d.output = narrow(dev);
    SoundSettings snd;
    snd.volumeDb = -60.0f;  // effectively silent on the test device
    bool ok = true;
    auto ms = [](auto t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    try {
        s.start(d, snd);
        log << "started: running=" << s.running() << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
        SoundSettings b = snd;
        b.room = "spacious";
        b.footstepsDb = 6.0f;
        auto t0 = std::chrono::steady_clock::now();
        s.apply(b);
        log << "apply room+footsteps: " << ms(t0) << " ms\n";
        b.head = "ku100";
        t0 = std::chrono::steady_clock::now();
        s.apply(b);
        log << "apply new head (load): " << ms(t0) << " ms\n";
        b.head = "kemar";
        t0 = std::chrono::steady_clock::now();
        s.apply(b);
        log << "apply cached head: " << ms(t0) << " ms\n";
        s.set3d(false);
        s.setVolumeDb(-50.0f);
        float l = 0, r = 0;
        s.meters(l, r);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const SessionStatus st = s.status();
        log << "status: running=" << st.running << " error='" << st.error << "' latency=" << st.latencyMs
            << " ms underruns=" << st.engine.underruns << " outLate=" << st.outLate << "\nsource=" << st.source
            << "\noutput=" << st.output << "\n";
        ok = st.running && st.error.empty();
        s.stop();
        log << "stopped: running=" << s.running() << "\n";
        ok = ok && !s.running();
    } catch (const std::exception& e) {
        log << "error: " << e.what() << "\n";
        ok = false;
    }
    log << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring cmd = cmdLine ? cmdLine : L"";
    if (cmd.find(L"--session-test") != std::wstring::npos) return sessionTest(cmd);
    const bool smoke = cmd.find(L"--smoke-test") != std::wstring::npos;
    const bool preview = cmd.find(L"--preview") != std::wstring::npos;
    int page = 0;
    if (const auto p = cmd.find(L"--page "); p != std::wstring::npos) page = _wtoi(cmd.c_str() + p + 7);
    const bool startHidden = cmd.find(L"--minimized") != std::wstring::npos;

    // Single instance: a second launch just brings the running TacAudio forward.
    HANDLE instanceMutex = nullptr;
    if (!smoke) {
        instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\TacAudio.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND other = FindWindowW(L"TacAudioGui", nullptr)) PostMessageW(other, WM_SHOWAPP, 0, 0);
            if (instanceMutex) CloseHandle(instanceMutex);
            return 0;
        }
    }

    wasapi::ComScope com;
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    ui::dpi = dpi;

    HICON bigIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    HICON smallIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                    GetSystemMetrics(SM_CYSMICON), 0));
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, hInst, bigIcon, LoadCursor(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"TacAudioGui", smallIcon};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"TacAudio", WS_OVERLAPPEDWINDOW, 80, 60, static_cast<int>(1200 * dpi),
                              static_cast<int>(920 * dpi), nullptr, nullptr, hInst, nullptr);
    g_hwnd = hwnd;
    // Dark title bar matching the app (Windows 11; ignored where unsupported).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF caption = RGB(19, 22, 29);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));

    if (!createDevice(hwnd)) {
        cleanupDevice();
        UnregisterClassW(wc.lpszClassName, hInst);
        return 1;
    }
    g_visible = !(smoke || startHidden);
    ShowWindow(hwnd, g_visible ? SW_SHOWDEFAULT : SW_HIDE);
    UpdateWindow(hwnd);
    if (!smoke) trayAdd(hInst);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ui::setup(dpi);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    ui::createTexture = [](const unsigned char* rgba, int w, int h) -> ImTextureID {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w);
        d.Height = static_cast<UINT>(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{rgba, static_cast<UINT>(w) * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_device->CreateTexture2D(&d, &init, &tex))) return 0;
        ID3D11ShaderResourceView* view = nullptr;
        g_device->CreateShaderResourceView(tex, nullptr, &view);
        tex->Release();
        return reinterpret_cast<ImTextureID>(view);
    };

    int result = 0;
    {
        App app(smoke && preview);
        g_app = &app;
        app.setPage(page);
        if (!smoke) app.autoStartIfEnabled();
        int frames = 0;
        char sf[16] = {};  // TACAUDIO_SMOKE_FRAMES: render longer (e.g. to let the game library load)
        GetEnvironmentVariableA("TACAUDIO_SMOKE_FRAMES", sf, sizeof(sf));
        const int smokeFrames = sf[0] ? std::max(2, atoi(sf)) : 40;
        bool done = false;
        auto lastTip = std::chrono::steady_clock::now();
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (!smoke) {
                syncHotkeys();
                if (std::chrono::steady_clock::now() - lastTip > std::chrono::seconds(1)) {
                    traySetTip(app.trayTooltip());
                    lastTip = std::chrono::steady_clock::now();
                }
            }
            if (!g_visible && !smoke) {  // tray mode: no rendering, just keep the engine supervised
                MsgWaitForMultipleObjects(0, nullptr, FALSE, app.wantsFastTick() ? 30 : 100, QS_ALLINPUT);
                app.tick();
                continue;
            }
            if (smoke && ++frames > smokeFrames) break;  // enough frames for animations to settle
            if (g_resizeW) {
                cleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeW = g_resizeH = 0;
                createRenderTarget();
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            const float clear[4] = {0.05f, 0.06f, 0.08f, 1.0f};
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (smoke && frames == smokeFrames) saveScreenshot(fs::temp_directory_path() / "tacaudio_gui_smoke.bmp");
            g_swapChain->Present(1, 0);
        }
        g_app = nullptr;
    }

    if (!smoke) Shell_NotifyIconW(NIM_DELETE, &g_tray);
    if (g_hotkeysOn) {
        UnregisterHotKey(hwnd, kHotPower);
        UnregisterHotKey(hwnd, kHot3d);
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    if (instanceMutex) CloseHandle(instanceMutex);
    return result;
}

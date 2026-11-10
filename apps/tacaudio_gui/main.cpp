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

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
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

    wasapi::ComScope com;
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    ui::dpi = dpi;

    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"TacAudioGui", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"TacAudio", WS_OVERLAPPEDWINDOW, 80, 60, static_cast<int>(1200 * dpi),
                              static_cast<int>(880 * dpi), nullptr, nullptr, hInst, nullptr);
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
    ShowWindow(hwnd, smoke ? SW_HIDE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ui::setup(dpi);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    int result = 0;
    {
        App app(smoke && preview);
        app.setPage(page);
        int frames = 0;
        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (smoke && ++frames > 40) break;  // enough frames for animations to settle
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
            if (smoke && frames == 40) saveScreenshot(fs::temp_directory_path() / "tacaudio_gui_smoke.bmp");
            g_swapChain->Present(1, 0);
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return result;
}

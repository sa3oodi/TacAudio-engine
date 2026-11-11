#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>

namespace tac {

// ------------------------------------------------------------------ direction field
void DirectionField::update(const float* levels, const float* azimuth, const bool* lfe, std::size_t n, float dt) {
    // Loudest non-LFE channel sets the reference; channels within 24 dB of it contribute, so the
    // ring shows where sound comes from rather than how loud the game is overall.
    float maxDb = -120.0f;
    std::array<float, 8> db{};
    for (std::size_t c = 0; c < n && c < 8; ++c) {
        db[c] = levels[c] > 1e-6f ? 20.0f * std::log10(levels[c]) : -120.0f;
        if (!lfe[c]) maxDb = std::max(maxDb, db[c]);
    }
    const float presence = std::clamp((maxDb + 60.0f) / 20.0f, 0.0f, 1.0f);  // fades in from -60 dBFS
    std::array<float, kBins> target{};
    for (std::size_t c = 0; c < n && c < 8; ++c) {
        if (lfe[c]) continue;
        const float w = std::clamp((db[c] - (maxDb - 24.0f)) / 24.0f, 0.0f, 1.0f) * presence;
        if (w <= 0.0f) continue;
        for (int d = 0; d < kBins; ++d) {
            const float diff = (static_cast<float>(d) - azimuth[c]) * std::numbers::pi_v<float> / 180.0f;
            const float lobe = std::max(0.0f, std::cos(diff));
            target[static_cast<std::size_t>(d)] += w * std::pow(lobe, 10.0f);  // ~45 degrees wide
        }
    }
    const float att = 1.0f - std::exp(-dt / 0.03f), rel = 1.0f - std::exp(-dt / 0.35f);
    for (int d = 0; d < kBins; ++d) {
        float& v = v_[static_cast<std::size_t>(d)];
        const float t = std::min(1.0f, target[static_cast<std::size_t>(d)]);
        v += (t - v) * (t > v ? att : rel);
    }
}

void DirectionField::updateWeights(const float* weights, const float* azimuth, const bool* lfe, std::size_t n, float dt) {
    std::array<float, kBins> target{};
    for (std::size_t c = 0; c < n && c < 8; ++c) {
        const float w = std::clamp(weights[c], 0.0f, 1.0f);
        if (lfe[c] || w <= 0.0f) continue;
        for (int d = 0; d < kBins; ++d) {
            const float diff = (static_cast<float>(d) - azimuth[c]) * std::numbers::pi_v<float> / 180.0f;
            target[static_cast<std::size_t>(d)] += w * std::pow(std::max(0.0f, std::cos(diff)), 10.0f);
        }
    }
    const float att = 1.0f - std::exp(-dt / 0.04f), rel = 1.0f - std::exp(-dt / 0.5f);
    for (int d = 0; d < kBins; ++d) {
        float& v = v_[static_cast<std::size_t>(d)];
        const float t = std::min(1.0f, target[static_cast<std::size_t>(d)]);
        v += (t - v) * (t > v ? att : rel);
    }
}

void DirectionField::decay(float dt) {
    const float rel = 1.0f - std::exp(-dt / 0.35f);
    for (auto& v : v_) v -= v * rel;
}

float DirectionField::peak() const { return *std::max_element(v_.begin(), v_.end()); }

// ------------------------------------------------------------------ overlay window
namespace {
LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

SoundOverlay::~SoundOverlay() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (bmp_) DeleteObject(bmp_);
    if (dc_) DeleteDC(dc_);
}

void SoundOverlay::ensureWindow(int size) {
    if (hwnd_ && size == size_) return;
    if (!hwnd_) {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = overlayProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"TacAudioOverlay";
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"TacAudio sound direction", WS_POPUP, 0, 0, size, size, nullptr, nullptr,
                                inst, nullptr);
        dc_ = CreateCompatibleDC(nullptr);
    }
    if (bmp_) DeleteObject(bmp_);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    SelectObject(dc_, bmp_);
    size_ = size;
}

int SoundOverlay::draw(const DirectionField& field, int radius, float opacity) {
    const int thick = std::max(6, radius / 12);
    const int size = 2 * (radius + 3 * thick);
    ensureWindow(size);
    if (!hwnd_ || !bits_) return 0;

    // Draw: a faint guide ring, the glow (orange, brighter = louder), and a front tick.
    auto* px = static_cast<std::uint32_t*>(bits_);
    const float c = static_cast<float>(size) * 0.5f;
    const float R = static_cast<float>(radius), T = static_cast<float>(thick);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - c, dy = static_cast<float>(y) + 0.5f - c;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float d = std::abs(r - R);
            std::uint32_t out = 0;
            if (d < 2.5f * T) {
                int deg = static_cast<int>(std::lround(std::atan2(dx, -dy) * 180.0f / std::numbers::pi_v<float>));
                const float v = field.at(deg);
                const float core = std::clamp(1.0f - d / T, 0.0f, 1.0f);            // solid ring
                const float glow = std::clamp(1.0f - d / (2.5f * T), 0.0f, 1.0f);   // soft halo
                const float guide = std::clamp(1.0f - std::abs(r - R) / 1.5f, 0.0f, 1.0f) * 0.22f;
                float a = std::max(guide, v * (0.75f * core + 0.35f * glow * glow));
                if (std::abs(dx) < 1.5f && dy < 0 && r > R + 0.8f * T && r < R + 2.2f * T) a = std::max(a, 0.6f);  // front notch
                a = std::clamp(a * opacity, 0.0f, 1.0f);
                // orange glow over a light guide colour
                const float mix = std::clamp(v * 1.5f, 0.0f, 1.0f);
                const float rr = 255.0f, gg = 230.0f - 108.0f * mix, bb = 210.0f - 184.0f * mix;
                const auto A = static_cast<std::uint32_t>(a * 255.0f + 0.5f);
                const auto Rr = static_cast<std::uint32_t>(rr * a + 0.5f), G = static_cast<std::uint32_t>(gg * a + 0.5f),
                           B = static_cast<std::uint32_t>(bb * a + 0.5f);  // premultiplied
                out = (A << 24) | (Rr << 16) | (G << 8) | B;
            }
            px[y * size + x] = out;
        }
    }
    return size;
}

void SoundOverlay::saveBitmap(const DirectionField& field, int radius, float opacity, const wchar_t* path) {
    const int size = draw(field, radius, opacity);
    if (!size) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = size;
    ih.biHeight = -size;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    const DWORD bytes = static_cast<DWORD>(size) * size * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + bytes;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bits_, bytes, 1, f);
    fclose(f);
}

void SoundOverlay::show(const DirectionField& field, int radius, float opacity, Position where) {
    const int size = draw(field, radius, opacity);
    if (!size) return;

    // Main screen; corners keep a small margin from the edges.
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& m = mi.rcMonitor;
    const int margin = std::max(16, size / 10);
    POINT pos{(m.left + m.right) / 2 - size / 2, (m.top + m.bottom) / 2 - size / 2};
    switch (where) {
        case Position::TopLeft: pos = {m.left + margin, m.top + margin}; break;
        case Position::TopRight: pos = {m.right - size - margin, m.top + margin}; break;
        case Position::BottomLeft: pos = {m.left + margin, m.bottom - size - margin}; break;
        case Position::BottomRight: pos = {m.right - size - margin, m.bottom - size - margin}; break;
        case Position::Center: break;
    }
    SIZE sz{size, size};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &pos, &sz, dc_, &src, 0, &bf, ULW_ALPHA);
    if (!visible_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
    static int n = 0;
    if (++n % 30 == 1)  // stay above windows that became topmost meanwhile (e.g. a borderless game)
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SoundOverlay::hide() {
    if (hwnd_ && visible_) ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

}  // namespace tac

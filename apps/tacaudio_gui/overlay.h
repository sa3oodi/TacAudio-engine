#pragma once
// Sound direction overlay (accessibility): a ring in the middle of the screen that glows towards
// where game sounds come from, computed from the level of each 7.1 channel. Click-through,
// always on top, never takes focus. Visible over windowed / borderless games (not over
// exclusive fullscreen).
//
// It only reads the game's audio levels - nothing inside the game.

#include <array>
#include <cstddef>

#include <windows.h>

namespace tac {

// Ring brightness per degree (0 = front, clockwise, + = right) from channel levels.
class DirectionField {
public:
    static constexpr int kBins = 360;
    // levels: linear peaks; azimuth: degrees (+ = right); lfe channels are ignored. dt: seconds
    // since the last update (for smoothing).
    void update(const float* levels, const float* azimuth, const bool* lfe, std::size_t n, float dt);
    void decay(float dt);  // no data: fade out
    float at(int deg) const { return v_[static_cast<std::size_t>(((deg % kBins) + kBins) % kBins)]; }
    float peak() const;

private:
    std::array<float, kBins> v_{};
};

class SoundOverlay {
public:
    SoundOverlay() = default;
    ~SoundOverlay();
    SoundOverlay(const SoundOverlay&) = delete;
    SoundOverlay& operator=(const SoundOverlay&) = delete;

    // radius in px (ring), opacity 0..1. Creates the window on first call; call from the UI thread.
    void show(const DirectionField& field, int radius, float opacity);
    void hide();
    // Dev check: draws the ring like show() and saves it as a 32-bit BMP, without showing a window.
    void saveBitmap(const DirectionField& field, int radius, float opacity, const wchar_t* path);

private:
    void ensureWindow(int size);
    int draw(const DirectionField& field, int radius, float opacity);  // returns the bitmap size
    HWND hwnd_ = nullptr;
    int size_ = 0;
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    void* bits_ = nullptr;
    bool visible_ = false;
};

}  // namespace tac

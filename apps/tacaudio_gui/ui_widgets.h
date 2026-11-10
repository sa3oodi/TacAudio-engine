#pragma once
// TacAudio UI kit on top of Dear ImGui: theme, fonts, icons and custom-drawn widgets
// (cards, toggle switches, slim sliders with value box + reset, segmented controls,
// meters, stat tiles, glowing graphs). All sizes are in logical pixels and scaled by dpi.

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace ui {

// ------------------------------------------------------------------ theme
namespace col {
inline constexpr ImU32 Bg0 = IM_COL32(13, 15, 20, 255);        // window background
inline constexpr ImU32 Bg1 = IM_COL32(19, 22, 29, 255);        // sidebar
inline constexpr ImU32 Card = IM_COL32(27, 31, 40, 255);
inline constexpr ImU32 CardHi = IM_COL32(33, 38, 49, 255);
inline constexpr ImU32 Line = IM_COL32(44, 50, 62, 255);
inline constexpr ImU32 Well = IM_COL32(16, 19, 25, 255);       // inset fields / tracks
inline constexpr ImU32 Text = IM_COL32(232, 235, 241, 255);
inline constexpr ImU32 Dim = IM_COL32(138, 147, 166, 255);
inline constexpr ImU32 Faint = IM_COL32(86, 94, 110, 255);
inline constexpr ImU32 Accent = IM_COL32(255, 122, 26, 255);   // TacAudio orange
inline constexpr ImU32 AccentHi = IM_COL32(255, 150, 70, 255);
inline constexpr ImU32 Teal = IM_COL32(47, 227, 196, 255);     // meters, graphs
inline constexpr ImU32 Yellow = IM_COL32(245, 200, 66, 255);
inline constexpr ImU32 Red = IM_COL32(255, 84, 84, 255);
inline constexpr ImU32 Green = IM_COL32(64, 210, 120, 255);
}  // namespace col

ImU32 withAlpha(ImU32 c, float a);

// ------------------------------------------------------------------ fonts & icons
struct Fonts {
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
};
extern Fonts fonts;
extern float dpi;
inline float S(float v) { return v * dpi; }

void setup(float dpiScale);  // fonts + style; call after ImGui::CreateContext()

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) glyphs, as UTF-8.
namespace icon {
extern const char* Home;
extern const char* Audio;
extern const char* Headphone;
extern const char* Devices;
extern const char* Gauge;
extern const char* Info;
extern const char* Refresh;
extern const char* Reset;
extern const char* Power;
extern const char* Volume;
extern const char* Settings;
}  // namespace icon

// ------------------------------------------------------------------ text helpers
void text(const char* s, ImU32 color = col::Text, float size = 15.0f, bool bold = false);
void textUpper(const char* s, ImU32 color = col::Dim, float size = 12.5f);  // spaced capitals
void tooltip(const char* s);

// ------------------------------------------------------------------ widgets
bool toggle(const char* id, bool* v);

// Card with a header (optional toggle, title, optional help). Content is disabled while the
// toggle is off. width 0 = fill. Always pair with endCard().
void beginCard(const char* id, const char* title, bool* enabled = nullptr, float width = 0.0f,
               const char* help = nullptr);
bool cardToggleChanged();  // true if the card toggle changed this frame (call after beginCard)
void endCard();

enum class Edit { None, Changing, Commit };  // Commit = released after dragging, or reset

// Label | slider | value box | reset. `def` is the reset target.
Edit sliderRow(const char* label, float* v, float min, float max, const char* fmt, float def,
               const char* help = nullptr, ImU32 fill = col::Accent);
// Label | segmented buttons | reset. Returns true when the selection changed.
bool segmentedRow(const char* label, int* index, const std::vector<std::string>& items, int def,
                  const char* help = nullptr);
// Label | dropdown | reset (reset hidden when def is empty).
bool comboRow(const char* label, std::string* value, const std::vector<std::string>& items,
              const std::vector<std::string>* itemHelp, const std::string& def, const char* help = nullptr);
// Label | toggle | description | reset. Returns true when the value changed.
bool toggleRow(const char* label, bool* v, const char* description, bool def);
// Small round icon button. Returns true when clicked.
bool iconButton(const char* id, const char* glyph, const char* help, bool enabled = true, float size = 28.0f);
bool pillButton(const char* label, ImU32 bg, ImU32 fg, float width = 0.0f);

bool navItem(const char* glyph, const char* label, bool selected);
void chip(const char* label, ImU32 dot, ImU32 bg = col::Well);

// Horizontal level meter (dB): green -> yellow -> red, with a peak-hold tick.
void meter(const char* label, float levelDb, float peakDb, float width);
// Big number tile for the performance page.
void statTile(const char* label, const char* value, const char* sub, ImU32 accent, ImVec2 size);

// Graph frame with dB grid; `curve(x01)` returns dB for x in [0,1]. Draws a glowing line + fill.
struct GraphAxis {
    float minDb, maxDb;
    std::vector<std::pair<float, const char*>> xTicks;  // (x in 0..1, label)
    std::vector<float> yTicks;                          // dB values
};
// fillToDb: the level the soft fill under the curve reaches down (or up) to.
void graph(const char* id, ImVec2 size, const GraphAxis& axis, const std::function<float(float)>& curve,
           ImU32 color, bool active, float nodeX = -1.0f, const std::function<float(float)>* reference = nullptr,
           float fillToDb = 0.0f);

void spinner(float radius, ImU32 color);

}  // namespace ui

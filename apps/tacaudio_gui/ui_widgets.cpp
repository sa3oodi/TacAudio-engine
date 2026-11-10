#include "ui_widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>

namespace ui {

Fonts fonts;
float dpi = 1.0f;

namespace icon {
// UTF-8 of Segoe Fluent Icons / MDL2 Assets code points.
const char* Home = "\xEE\xA0\x8F";       // U+E80F
const char* Audio = "\xEE\xA3\x96";      // U+E8D6
const char* Headphone = "\xEE\x9F\xB6";  // U+E7F6
const char* Devices = "\xEE\x9D\xB2";    // U+E772
const char* Gauge = "\xEE\xA7\x99";      // U+E9D9
const char* Info = "\xEE\xA5\x86";       // U+E946
const char* Refresh = "\xEE\x9C\xAC";    // U+E72C
const char* Reset = "\xEE\x9E\xA7";      // U+E7A7 (undo arrow)
const char* Power = "\xEE\x9F\xA8";      // U+E7E8
const char* Volume = "\xEE\x9D\xA7";     // U+E767
const char* Settings = "\xEE\x9C\x93";   // U+E713
const char* Setup = "\xEE\xA4\x8F";      // U+E90F (wrench)
const char* Check = "\xEE\x9C\xBE";      // U+E73E (check mark)
}  // namespace icon

namespace {

ImU32 lerpCol(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

// Per-widget animated value in [0,1] (stored in ImGui's state storage).
float animate(ImGuiID id, float target, float speed = 14.0f) {
    float* f = ImGui::GetStateStorage()->GetFloatRef(id, target);
    *f += (target - *f) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    return *f;
}

void textAt(ImDrawList* dl, ImVec2 p, ImU32 c, const char* s, float size, bool bold) {
    dl->AddText(bold ? fonts.semibold : fonts.regular, S(size), p, c, s);
}
ImVec2 measure(const char* s, float size, bool bold) {
    ImFont* f = bold ? fonts.semibold : fonts.regular;
    return f->CalcTextSizeA(S(size), FLT_MAX, 0.0f, s);
}

struct CardState {
    bool disabled = false;
    bool toggled = false;
};
std::vector<CardState> g_cards;

constexpr float kLabelW = 104.0f, kBoxW = 72.0f, kResetW = 28.0f, kGap = 10.0f, kRowH = 32.0f;

void rowLabel(ImVec2 pos, const char* label, const char* help) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = pos.y + (S(kRowH) - S(14.5f)) * 0.5f - S(1);
    textAt(dl, ImVec2(pos.x, y), col::Dim, label, 14.5f, false);
    if (help && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + S(kLabelW), pos.y + S(kRowH)))) tooltip(help);
}

}  // namespace

ImU32 withAlpha(ImU32 c, float a) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= a;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// ------------------------------------------------------------------ setup
void setup(float dpiScale) {
    dpi = dpiScale;
    ImGuiIO& io = ImGui::GetIO();
    namespace fs = std::filesystem;
    const char* iconFont = fs::exists("C:\\Windows\\Fonts\\SegoeIcons.ttf") ? "C:\\Windows\\Fonts\\SegoeIcons.ttf"
                           : fs::exists("C:\\Windows\\Fonts\\segmdl2.ttf")  ? "C:\\Windows\\Fonts\\segmdl2.ttf"
                                                                            : nullptr;
    static const ImWchar iconRanges[] = {0xE700, 0xF8FF, 0};
    auto load = [&](const char* path) -> ImFont* {
        ImFont* f = fs::exists(path) ? io.Fonts->AddFontFromFileTTF(path, 16.0f) : io.Fonts->AddFontDefault();
        if (iconFont) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.GlyphOffset = ImVec2(0, 2.0f);
            io.Fonts->AddFontFromFileTTF(iconFont, 16.0f, &cfg, iconRanges);
        }
        return f;
    };
    fonts.regular = load("C:\\Windows\\Fonts\\segoeui.ttf");
    fonts.semibold = load("C:\\Windows\\Fonts\\seguisb.ttf");

    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.FontSizeBase = 15.0f;
    st.FontScaleDpi = dpi;
    st.WindowRounding = 0;
    st.ChildRounding = 10;
    st.FrameRounding = 6;
    st.PopupRounding = 8;
    st.GrabRounding = 4;
    st.ScrollbarRounding = 6;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.PopupBorderSize = 1;
    st.FramePadding = ImVec2(10, 7);
    st.ItemSpacing = ImVec2(10, 10);
    st.WindowPadding = ImVec2(0, 0);
    st.ScrollbarSize = 10;
    ImVec4* c = st.Colors;
    auto v = [](ImU32 x) { return ImGui::ColorConvertU32ToFloat4(x); };
    c[ImGuiCol_Text] = v(col::Text);
    c[ImGuiCol_TextDisabled] = v(col::Faint);
    c[ImGuiCol_WindowBg] = v(col::Bg0);
    c[ImGuiCol_ChildBg] = v(col::Card);
    c[ImGuiCol_PopupBg] = v(IM_COL32(24, 28, 37, 250));
    c[ImGuiCol_Border] = v(col::Line);
    c[ImGuiCol_FrameBg] = v(col::Well);
    c[ImGuiCol_FrameBgHovered] = v(IM_COL32(24, 28, 36, 255));
    c[ImGuiCol_FrameBgActive] = v(IM_COL32(28, 33, 42, 255));
    c[ImGuiCol_Button] = v(withAlpha(col::CardHi, 0.0f));
    c[ImGuiCol_ButtonHovered] = v(col::CardHi);
    c[ImGuiCol_ButtonActive] = v(col::Line);
    c[ImGuiCol_Header] = v(withAlpha(col::Accent, 0.22f));
    c[ImGuiCol_HeaderHovered] = v(withAlpha(col::Accent, 0.32f));
    c[ImGuiCol_HeaderActive] = v(withAlpha(col::Accent, 0.42f));
    c[ImGuiCol_ScrollbarBg] = v(withAlpha(col::Bg0, 0.0f));
    c[ImGuiCol_ScrollbarGrab] = v(col::Line);
    c[ImGuiCol_CheckMark] = v(col::Accent);
    c[ImGuiCol_Separator] = v(col::Line);
    st.ScaleAllSizes(dpi);
}

// ------------------------------------------------------------------ text
void text(const char* s, ImU32 color, float size, bool bold) {
    ImGui::PushFont(bold ? fonts.semibold : fonts.regular, size);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void textUpper(const char* s, ImU32 color, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = p.x, spacing = S(1.4f);
    char ch[2] = {0, 0};
    for (const char* c = s; *c; ++c) {
        ch[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        textAt(dl, p, color, ch, size, true);
        p.x += measure(ch, size, true).x + spacing;
    }
    ImGui::Dummy(ImVec2(p.x - x0, measure("A", size, true).y));
}

void tooltip(const char* s) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10), S(8)));
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(S(320));
    ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ toggle
bool toggle(const char* id, bool* v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = S(22), w = S(40);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (clicked) *v = !*v;
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    const float t = animate(ImGui::GetItemID(), *v ? 1.0f : 0.0f);
    const ImU32 off = IM_COL32(58, 65, 80, 255);
    if (t > 0.01f) dl->AddRectFilled(p - ImVec2(S(3), S(3)), p + ImVec2(w + S(3), h + S(3)), withAlpha(col::Accent, 0.16f * t), h);
    dl->AddRectFilled(p, p + ImVec2(w, h), lerpCol(off, col::Accent, t), h * 0.5f);
    const float r = h * 0.5f - S(3);
    const float cx = p.x + h * 0.5f + t * (w - h);
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, disabled ? IM_COL32(150, 150, 150, 255) : IM_COL32(255, 255, 255, 255));
    return clicked;
}

// ------------------------------------------------------------------ card
void beginCard(const char* id, const char* title, bool* enabled, float width, const char* help) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::Card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(20), S(16)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(8)));
    ImGui::BeginChild(id, ImVec2(width, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    CardState cs;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    if (enabled) {
        cs.toggled = toggle("##cardToggle", enabled);
        ImGui::SameLine(0, S(12));
    }
    // Title, vertically centred on the toggle height.
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(tp.x, start.y + (S(22) - S(13.5f)) * 0.5f - S(1)));
    textUpper(title, col::Text, 13.5f);
    if (help) {
        ImGui::SameLine(0, S(8));
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##help", ImVec2(S(16), S(16)));
        const ImVec2 c = hp + ImVec2(S(8), S(8));
        dl->AddCircle(c, S(7.5f), col::Faint, 0, S(1.2f));
        const ImVec2 q = measure("?", 11.0f, true);
        textAt(dl, c - ImVec2(q.x * 0.5f, q.y * 0.5f), col::Dim, "?", 11.0f, true);
        if (ImGui::IsItemHovered()) tooltip(help);
    }
    // Divider across the whole card.
    const float y = std::max(ImGui::GetCursorScreenPos().y, start.y + S(22)) + S(8);
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowWidth(), y), col::Line, S(1));
    ImGui::SetCursorScreenPos(ImVec2(start.x, y + S(14)));
    ImGui::Dummy(ImVec2(0, 0));
    if (enabled && !*enabled) {
        ImGui::BeginDisabled();
        cs.disabled = true;
    }
    g_cards.push_back(cs);
}

bool cardToggleChanged() { return !g_cards.empty() && g_cards.back().toggled; }

void endCard() {
    if (!g_cards.empty()) {
        if (g_cards.back().disabled) ImGui::EndDisabled();
        g_cards.pop_back();
    }
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ buttons
bool iconButton(const char* id, const char* glyph, const char* help, bool enabled, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float s = S(size);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::EndDisabled();
    const float t = animate(ImGui::GetID(id), hovered && enabled ? 1.0f : 0.0f, 18.0f);
    if (t > 0.01f) dl->AddCircleFilled(p + ImVec2(s * 0.5f, s * 0.5f), s * 0.5f, withAlpha(col::CardHi, t));
    const ImVec2 g = measure(glyph, 14.0f, false);
    textAt(dl, p + ImVec2((s - g.x) * 0.5f, (s - g.y) * 0.5f), enabled ? (hovered ? col::Accent : col::Dim) : withAlpha(col::Faint, 0.6f),
           glyph, 14.0f, false);
    if (help && hovered) tooltip(help);
    return clicked && enabled;
}

bool pillButton(const char* label, ImU32 bg, ImU32 fg, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = measure(label, 14.5f, true);
    const ImVec2 sz(width > 0 ? width : ts.x + S(32), S(34));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, sz);
    const float t = animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f, 18.0f);
    dl->AddRectFilled(p, p + sz, lerpCol(bg, IM_COL32(255, 255, 255, 255), 0.08f * t), sz.y * 0.5f);
    textAt(dl, p + ImVec2((sz.x - ts.x) * 0.5f, (sz.y - ts.y) * 0.5f - S(1)), fg, label, 14.5f, true);
    return clicked;
}

// ------------------------------------------------------------------ rows
Edit sliderRow(const char* label, float* v, float min, float max, const char* fmt, float def, const char* help, ImU32 fill) {
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    rowLabel(row, label, help);

    const float trackX = row.x + S(kLabelW);
    const float trackW = std::max(S(40), avail - S(kLabelW) - S(kBoxW) - S(kResetW) - 2 * S(kGap));
    ImGui::SetCursorScreenPos(ImVec2(trackX - S(8), row.y));
    ImGui::InvisibleButton("##track", ImVec2(trackW + S(16), S(kRowH)));
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    Edit e = Edit::None;
    if (active) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - trackX) / trackW, 0.0f, 1.0f);
        const float nv = min + t * (max - min);
        if (nv != *v) {
            *v = nv;
            e = Edit::Changing;
        }
    }
    if (ImGui::IsItemDeactivated()) e = Edit::Commit;
    if (hovered && !active && ImGui::GetIO().MouseWheel != 0.0f) {  // fine steps with the wheel
        *v = std::clamp(*v + ImGui::GetIO().MouseWheel * (max - min) / 100.0f, min, max);
        e = Edit::Commit;
    }

    const float glow = animate(ImGui::GetItemID(), active ? 1.0f : hovered ? 0.5f : 0.0f);
    const float cy = row.y + S(kRowH) * 0.5f, th = S(4);
    const float t01 = std::clamp((*v - min) / (max - min), 0.0f, 1.0f);
    const float hx = trackX + t01 * trackW;
    const ImU32 f = disabled ? col::Faint : fill;
    dl->AddRectFilled(ImVec2(trackX, cy - th * 0.5f), ImVec2(trackX + trackW, cy + th * 0.5f), col::Line, th);
    if (glow > 0.01f && !disabled)
        dl->AddRectFilled(ImVec2(trackX, cy - th * 1.6f), ImVec2(hx, cy + th * 1.6f), withAlpha(f, 0.18f * glow), th * 2);
    dl->AddRectFilled(ImVec2(trackX, cy - th * 0.5f), ImVec2(hx, cy + th * 0.5f), f, th);
    // Handle: rounded white block with a grip line.
    const float hw = S(12), hh = S(20);
    dl->AddRectFilled(ImVec2(hx - hw * 0.5f, cy - hh * 0.5f), ImVec2(hx + hw * 0.5f, cy + hh * 0.5f),
                      disabled ? IM_COL32(120, 125, 135, 255) : IM_COL32(245, 247, 250, 255), S(4));
    dl->AddLine(ImVec2(hx, cy - hh * 0.25f), ImVec2(hx, cy + hh * 0.25f), IM_COL32(60, 66, 80, 255), S(1.5f));

    // Value box.
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, *v);
    const ImVec2 bp(trackX + trackW + S(kGap), row.y + S(4));
    const ImVec2 bs(S(kBoxW), S(kRowH) - S(8));
    dl->AddRectFilled(bp, bp + bs, col::Well, S(6));
    const ImVec2 ts = measure(buf, 13.5f, true);
    textAt(dl, bp + ImVec2((bs.x - ts.x) * 0.5f, (bs.y - ts.y) * 0.5f - S(1)), disabled ? col::Faint : col::Text, buf, 13.5f, true);

    // Reset.
    ImGui::SetCursorScreenPos(ImVec2(bp.x + bs.x + S(kGap), row.y + (S(kRowH) - S(kResetW)) * 0.5f));
    char tip[64];
    std::snprintf(tip, sizeof(tip), "Reset to default (%s)", [&] {
        static char d[32];
        std::snprintf(d, sizeof(d), fmt, def);
        return d;
    }());
    if (iconButton("##reset", icon::Reset, tip, std::fabs(*v - def) > 1e-4f && !disabled, kResetW)) {
        *v = def;
        e = Edit::Commit;
    }
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(kRowH)));
    ImGui::Dummy(ImVec2(avail, S(2)));
    ImGui::PopID();
    return e;
}

bool segmentedRow(const char* label, int* index, const std::vector<std::string>& items, int def, const char* help) {
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    rowLabel(row, label, help);
    const float x0 = row.x + S(kLabelW);
    const float w = std::max(S(60), avail - S(kLabelW) - S(kResetW) - S(kGap));
    const ImVec2 cp(x0, row.y + S(2)), cs(w, S(kRowH) - S(4));
    dl->AddRectFilled(cp, cp + cs, col::Well, S(8));
    bool changed = false;
    const float segW = w / static_cast<float>(items.size());
    const bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    for (std::size_t i = 0; i < items.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 sp(x0 + segW * i, cp.y);
        ImGui::SetCursorScreenPos(sp);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, cs.y)) && *index != static_cast<int>(i)) {
            *index = static_cast<int>(i);
            changed = true;
        }
        const bool sel = *index == static_cast<int>(i);
        const float t = animate(ImGui::GetItemID(), sel ? 1.0f : ImGui::IsItemHovered() ? 0.25f : 0.0f);
        if (t > 0.01f) {
            const ImU32 c = sel ? (disabled ? col::Faint : col::Accent) : col::CardHi;
            dl->AddRectFilled(sp + ImVec2(S(3), S(3)), sp + ImVec2(segW - S(3), cs.y - S(3)), withAlpha(c, sel ? t : t * 2.5f), S(6));
        }
        const ImVec2 ts = measure(items[i].c_str(), 13.5f, true);
        textAt(dl, sp + ImVec2((segW - ts.x) * 0.5f, (cs.y - ts.y) * 0.5f - S(1)),
               sel ? IM_COL32(20, 16, 12, 255) : (disabled ? col::Faint : col::Dim), items[i].c_str(), 13.5f, true);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + w + S(kGap), row.y + (S(kRowH) - S(kResetW)) * 0.5f));
    char tip[96];
    std::snprintf(tip, sizeof(tip), "Reset to default (%s)", def >= 0 && def < static_cast<int>(items.size()) ? items[def].c_str() : "");
    if (iconButton("##reset", icon::Reset, tip, *index != def && !disabled, kResetW)) {
        *index = def;
        changed = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(kRowH)));
    ImGui::Dummy(ImVec2(avail, S(2)));
    ImGui::PopID();
    return changed;
}

bool comboRow(const char* label, std::string* value, const std::vector<std::string>& items,
              const std::vector<std::string>* itemHelp, const std::string& def, const char* help) {
    ImGui::PushID(label);
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    rowLabel(row, label, help);
    const float w = std::max(S(60), avail - S(kLabelW) - (def.empty() ? 0 : S(kResetW) + S(kGap)));
    ImGui::SetCursorScreenPos(ImVec2(row.x + S(kLabelW), row.y + S(1)));
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), (S(kRowH) - S(2) - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    bool changed = false;
    // Chevron instead of ImGui's square arrow button (drawn on this window, before any popup opens).
    ImDrawList* wdl = ImGui::GetWindowDrawList();
    const ImVec2 fp = ImGui::GetCursorScreenPos();
    const float fh = ImGui::GetFrameHeight();
    const char* chevron = "\xEE\x9C\x8D";  // U+E70D
    const ImVec2 cs = measure(chevron, 11.0f, false);
    const bool open = ImGui::BeginCombo("##combo", value->c_str(), ImGuiComboFlags_NoArrowButton);
    wdl->AddText(fonts.regular, S(11), ImVec2(fp.x + w - S(14) - cs.x, fp.y + (fh - cs.y) * 0.5f), col::Dim, chevron);
    if (open) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            const bool sel = items[i] == *value;
            if (ImGui::Selectable(items[i].c_str(), sel)) {
                changed = items[i] != *value;
                *value = items[i];
            }
            if (itemHelp && i < itemHelp->size() && ImGui::IsItemHovered()) tooltip((*itemHelp)[i].c_str());
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar(2);
    const bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    if (!def.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(row.x + S(kLabelW) + w + S(kGap), row.y + (S(kRowH) - S(kResetW)) * 0.5f));
        const std::string tip = "Reset to default (" + def + ")";
        if (iconButton("##reset", icon::Reset, tip.c_str(), *value != def && !disabled, kResetW)) {
            *value = def;
            changed = true;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(kRowH)));
    ImGui::Dummy(ImVec2(avail, S(2)));
    ImGui::PopID();
    return changed;
}

bool toggleRow(const char* label, bool* v, const char* description, bool def) {
    ImGui::PushID(label);
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    textAt(dl, ImVec2(row.x, row.y + (S(kRowH) - S(14.5f)) * 0.5f - S(1)), col::Dim, label, 14.5f, false);
    const float x0 = row.x + S(kLabelW) + S(60);
    ImGui::SetCursorScreenPos(ImVec2(x0, row.y + (S(kRowH) - S(22)) * 0.5f));
    bool changed = toggle("##t", v);
    if (description) {
        const float dx = x0 + S(56);
        ImGui::PushClipRect(ImVec2(dx, row.y), ImVec2(row.x + avail - S(kResetW) - S(kGap), row.y + S(kRowH)), true);
        textAt(dl, ImVec2(dx, row.y + (S(kRowH) - S(13)) * 0.5f - S(1)), col::Faint, description, 13.0f, false);
        ImGui::PopClipRect();
    }
    ImGui::SetCursorScreenPos(ImVec2(row.x + avail - S(kResetW), row.y + (S(kRowH) - S(kResetW)) * 0.5f));
    const bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    if (iconButton("##reset", icon::Reset, def ? "Reset to default (on)" : "Reset to default (off)", *v != def && !disabled,
                   kResetW)) {
        *v = def;
        changed = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + S(kRowH)));
    ImGui::Dummy(ImVec2(avail, S(4)));
    ImGui::PopID();
    return changed;
}

// ------------------------------------------------------------------ navigation, chips
bool navItem(const char* glyph, const char* label, bool selected) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ImGui::GetContentRegionAvail().x, S(44));
    const bool clicked = ImGui::InvisibleButton(label, sz);
    const float t = animate(ImGui::GetItemID(), selected ? 1.0f : ImGui::IsItemHovered() ? 0.45f : 0.0f);
    if (t > 0.01f) dl->AddRectFilled(p, p + sz, withAlpha(col::CardHi, t), S(8));
    if (selected) dl->AddRectFilled(p + ImVec2(0, S(8)), p + ImVec2(S(3), sz.y - S(8)), col::Accent, S(2));
    const ImU32 c = selected ? col::Text : col::Dim;
    const ImVec2 g = measure(glyph, 17.0f, false);
    textAt(dl, p + ImVec2(S(18), (sz.y - g.y) * 0.5f), selected ? col::Accent : c, glyph, 17.0f, false);
    const ImVec2 ts = measure(label, 15.0f, selected);
    textAt(dl, p + ImVec2(S(52), (sz.y - ts.y) * 0.5f - S(1)), c, label, 15.0f, selected);
    return clicked;
}

void chip(const char* label, ImU32 dot, ImU32 bg) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = measure(label, 13.0f, true);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ts.x + S(34), S(28));
    dl->AddRectFilled(p, p + sz, bg, sz.y * 0.5f);
    const ImVec2 c = p + ImVec2(S(14), sz.y * 0.5f);
    dl->AddCircleFilled(c, S(7), withAlpha(dot, 0.25f));
    dl->AddCircleFilled(c, S(4), dot);
    textAt(dl, p + ImVec2(S(25), (sz.y - ts.y) * 0.5f - S(1)), col::Text, label, 13.0f, true);
    ImGui::Dummy(sz);
}

// ------------------------------------------------------------------ meters, tiles
void meter(const char* label, float levelDb, float peakDb, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lw = S(18), h = S(8);
    textAt(dl, p + ImVec2(0, -S(3)), col::Dim, label, 12.5f, true);
    const ImVec2 b(p.x + lw, p.y);
    const float w = width - lw;
    dl->AddRectFilled(b, b + ImVec2(w, h), col::Well, h * 0.5f);
    auto x01 = [](float db) { return std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f); };
    const float f = x01(levelDb);
    if (f > 0.0f) {
        const float yellowAt = 0.75f, redAt = 0.92f;
        const float x1 = std::min(f, yellowAt), x2 = std::min(f, redAt);
        dl->AddRectFilledMultiColor(b, b + ImVec2(w * x1, h), col::Teal, lerpCol(col::Teal, col::Yellow, x1 / yellowAt),
                                    lerpCol(col::Teal, col::Yellow, x1 / yellowAt), col::Teal);
        if (f > yellowAt)
            dl->AddRectFilled(ImVec2(b.x + w * yellowAt, b.y), ImVec2(b.x + w * x2, b.y + h), col::Yellow);
        if (f > redAt) dl->AddRectFilled(ImVec2(b.x + w * redAt, b.y), ImVec2(b.x + w * f, b.y + h), col::Red, h * 0.5f);
    }
    const float px = b.x + w * x01(peakDb);
    if (peakDb > -47.0f) dl->AddRectFilled(ImVec2(px - S(1), b.y - S(2)), ImVec2(px + S(1), b.y + h + S(2)), col::Text);
    ImGui::Dummy(ImVec2(width, h + S(6)));
}

void statTile(const char* label, const char* value, const char* sub, ImU32 accent, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, p + size, col::Card, S(12));
    dl->AddRectFilled(p + ImVec2(0, S(14)), p + ImVec2(S(3), size.y - S(14)), accent, S(2));
    ImGui::SetCursorScreenPos(p + ImVec2(S(18), S(14)));
    textUpper(label, col::Dim, 11.5f);
    textAt(dl, p + ImVec2(S(18), S(34)), accent, value, 30.0f, true);
    if (sub) textAt(dl, p + ImVec2(S(18), size.y - S(28)), col::Faint, sub, 12.5f, false);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(size);
}

// ------------------------------------------------------------------ graph
void graph(const char* id, ImVec2 size, const GraphAxis& ax, const std::function<float(float)>& curve, ImU32 color,
           bool active, float nodeX, const std::function<float(float)>* reference, float fillToDb) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, p + size, col::Well, S(10));
    const float padL = S(40), padR = S(14), padT = S(12), padB = S(24);
    const ImVec2 a(p.x + padL, p.y + padT), b(p.x + size.x - padR, p.y + size.y - padB);
    auto Y = [&](float db) { return b.y - (std::clamp(db, ax.minDb, ax.maxDb) - ax.minDb) / (ax.maxDb - ax.minDb) * (b.y - a.y); };
    auto X = [&](float x) { return a.x + x * (b.x - a.x); };
    char buf[16];
    for (float db : ax.yTicks) {
        const float y = Y(db);
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), db == 0.0f ? col::Line : withAlpha(col::Line, 0.5f), S(1));
        std::snprintf(buf, sizeof(buf), db > 0 ? "+%.0f" : "%.0f", db);
        const ImVec2 ts = measure(buf, 11.5f, false);
        textAt(dl, ImVec2(a.x - S(8) - ts.x, y - ts.y * 0.5f), col::Faint, buf, 11.5f, false);
    }
    for (const auto& [x, lbl] : ax.xTicks) {
        const float px = X(x);
        dl->AddLine(ImVec2(px, a.y), ImVec2(px, b.y), withAlpha(col::Line, 0.35f), S(1));
        const ImVec2 ts = measure(lbl, 11.5f, false);
        textAt(dl, ImVec2(px - ts.x * 0.5f, b.y + S(6)), col::Faint, lbl, 11.5f, false);
    }
    if (reference) {
        ImVec2 prev;
        for (int i = 0; i <= 60; ++i) {
            const float x = i / 60.0f;
            const ImVec2 q(X(x), Y((*reference)(x)));
            if (i && i % 2) dl->AddLine(prev, q, col::Faint, S(1.2f));  // dashed
            prev = q;
        }
    }
    constexpr int N = 140;
    ImVec2 pts[N + 3];
    for (int i = 0; i <= N; ++i) {
        const float x = i / static_cast<float>(N);
        pts[i] = ImVec2(X(x), Y(curve(x)));
    }
    const ImU32 c = active ? color : col::Faint;
    // Soft fill between the curve and fillToDb, as one polygon (no seams between strips).
    const float base = Y(std::clamp(fillToDb, ax.minDb, ax.maxDb));
    pts[N + 1] = ImVec2(pts[N].x, base);
    pts[N + 2] = ImVec2(pts[0].x, base);
    dl->AddConcavePolyFilled(pts, N + 3, withAlpha(c, active ? 0.12f : 0.05f));
    if (active) {
        dl->AddPolyline(pts, N + 1, withAlpha(c, 0.10f), ImDrawFlags_None, S(9));
        dl->AddPolyline(pts, N + 1, withAlpha(c, 0.22f), ImDrawFlags_None, S(4.5f));
    }
    dl->AddPolyline(pts, N + 1, c, ImDrawFlags_None, S(2));
    if (nodeX >= 0.0f) {
        const ImVec2 n(X(nodeX), Y(curve(nodeX)));
        if (active)
            for (int i = 4; i >= 1; --i) dl->AddCircleFilled(n, S(4.0f + 3.0f * i), withAlpha(c, 0.06f));
        dl->AddCircleFilled(n, S(6), c);
        dl->AddCircleFilled(n, S(2.5f), IM_COL32(255, 255, 255, active ? 230 : 120));
    }
    ImGui::Dummy(size);
    ImGui::PopID();
}

void spinner(float radius, ImU32 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = S(radius), t = static_cast<float>(ImGui::GetTime()) * 6.0f;
    const ImVec2 c = p + ImVec2(r, r);
    dl->PathArcTo(c, r - S(1.5f), t, t + std::numbers::pi_v<float> * 1.4f, 24);
    dl->PathStroke(color, ImDrawFlags_None, S(2.5f));
    ImGui::Dummy(ImVec2(r * 2, r * 2));
}

}  // namespace ui

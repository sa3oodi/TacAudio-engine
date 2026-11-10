#include "core/layout.h"

#include <sstream>
#include <stdexcept>

namespace tac {

namespace {
struct MaskBit {
    std::uint32_t bit;
    const char* name;
};
// Windows ksmedia.h SPEAKER_* bits, in channel order.
constexpr MaskBit kBits[] = {
    {0x1, "FL"},      {0x2, "FR"},      {0x4, "FC"},       {0x8, "LFE"},     {0x10, "BL"},    {0x20, "BR"},
    {0x40, "FLC"},    {0x80, "FRC"},    {0x100, "BC"},     {0x200, "SL"},    {0x400, "SR"},   {0x800, "TC"},
    {0x1000, "TFL"},  {0x2000, "TFC"},  {0x4000, "TFR"},   {0x8000, "TBL"},  {0x10000, "TBC"}, {0x20000, "TBR"},
};
}  // namespace

SpeakerLayout layoutFromChannelMask(std::uint32_t mask, const LayoutAngles& a) {
    if (mask == 0) throw std::invalid_argument("channel mask is 0 (unknown layout)");
    const bool hasSides = (mask & 0x600) != 0;
    const float back = hasSides ? a.backDeg : a.back51Deg;
    const float h = a.heightElevationDeg;

    SpeakerLayout layout;
    layout.channelMask = mask;
    for (const auto& b : kBits) {
        if (!(mask & b.bit)) continue;
        Speaker s;
        s.name = b.name;
        switch (b.bit) {
            case 0x1: s.azimuthDeg = -a.frontDeg; break;
            case 0x2: s.azimuthDeg = a.frontDeg; break;
            case 0x4: s.azimuthDeg = 0; break;
            case 0x8: s.isLfe = true; break;
            case 0x10: s.azimuthDeg = -back; break;
            case 0x20: s.azimuthDeg = back; break;
            case 0x40: s.azimuthDeg = -15; break;
            case 0x80: s.azimuthDeg = 15; break;
            case 0x100: s.azimuthDeg = 180; break;
            case 0x200: s.azimuthDeg = -a.sideDeg; break;
            case 0x400: s.azimuthDeg = a.sideDeg; break;
            case 0x800: s.elevationDeg = 90; break;
            case 0x1000: s.azimuthDeg = -45; s.elevationDeg = h; break;
            case 0x2000: s.azimuthDeg = 0; s.elevationDeg = h; break;
            case 0x4000: s.azimuthDeg = 45; s.elevationDeg = h; break;
            case 0x8000: s.azimuthDeg = -135; s.elevationDeg = h; break;
            case 0x10000: s.azimuthDeg = 180; s.elevationDeg = h; break;
            case 0x20000: s.azimuthDeg = 135; s.elevationDeg = h; break;
            default: break;
        }
        layout.speakers.push_back(s);
    }
    return layout;
}

SpeakerLayout layout71(const LayoutAngles& angles) { return layoutFromChannelMask(0x63F, angles); }

std::string SpeakerLayout::describe() const {
    std::ostringstream os;
    for (std::size_t i = 0; i < speakers.size(); ++i) {
        const auto& s = speakers[i];
        if (i) os << ", ";
        os << s.name;
        if (s.isLfe) continue;
        os << '(' << s.azimuthDeg;
        if (s.elevationDeg != 0) os << '/' << s.elevationDeg;
        os << ')';
    }
    return os.str();
}

}  // namespace tac

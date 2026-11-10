#pragma once
// Speaker layouts built from Windows SPEAKER_* channel masks.
// Channel order follows the mask bit order (that is how WASAPI and WAV deliver it),
// e.g. Windows 7.1 = FL FR FC LFE BL BR SL SR.

#include <cstdint>
#include <string>
#include <vector>

namespace tac {

struct Speaker {
    std::string name;
    float azimuthDeg = 0;    // TacAudio convention: 0 front, +right
    float elevationDeg = 0;
    bool isLfe = false;
};

struct LayoutAngles {
    float frontDeg = 30.0f;  // FL/FR
    float sideDeg = 90.0f;   // SL/SR. ITU allows 90..110; 90 gives the sharpest left/right for gaming
    float backDeg = 140.0f;  // BL/BR in 7.1 (ITU 135..150)
    float back51Deg = 110.0f;  // BL/BR when the layout has no side pair (5.1 "back" variant)
    float heightElevationDeg = 45.0f;
};

struct SpeakerLayout {
    std::uint32_t channelMask = 0;
    std::vector<Speaker> speakers;  // index == channel index

    std::size_t size() const noexcept { return speakers.size(); }
    std::string describe() const;
};

SpeakerLayout layoutFromChannelMask(std::uint32_t mask, const LayoutAngles& angles = {});
SpeakerLayout layout71(const LayoutAngles& angles = {});

}  // namespace tac

#pragma once
// Synthetic test scenes rendered to a multichannel (e.g. 7.1) bed with VBAP.
// Used to A/B direction quality and, later, to test footstep/gunshot processing.

#include <cstdint>
#include <string>
#include <vector>

#include "core/layout.h"
#include "core/wav.h"

namespace tac {

enum class SceneKind {
    Compass,  // footsteps at 8 fixed directions: front, front-right, right, ... (2 s each)
    Circle,   // footsteps walking a full clockwise circle around the listener
    Battle,   // circling footsteps + loud gunshots from the front-right (for M3 processing)
};

struct SceneEvent {
    double timeSec;
    float azimuthDeg;
    std::string label;
};

struct Scene {
    AudioBuffer audio;               // channels == layout.size()
    std::vector<SceneEvent> events;  // what happens when (for listening notes)
};

Scene makeScene(SceneKind kind, const SpeakerLayout& layout, std::uint32_t sampleRate = 48000);

const char* sceneName(SceneKind kind) noexcept;

}  // namespace tac

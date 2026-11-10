#pragma once
// 2D VBAP (vector-base amplitude panning) on the horizontal speakers of a layout.
// Gains are power-normalised (sum of squares = 1), so a moving source keeps a
// constant loudness all around the listener. Height speakers and LFE get 0.

#include <vector>

#include "core/layout.h"

namespace tac {

class Vbap2D {
public:
    explicit Vbap2D(const SpeakerLayout& layout);

    // Writes layout.size() gains for a source at `azimuthDeg`.
    void gains(float azimuthDeg, float* out) const noexcept;
    std::vector<float> gains(float azimuthDeg) const;

    std::size_t numChannels() const noexcept { return numChannels_; }

private:
    struct Pair {
        int a, b;            // channel indices, a counter-clockwise of b
        double inv[2][2];    // inverse of the 2x2 speaker-vector matrix
        double azA, span;    // start azimuth (deg) and arc width
    };
    std::size_t numChannels_;
    std::vector<Pair> pairs_;
};

}  // namespace tac

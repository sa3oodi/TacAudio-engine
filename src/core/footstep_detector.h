#pragma once
// Footstep detector for one channel: how much "footstep-like sound" is on it right now (0..1).
//
// A footstep in a game mix is a short impact whose energy sits in the crunch band (~1.5-6 kHz),
// at a moderate level, repeated. The detector requires all of:
//   * an onset: the band's level (5 ms) jumps above its own background (300 ms)
//   * not loud: gunfire, explosions and killstreaks peak far higher than steps
//   * not bass-heavy: crunch within ~15 dB of the low end (explosions / vehicles are mostly rumble)
//   * audible: above the noise floor
// The result rises within ~10 ms, lags 16 ms (onsets are confirmed once the whole sound is
// there) and is held for about one step, so a walking enemy gives a steady signal.
// Real-time safe (no allocation, no locks).

#include <cstddef>

#include "core/biquad.h"

namespace tac {

class FootstepDetector {
public:
    explicit FootstepDetector(double sampleRate = 48000.0);

    void process(const float* x, std::size_t n) noexcept;
    float activity() const noexcept { return act_; }
    // Strongest activity since the last call.
    float takeMax() noexcept {
        const float m = max_;
        max_ = act_;
        return m;
    }
    void reset() noexcept;

private:
    Biquad hp_, lp_, low_;          // crunch band = hp(1.5 kHz) + lp(6 kHz); rumble = lp(250 Hz)
    float fast_ = 0, slow_ = 0;     // band energy, 5 ms / 300 ms
    float bandE_ = 0, lowE_ = 0;    // band / rumble energy, 20 ms
    float peak_ = 0;                // full-band peak, 50 ms release
    float act_ = 0, max_ = 0;
    float cFast_, cSlow_, c20_, cPeak_, cAtt_, cRel_;
    int tick_ = 0;
    float want_ = 0;
    // Onsets are judged 16 ms later, once the whole sound is there (an explosion's first
    // milliseconds can look crunchy before its rumble builds up).
    static constexpr int kDelay = 48;  // decisions (16 ms at 3 kHz)
    float onsetHist_[kDelay] = {};
    int histPos_ = 0;
};

}  // namespace tac

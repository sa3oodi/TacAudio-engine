#pragma once
// Footstep detection for the direction ring: which channels carry enemy footsteps right now (0..1).
//
// Two stages:
//
// 1. FootstepDetector (per channel) finds footstep-like *impacts*: a short sound whose energy sits
//    in the crunch band (~1.5-6 kHz), at a moderate level. It requires all of
//      * an onset: the band's level (5 ms) jumps above its own background (300 ms)
//      * not loud: your own gunfire, explosions and killstreaks peak far higher than steps
//      * not bass-heavy: crunch within ~15 dB of the low end (explosions / vehicles are rumble)
//      * audible: above the noise floor
//    Onsets are judged 16 ms later, once the whole sound is there.
//
// 2. FootstepTracker (all channels together) turns impacts into *footsteps*. Many sounds are
//    short crunchy impacts too (reloads, bolt clicks, distant shots, bullet hits, hit markers), so
//    an impact only counts when it is part of a walk:
//      * rhythm: at least 3 impacts from the same direction (within 60 degrees), 0.22-1.0 s apart,
//        each gap within 0.7-1.43x of the previous one (walking and running are regular,
//        reload sounds are not)
//      * short: one step's heel, gravel and toe are over within ~0.15 s; a sound that keeps
//        hitting (automatic fire, a reload rattle) is not a step
//      * not your own sounds: your weapon, reload and footsteps are mixed straight ahead / to
//        every speaker (not placed in the world), so impacts centred within 15 degrees of the
//        front, or spread evenly over the speakers, are ignored. An enemy right in front of you
//        is on your screen anyway.
//    The first two steps of a walk only arm the tracker; the ring lights from the third on.
// Real-time safe (no allocation after construction, no locks).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/biquad.h"

namespace tac {

class FootstepDetector {
public:
    explicit FootstepDetector(double sampleRate = 48000.0);

    static constexpr int kDecim = 16;  // one decision every 16 samples (0.33 ms at 48 kHz)

    // Returns true when a new decision was made (every kDecim samples) during this call.
    bool process(const float* x, std::size_t n) noexcept;
    float candidate() const noexcept { return cand_; }  // 0..1: an impact that looks like a step
    // Band energy above the channel's own background (what the impact added), 20 ms.
    float excessEnergy() const noexcept { return bandE_ > slow_ ? bandE_ - slow_ : 0.0f; }
    void reset() noexcept;

private:
    Biquad hp_, lp_, low_;          // crunch band = hp(1.5 kHz) + lp(6 kHz); rumble = lp(250 Hz)
    float fast_ = 0, slow_ = 0;     // band energy, 5 ms / 300 ms
    float bandE_ = 0, lowE_ = 0;    // band / rumble energy, 20 ms
    float peak_ = 0;                // full-band peak, 50 ms release
    float cand_ = 0;
    float cFast_, cSlow_, c20_, cPeak_;
    int tick_ = 0;
    // An explosion's first milliseconds can look crunchy before its rumble builds up.
    static constexpr int kDelay = 48;  // decisions (16 ms at 3 kHz)
    float onsetHist_[kDelay] = {};
    int histPos_ = 0;
};

class FootstepTracker {
public:
    static constexpr std::size_t kMaxChannels = 8;

    FootstepTracker() = default;
    // One entry per channel (speaker): its direction (degrees, 0 = front, + = right) and LFE flag.
    FootstepTracker(double sampleRate, const std::vector<float>& azimuthDeg, const std::vector<bool>& lfe);

    std::size_t channels() const noexcept { return n_; }
    // ch: planar input, one pointer per channel (channels() of them).
    void process(const float* const* ch, std::size_t frames) noexcept;
    // Strongest footstep activity on channel c since the last call.
    float takeMax(std::size_t c) noexcept;
    float activity(std::size_t c) const noexcept { return c < n_ ? act_[c] : 0.0f; }
    void reset() noexcept;

    // Diagnostics (tests): impacts seen, and footsteps confirmed as part of a walk.
    int impacts() const noexcept { return impacts_; }
    int steps() const noexcept { return steps_; }

private:
    void decide() noexcept;
    void classify() noexcept;

    struct Walk {
        std::int64_t last = -1;   // tick of the last step, -1 = unused
        std::int64_t gap = 0;     // previous gap (ticks), 0 = only one step so far
        float az = 0;             // direction of the last step (degrees)
        int count = 0;
    };

    std::size_t n_ = 0;
    double tickRate_ = 3000.0;
    std::vector<FootstepDetector> det_;
    std::array<float, kMaxChannels> cosAz_{}, sinAz_{}, azDeg_{};
    std::array<bool, kMaxChannels> lfe_{};
    int nonLfe_ = 0;
    int phase_ = 0;                 // samples into the current decision
    std::int64_t now_ = 0;          // decisions so far
    // An impact = a cluster of onsets (heel, gravel, toe) that ends after 120 ms without a new one.
    bool high_ = false;             // candidate above the rise threshold
    bool inImpact_ = false;
    std::int64_t impactStart_ = 0, lastRise_ = 0;
    std::array<float, kMaxChannels> evE_{};
    std::array<Walk, 3> walks_{};
    std::array<float, kMaxChannels> act_{}, max_{};
    float cRel_ = 0;
    int impacts_ = 0, steps_ = 0;
};

}  // namespace tac

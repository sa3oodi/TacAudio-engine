#pragma once
// Per-channel dynamics for competitive listening (M3 "TacProcessor", first stage).
//
// Runs on every input channel BEFORE the binaural stage (so HRTF cues are untouched):
//   * downward compression above `thresholdDb`  -> gunfire, explosions, killstreaks are tamed
//   * upward compression below `boostBelowDb`   -> quiet sounds (distant footsteps) are lifted
//   * no boost below `gateDb`                   -> background noise is not raised
// Channels are independent: a loud sound on one side does not duck quiet sounds elsewhere.
// A short look-ahead lets the gain drop before a transient's peak arrives.
//
//   gain(dB)
//    +max |____
//         |    \__                 (upward: quiet sounds lifted)
//      0  |-------\_______________
//         |                       \___      (downward: loud sounds reduced)
//         +---gate----boostBelow------threshold--------> input level (dBFS)

#include <cstddef>
#include <string>
#include <vector>

#include "core/biquad.h"

namespace tac {

struct DynamicsParams {
    std::string name = "off";
    bool enabled = false;
    float thresholdDb = -24.0f;  // compress above
    float ratio = 4.0f;
    float kneeDb = 6.0f;
    float boostBelowDb = -38.0f;  // lift below
    float boostRatio = 2.0f;
    float maxBoostDb = 8.0f;
    float gateDb = -62.0f;        // no lift below (fades in over 6 dB)
    float makeupDb = 4.0f;        // overall gain after taming (keeps footsteps loud)
    // Punch 0..100: how much of a sound's first milliseconds passes before taming starts.
    //   0   -> peak detector, 1 ms attack: everything loud is flattened (no impact left)
    //   100 -> ~50 ms loudness detector, 30 ms attack: shot cracks pass, sustained loudness
    //          (killstreak signatures, constant fire, explosion rumble) is still tamed.
    float punch = 60.0f;
    float releaseMs = 120.0f;
    float lookaheadMs = 1.5f;

    // Footstep clarity: a band boost around the footstep "crunch" that is applied only while
    // the channel is quiet (full below footstepBelowDb, gone 10 dB above it), so loud gun
    // cracks in the same band are not boosted. 0 dB = off. Works even when dynamics are off.
    float footstepDb = 0.0f;
    float footstepHz = 3500.0f;
    float footstepQ = 0.8f;
    float footstepBelowDb = -30.0f;

    bool active() const noexcept { return enabled || footstepDb > 0.0f; }
};

// "off", "light", "medium", "strong". Throws std::invalid_argument for unknown names.
DynamicsParams dynamicsPreset(const std::string& name);

class ChannelDynamics {
public:
    ChannelDynamics(double sampleRate, std::size_t channels, const DynamicsParams& params);

    // In place; output is delayed by latency() samples.
    void process(float* const* io, std::size_t n) noexcept;
    void reset() noexcept;

    std::size_t latency() const noexcept { return lookahead_; }
    // Static gain curve (dB) for a given input level (dBFS), excluding makeup. Exposed for tests.
    float curveDb(float levelDb) const noexcept;

    // Strongest reduction / boost applied since the last call (dB, makeup excluded).
    float takeMinGainDb() noexcept;
    float takeMaxGainDb() noexcept;

private:
    struct Channel {
        std::vector<float> delay;
        float peak = 0.0f;    // fast peak detector (punch 0)
        float ms = 0.0f;      // mean-square loudness detector (punch > 0)
        float gainDb = 0.0f;  // smoothed gain
        Biquad band;          // footstep band (0 dB peak band-pass)
        float stepGain = 0.0f;  // smoothed linear extra gain for the band
    };
    DynamicsParams p_;
    std::size_t lookahead_;
    std::size_t pos_ = 0;
    float detRelease_, msCoef_, peakMix_, attCoef_, relCoef_, makeup_;
    float stepMax_ = 0.0f, stepCoef_ = 0.0f;
    std::vector<Channel> ch_;
    float minGainDb_ = 0.0f, maxGainDb_ = 0.0f;
};

}  // namespace tac

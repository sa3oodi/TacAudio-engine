#pragma once
// Small-room reverb for headphone externalisation (same architecture family as
// commercial virtualisers: early reflections + feedback delay network).
//
//   stereo send ─┬─ band-limit (low/high cut) ─┬─ early reflections (L/R + cross taps)
//                │                              └─ pre-delay ─ 4-line FDN (Hadamard,
//                │                                             per-line damping, RT60 gains)
//                └────────────────────────────────── wet = early + late, M/S width
//
// The wet signal is ADDED to the binaural output. Bass and top end are kept out of
// the reverb so footsteps and transients stay clean. Real-time safe (no allocation in process()).

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "core/biquad.h"

namespace tac {

struct RoomParams {
    std::string name = "neutral";
    bool enabled = false;
    float size = 1.0f;          // scales reflection and delay-line times
    float preDelayMs = 6.0f;    // extra delay before the late reverb starts
    float earlyDb = -16.0f;     // early reflections level (re. direct)
    float lateDb = -20.0f;      // late reverb level (re. direct)
    float rt60 = 0.25f;         // late decay time (s) at mid frequencies
    float dampingHz = 6000.0f;  // highs decay faster above this
    float lowCutHz = 160.0f;    // no reverb below (clean bass)
    float highCutHz = 9000.0f;  // no reverb above (clean transients)
    float width = 1.0f;         // 0 = mono wet, 1 = natural, >1 = wider
};

// "neutral" (dry), "balanced", "spacious". Throws std::invalid_argument for unknown names.
RoomParams roomPreset(const std::string& name);

class RoomReverb {
public:
    RoomReverb(double sampleRate, const RoomParams& params);

    // Adds the wet signal of (inL, inR) into (outL, outR).
    void process(const float* inL, const float* inR, float* outL, float* outR, std::size_t n) noexcept;
    void reset() noexcept;

    const RoomParams& params() const noexcept { return p_; }

private:
    struct Delay {
        std::vector<float> buf;
        std::size_t pos = 0;
        void init(std::size_t len) { buf.assign(len, 0.0f); pos = 0; }
        void push(float x) noexcept {
            buf[pos] = x;
            if (++pos == buf.size()) pos = 0;
        }
        // Sample written `d` pushes ago (d >= 1, d <= length).
        float tap(std::size_t d) const noexcept { return buf[(pos + buf.size() - d) % buf.size()]; }
    };
    struct Tap {
        std::size_t delay;
        float gain;
    };

    RoomParams p_;
    Biquad lowCutL_, lowCutR_, highCutL_, highCutR_;
    Delay inL_, inR_;                       // band-limited input history (early taps + pre-delay)
    std::vector<Tap> earlyLL_, earlyRL_, earlyRR_, earlyLR_;  // [from][to]
    std::size_t preDelay_ = 1;
    std::array<Delay, 4> lines_;
    std::array<float, 4> lineGain_{}, damp_{};
    float dampCoef_ = 0, earlyGain_ = 0, lateGain_ = 0, lateIn_ = 0;
};

}  // namespace tac

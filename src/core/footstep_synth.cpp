#include "core/footstep_synth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/biquad.h"

namespace tac {

namespace {
struct Rand {
    std::uint32_t s;
    float uni() noexcept {  // [0, 1)
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s >> 8) / 16777216.0f;
    }
    float noise() noexcept { return 2.0f * uni() - 1.0f; }
    float range(float a, float b) noexcept { return a + (b - a) * uni(); }
};

// Filtered noise burst with a fast attack and exponential decay, added at `at`.
void burst(std::vector<float>& out, double fs, Rand& r, double at, double decaySec, Biquad f, float amp,
           double attackSec = 0.0005) {
    const auto start = static_cast<std::size_t>(at * fs);
    const auto len = static_cast<std::size_t>((decaySec * 6.0 + attackSec) * fs);
    for (std::size_t i = 0; i < len && start + i < out.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        const double env = std::min(1.0, t / attackSec) * std::exp(-t / decaySec);
        out[start + i] += amp * static_cast<float>(env) * f.process(r.noise());
    }
}

// Decaying low "knock" (the boot's body), added at `at`.
void knock(std::vector<float>& out, double fs, double at, double hz, double decaySec, float amp) {
    const auto start = static_cast<std::size_t>(at * fs);
    const auto len = static_cast<std::size_t>(decaySec * 6.0 * fs);
    for (std::size_t i = 0; i < len && start + i < out.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        out[start + i] += amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * t) * std::exp(-t / decaySec) *
                                                  std::min(1.0, t / 0.001));
    }
}

void oneStep(std::vector<float>& out, double fs, Rand& r, double at, float loud) {
    // Heel strike: thump, knock and a sharp click (the click carries most of the direction cue).
    burst(out, fs, r, at, r.range(0.016f, 0.026f), Biquad::lowpass(fs, r.range(140.0f, 220.0f), 1.1), 1.0f * loud, 0.002);
    knock(out, fs, at, r.range(95.0f, 140.0f), 0.028, 0.35f * loud);
    burst(out, fs, r, at + 0.001, r.range(0.003f, 0.006f), Biquad::highpass(fs, r.range(1800.0f, 2600.0f)), 0.55f * loud);
    // Gravel: many tiny grains, dense at first and thinning out.
    const int grains = static_cast<int>(r.range(22.0f, 34.0f));
    for (int g = 0; g < grains; ++g) {
        const double t = at + 0.002 + 0.11 * std::pow(static_cast<double>(r.uni()), 1.8);
        const float a = loud * r.range(0.12f, 0.42f) * static_cast<float>(std::exp(-(t - at) / 0.06));
        burst(out, fs, r, t, r.range(0.0012f, 0.0045f), Biquad::bandpass(fs, r.range(1800.0f, 7500.0f), r.range(1.0f, 2.2f)), a,
              0.0002);
    }
    // Toe roll ~70 ms later: softer thump, click and a few grains.
    const double toe = at + r.range(0.060f, 0.085f);
    burst(out, fs, r, toe, 0.014, Biquad::lowpass(fs, r.range(200.0f, 300.0f), 1.0), 0.45f * loud, 0.002);
    burst(out, fs, r, toe + 0.001, 0.004, Biquad::highpass(fs, r.range(2200.0f, 3200.0f)), 0.3f * loud);
    for (int g = 0; g < 10; ++g)
        burst(out, fs, r, toe + 0.05 * r.uni(), r.range(0.0012f, 0.004f),
              Biquad::bandpass(fs, r.range(2500.0f, 8000.0f), 1.5), loud * r.range(0.06f, 0.2f), 0.0002);
}
}  // namespace

std::vector<float> synthFootsteps(double fs, int count, double intervalSec, std::uint32_t seed, float peak) {
    Rand r{seed ? seed : 0x9E3779B9u};
    const double total = std::max(0, count - 1) * intervalSec * 1.08 + 0.35;
    std::vector<float> out(static_cast<std::size_t>(total * fs), 0.0f);
    double t = 0.01;
    for (int k = 0; k < count; ++k) {
        oneStep(out, fs, r, t, r.range(0.8f, 1.0f));
        t += intervalSec * r.range(0.94f, 1.06f);  // a little uneven, like real walking
    }
    float pk = 0.0f;
    for (float v : out) pk = std::max(pk, std::abs(v));
    if (pk > 0.0f)
        for (float& v : out) v *= peak / pk;
    return out;
}

}  // namespace tac

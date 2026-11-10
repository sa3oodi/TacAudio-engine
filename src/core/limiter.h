#pragma once
// Stereo-linked look-ahead peak limiter.
//
//   required gain r[n] = min(1, ceiling / max(|L|,|R|))
//   g = boxAverage_L( release( min over the next L samples of r ) )
//
// The min filter + box filter of the same length guarantee the gain has fully
// reached r at the peak sample, so output never exceeds the ceiling (sample peak).
// Latency: `lookahead` samples.

#include <cstddef>
#include <vector>

namespace tac {

struct LimiterConfig {
    bool enabled = true;
    float ceilingDb = -1.0f;
    float releaseMs = 80.0f;
    std::size_t lookahead = 48;  // 1 ms at 48 kHz
};

class Limiter {
public:
    Limiter(double sampleRate, const LimiterConfig& cfg = {});

    void process(float* left, float* right, std::size_t n) noexcept;
    void reset() noexcept;

    std::size_t latency() const noexcept { return cfg_.enabled ? L_ : 0; }
    // Lowest gain applied since the last call (1 = no limiting).
    float takeMinGain() noexcept {
        const float g = minGain_;
        minGain_ = 1.0f;
        return g;
    }

private:
    LimiterConfig cfg_;
    std::size_t L_;
    float ceiling_, releaseCoef_;
    std::vector<float> delayL_, delayR_, req_, env_;
    std::size_t pos_ = 0;  // shared circular index for delay/req/env (length L)
    float envelope_ = 1.0f;
    double envSum_;
    float minGain_ = 1.0f;
};

}  // namespace tac

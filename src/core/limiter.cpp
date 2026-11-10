#include "core/limiter.h"

#include <algorithm>
#include <cmath>

namespace tac {

Limiter::Limiter(double sampleRate, const LimiterConfig& cfg)
    : cfg_(cfg),
      L_(std::max<std::size_t>(cfg.lookahead, 1)),
      ceiling_(std::pow(10.0f, cfg.ceilingDb / 20.0f)),
      releaseCoef_(static_cast<float>(std::exp(-1.0 / (cfg.releaseMs * 0.001 * sampleRate)))),
      delayL_(L_, 0.0f),
      delayR_(L_, 0.0f),
      req_(L_ + 1, 1.0f),
      env_(L_, 1.0f),
      envSum_(static_cast<double>(L_)) {}

void Limiter::reset() noexcept {
    std::fill(delayL_.begin(), delayL_.end(), 0.0f);
    std::fill(delayR_.begin(), delayR_.end(), 0.0f);
    std::fill(req_.begin(), req_.end(), 1.0f);
    std::fill(env_.begin(), env_.end(), 1.0f);
    envSum_ = static_cast<double>(L_);
    envelope_ = 1.0f;
    pos_ = 0;
}

void Limiter::process(float* left, float* right, std::size_t n) noexcept {
    if (!cfg_.enabled) return;
    const std::size_t reqLen = L_ + 1;
    for (std::size_t i = 0; i < n; ++i) {
        const float xl = left[i], xr = right[i];
        const float peak = std::max(std::abs(xl), std::abs(xr));
        const float r = peak > ceiling_ ? ceiling_ / peak : 1.0f;

        // Window of required gains covering the look-ahead span [n-L, n].
        req_[pos_ % reqLen] = r;
        float m = 1.0f;
        for (float v : req_) m = std::min(m, v);

        // Instant attack to the window minimum, exponential release.
        envelope_ = m < envelope_ ? m : m + (envelope_ - m) * releaseCoef_;

        // Box average of the envelope over L samples (running sum).
        const std::size_t e = pos_ % L_;
        envSum_ += static_cast<double>(envelope_) - env_[e];
        env_[e] = envelope_;
        const float g = static_cast<float>(envSum_ / static_cast<double>(L_));
        minGain_ = std::min(minGain_, g);

        // Delayed audio out, delayed audio in.
        const float dl = delayL_[e], dr = delayR_[e];
        delayL_[e] = xl;
        delayR_[e] = xr;
        left[i] = dl * g;
        right[i] = dr * g;

        ++pos_;
        if (pos_ == L_ * reqLen) pos_ = 0;  // keep both moduli consistent
    }
}

}  // namespace tac

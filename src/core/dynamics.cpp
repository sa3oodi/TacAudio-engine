#include "core/dynamics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tac {

DynamicsParams dynamicsPreset(const std::string& name) {
    DynamicsParams p;
    p.name = name;
    if (name == "off") {
        p.enabled = false;
    } else if (name == "light") {
        p.enabled = true;
        p.thresholdDb = -20.0f;
        p.ratio = 2.5f;
        p.boostBelowDb = -40.0f;
        p.boostRatio = 1.5f;
        p.maxBoostDb = 4.0f;
        p.makeupDb = 2.0f;
        p.releaseMs = 100.0f;
        p.punch = 70.0f;
    } else if (name == "medium") {
        p.enabled = true;  // defaults above
    } else if (name == "strong") {
        p.enabled = true;
        p.thresholdDb = -28.0f;
        p.ratio = 8.0f;
        p.boostBelowDb = -36.0f;
        p.boostRatio = 3.0f;
        p.maxBoostDb = 12.0f;
        p.makeupDb = 6.0f;
        p.releaseMs = 150.0f;
        p.punch = 50.0f;
    } else {
        throw std::invalid_argument("unknown dynamics preset '" + name + "' (off | light | medium | strong)");
    }
    return p;
}

namespace {
float coef(double ms, double fs) { return static_cast<float>(std::exp(-1.0 / (std::max(0.05, ms) * 0.001 * fs))); }
}  // namespace

ChannelDynamics::ChannelDynamics(double fs, std::size_t channels, const DynamicsParams& p)
    : p_(p),
      lookahead_(std::max<std::size_t>(1, static_cast<std::size_t>(p.lookaheadMs * 0.001 * fs))),
      detRelease_(coef(50.0, fs)),  // peak detector release
      ch_(channels) {
    const double punch = std::clamp(static_cast<double>(p.punch), 0.0, 100.0) / 100.0;
    msCoef_ = coef(5.0 + 45.0 * punch, fs);         // loudness window 5..50 ms
    peakMix_ = static_cast<float>(1.0 - punch);      // punch 0 = pure peak, 100 = pure loudness
    attCoef_ = coef(1.0 + 29.0 * punch, fs);         // gain attack 1..30 ms
    relCoef_ = coef(std::max(1.0f, p.releaseMs), fs);
    makeup_ = p.enabled ? std::pow(10.0f, p.makeupDb / 20.0f) : 1.0f;
    stepMax_ = std::pow(10.0f, std::max(0.0f, p.footstepDb) / 20.0f) - 1.0f;  // extra band gain
    stepCoef_ = coef(20.0, fs);                                               // smooth band gain changes
    for (auto& c : ch_) {
        c.delay.assign(lookahead_, 0.0f);
        c.band = Biquad::bandpass(fs, p.footstepHz, p.footstepQ);
    }
}

void ChannelDynamics::reset() noexcept {
    for (auto& c : ch_) {
        std::fill(c.delay.begin(), c.delay.end(), 0.0f);
        c.peak = 0.0f;
        c.ms = 0.0f;
        c.gainDb = 0.0f;
        c.band.reset();
        c.stepGain = 0.0f;
    }
    pos_ = 0;
}

float ChannelDynamics::curveDb(float L) const noexcept {
    float g = 0.0f;
    // Downward, soft knee.
    const float over = L - p_.thresholdDb;
    const float slope = 1.0f - 1.0f / p_.ratio;
    if (over > p_.kneeDb / 2) {
        g = -over * slope;
    } else if (over > -p_.kneeDb / 2) {
        const float x = over + p_.kneeDb / 2;
        g = -slope * x * x / (2.0f * p_.kneeDb);
    }
    // Upward below boostBelowDb, faded out towards the gate.
    if (L < p_.boostBelowDb) {
        const float boost = std::min(p_.maxBoostDb, (p_.boostBelowDb - L) * (1.0f - 1.0f / p_.boostRatio));
        const float fade = std::clamp((L - p_.gateDb) / 6.0f, 0.0f, 1.0f);
        g += boost * fade;
    }
    return g;
}

void ChannelDynamics::process(float* const* io, std::size_t n) noexcept {
    if (!p_.active()) return;
    const bool comp = p_.enabled;
    const bool steps = stepMax_ > 0.0f;
    for (std::size_t c = 0; c < ch_.size(); ++c) {
        Channel& s = ch_[c];
        float* x = io[c];
        std::size_t pos = pos_;
        for (std::size_t i = 0; i < n; ++i) {
            const float in = x[i];
            // Detector on the undelayed signal (look-ahead), gain applied to the delayed one.
            // Blend of an instant peak level and a windowed loudness (RMS) level, set by punch.
            const float a = std::abs(in);
            s.peak = a > s.peak ? a : s.peak * detRelease_;
            s.ms = in * in + (s.ms - in * in) * msCoef_;
            const float peakDb = 20.0f * std::log10(s.peak + 1e-9f);
            const float rmsDb = 10.0f * std::log10(s.ms + 1e-18f);
            const float level = peakMix_ * peakDb + (1.0f - peakMix_) * rmsDb;
            if (comp) {
                const float target = curveDb(level);
                const float coef = target < s.gainDb ? attCoef_ : relCoef_;
                s.gainDb = target + (s.gainDb - target) * coef;
                minGainDb_ = std::min(minGainDb_, s.gainDb);
                maxGainDb_ = std::max(maxGainDb_, s.gainDb);
            }

            const float delayed = s.delay[pos];
            s.delay[pos] = in;
            if (++pos == lookahead_) pos = 0;

            float y = delayed;
            if (steps) {
                // Full band boost while quiet, fading out over the 10 dB above footstepBelowDb.
                const float quiet = std::clamp((p_.footstepBelowDb + 10.0f - level) / 10.0f, 0.0f, 1.0f);
                const float want = stepMax_ * quiet;
                s.stepGain = want + (s.stepGain - want) * stepCoef_;
                y += s.stepGain * s.band.process(delayed);
            }
            x[i] = comp ? y * std::pow(10.0f, s.gainDb / 20.0f) * makeup_ : y;
        }
    }
    pos_ = (pos_ + n) % lookahead_;
}

float ChannelDynamics::takeMinGainDb() noexcept {
    const float v = minGainDb_;
    minGainDb_ = 0.0f;
    return v;
}
float ChannelDynamics::takeMaxGainDb() noexcept {
    const float v = maxGainDb_;
    maxGainDb_ = 0.0f;
    return v;
}

}  // namespace tac

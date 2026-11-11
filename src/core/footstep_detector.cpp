#include "core/footstep_detector.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace tac {

namespace {
float coef(double ms, double fs) { return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * fs))); }

// Walk rules (see the header).
constexpr double kMinGapSec = 0.22, kMaxGapSec = 1.0;   // between two steps
constexpr double kGapRatio = 1.43;                      // each gap within 0.7-1.43x of the previous
constexpr float kMaxTurnDeg = 60.0f;                    // a walker moves slowly around you
constexpr double kQuietSec = 0.12;                      // an impact ends after this long without an onset
constexpr double kMaxImpactSec = 0.2;                   // longer = it keeps hitting (fire, rattle)
constexpr float kCentreDeg = 15.0f;                     // straight ahead = your own sounds
constexpr int kStepsToShow = 3;

float angleDiff(float a, float b) {
    float d = std::fmod(std::abs(a - b), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}
}  // namespace

FootstepDetector::FootstepDetector(double fs)
    : hp_(Biquad::highpass(fs, 1500.0)),
      lp_(Biquad::lowpass(fs, 6000.0)),
      low_(Biquad::lowpass(fs, 250.0)),
      cFast_(coef(5.0, fs)),
      cSlow_(coef(300.0, fs)),
      c20_(coef(20.0, fs)),
      cPeak_(coef(50.0, fs)) {}

void FootstepDetector::reset() noexcept {
    hp_.reset();
    lp_.reset();
    low_.reset();
    fast_ = slow_ = bandE_ = lowE_ = peak_ = cand_ = 0.0f;
    tick_ = 0;
    std::fill(std::begin(onsetHist_), std::end(onsetHist_), 0.0f);
    histPos_ = 0;
}

bool FootstepDetector::process(const float* x, std::size_t n) noexcept {
    bool decided = false;
    for (std::size_t i = 0; i < n; ++i) {
        const float s = x[i];
        const float b = lp_.process(hp_.process(s));
        const float l = low_.process(s);
        const float eb = b * b, el = l * l;
        fast_ = eb + (fast_ - eb) * cFast_;
        slow_ = eb + (slow_ - eb) * cSlow_;
        bandE_ = eb + (bandE_ - eb) * c20_;
        lowE_ = el + (lowE_ - el) * c20_;
        const float a = std::abs(s);
        peak_ = a > peak_ ? a : peak_ * cPeak_;

        if (++tick_ < kDecim) continue;
        tick_ = 0;
        decided = true;
        const float jumpDb = 10.0f * std::log10((fast_ + 1e-14f) / (slow_ + 1e-14f));
        const float peakDb = 20.0f * std::log10(peak_ + 1e-9f);
        const float tiltDb = 10.0f * std::log10((bandE_ + 1e-14f) / (lowE_ + 1e-14f));
        const float onset = std::clamp((jumpDb - 4.0f) / 6.0f, 0.0f, 1.0f);
        const float notLoud = std::clamp((-14.0f - peakDb) / 6.0f, 0.0f, 1.0f);   // full below -20 dBFS
        // Steps have a heel thump too, so some low end is fine; explosions / vehicles are 30+ dB bass-heavy.
        const float crunchy = std::clamp((tiltDb + 20.0f) / 8.0f, 0.0f, 1.0f);
        const float audible = std::clamp((peakDb + 66.0f) / 6.0f, 0.0f, 1.0f);    // above -60 dBFS
        const float delayedOnset = onsetHist_[histPos_];
        onsetHist_[histPos_] = onset * audible;
        histPos_ = (histPos_ + 1) % kDelay;
        cand_ = delayedOnset * notLoud * crunchy;
    }
    return decided;
}

FootstepTracker::FootstepTracker(double fs, const std::vector<float>& azimuthDeg, const std::vector<bool>& lfe)
    : n_(std::min({azimuthDeg.size(), lfe.size(), kMaxChannels})),
      tickRate_(fs / FootstepDetector::kDecim),
      det_(n_, FootstepDetector(fs)),
      cRel_(coef(250.0, fs / FootstepDetector::kDecim)) {
    for (std::size_t c = 0; c < n_; ++c) {
        const float r = azimuthDeg[c] * std::numbers::pi_v<float> / 180.0f;
        azDeg_[c] = azimuthDeg[c];
        cosAz_[c] = std::cos(r);
        sinAz_[c] = std::sin(r);
        lfe_[c] = lfe[c];
        if (!lfe[c]) ++nonLfe_;
    }
}

void FootstepTracker::reset() noexcept {
    for (auto& d : det_) d.reset();
    phase_ = 0;
    now_ = 0;
    high_ = inImpact_ = false;
    impactStart_ = lastRise_ = 0;
    evE_.fill(0.0f);
    walks_ = {};
    act_.fill(0.0f);
    max_.fill(0.0f);
    impacts_ = steps_ = 0;
}

float FootstepTracker::takeMax(std::size_t c) noexcept {
    if (c >= n_) return 0.0f;
    const float m = max_[c];
    max_[c] = act_[c];
    return m;
}

void FootstepTracker::process(const float* const* ch, std::size_t frames) noexcept {
    if (n_ == 0) return;
    std::size_t off = 0;
    while (off < frames) {
        // All channels advance together, one decision (kDecim samples) at a time.
        const std::size_t len = std::min<std::size_t>(frames - off, static_cast<std::size_t>(FootstepDetector::kDecim - phase_));
        for (std::size_t c = 0; c < n_; ++c) det_[c].process(ch[c] + off, len);
        phase_ += static_cast<int>(len);
        off += len;
        if (phase_ == FootstepDetector::kDecim) {
            phase_ = 0;
            decide();
        }
    }
}

void FootstepTracker::decide() noexcept {
    ++now_;
    float cand = 0.0f;
    for (std::size_t c = 0; c < n_; ++c)
        if (!lfe_[c]) cand = std::max(cand, det_[c].candidate());

    // Onsets (with hysteresis) are grouped into impacts.
    if (!high_ && cand >= 0.5f) {
        high_ = true;
        if (!inImpact_) {
            inImpact_ = true;
            impactStart_ = now_;
            evE_.fill(0.0f);
        }
        lastRise_ = now_;
    } else if (high_ && cand < 0.25f) {
        high_ = false;
    }
    if (inImpact_) {
        // What the impact added on each speaker, over its first 40 ms (its direction and spread).
        if (static_cast<double>(now_ - impactStart_) < 0.04 * tickRate_)
            for (std::size_t c = 0; c < n_; ++c)
                if (!lfe_[c]) evE_[c] = std::max(evE_[c], det_[c].excessEnergy());
        if (!high_ && static_cast<double>(now_ - lastRise_) > kQuietSec * tickRate_) {
            inImpact_ = false;
            classify();
        }
    }

    for (std::size_t c = 0; c < n_; ++c) {
        act_[c] *= cRel_;
        max_[c] = std::max(max_[c], act_[c]);
    }
}

void FootstepTracker::classify() noexcept {
    ++impacts_;
    if (static_cast<double>(lastRise_ - impactStart_) > kMaxImpactSec * tickRate_) return;  // keeps hitting

    float total = 0.0f, peak = 0.0f, x = 0.0f, y = 0.0f;
    for (std::size_t c = 0; c < n_; ++c) {
        total += evE_[c];
        peak = std::max(peak, evE_[c]);
        x += evE_[c] * cosAz_[c];
        y += evE_[c] * sinAz_[c];
    }
    if (peak <= 0.0f) return;
    const float az = std::atan2(y, x) * 180.0f / std::numbers::pi_v<float>;
    const float focus = std::sqrt(x * x + y * y) / total;  // 1 = one direction, 0 = all around

    // Your own sounds: straight ahead, or spread evenly over the speakers.
    int loudSpk = 0;
    for (std::size_t c = 0; c < n_; ++c)
        if (!lfe_[c] && evE_[c] >= 0.25f * peak) ++loudSpk;
    const bool everywhere = nonLfe_ >= 4 && (loudSpk >= nonLfe_ - 1 || focus < 0.3f);
    if (everywhere || angleDiff(az, 0.0f) < kCentreDeg) return;

    // Join the walk it continues (same direction, a step's gap, the same pace), else start one.
    const std::int64_t start = impactStart_;
    Walk* best = nullptr;
    for (auto& w : walks_) {
        if (w.last < 0) continue;
        const double gap = static_cast<double>(start - w.last);
        if (gap < kMinGapSec * tickRate_ || gap > kMaxGapSec * tickRate_) continue;
        if (angleDiff(az, w.az) > kMaxTurnDeg) continue;
        if (w.gap > 0) {
            const double ratio = gap / static_cast<double>(w.gap);
            if (ratio > kGapRatio || ratio < 1.0 / kGapRatio) continue;
        }
        if (!best || w.count > best->count) best = &w;
    }
    if (best) {
        best->gap = start - best->last;
        best->last = start;
        best->az = az;
        ++best->count;
    } else {
        // Replace an expired walk, or the oldest.
        Walk* slot = &walks_[0];
        for (auto& w : walks_)
            if (w.last < slot->last) slot = &w;
        *slot = Walk{start, 0, az, 1};
        best = slot;
    }
    // Expire walks that stopped.
    for (auto& w : walks_)
        if (w.last >= 0 && static_cast<double>(now_ - w.last) > (kMaxGapSec + 0.4) * tickRate_) w.last = -1;
    if (best->count < kStepsToShow) return;

    // A footstep: light the speakers it came from.
    ++steps_;
    for (std::size_t c = 0; c < n_; ++c) {
        if (lfe_[c]) continue;
        const float w = std::sqrt(evE_[c] / peak);  // amplitude share
        act_[c] = std::max(act_[c], std::min(1.0f, w));
        max_[c] = std::max(max_[c], act_[c]);
    }
}

}  // namespace tac

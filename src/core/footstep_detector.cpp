#include "core/footstep_detector.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace tac {

namespace {
float coef(double ms, double fs) { return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * fs))); }

// Walk rules (see the header).
constexpr double kMinGapSec = 0.2, kMaxGapSec = 1.2;    // between two steps
constexpr double kGapRatio = 1.43;                      // steady pace: each gap within 0.7-1.43x of the last
constexpr float kMaxTurnDeg = 60.0f;                    // a walker moves slowly around you
constexpr double kMeasureSec = 0.04, kMergeSec = 0.15;  // one impact
constexpr float kCentreDeg = 12.0f;                     // straight ahead = your own sounds
constexpr float kMinFocus = 0.25f;                      // less = no clear direction
constexpr double kRiseMergeSec = 0.035, kBurstGapSec = 0.2, kBurstHoldSec = 0.5;

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
        const float onset = std::clamp((jumpDb - 3.0f) / 5.0f, 0.0f, 1.0f);       // full at +8 dB
        // Only near-full-scale hits (your own gun, explosions on top of you) are too loud: close
        // enemies in a loud game mix can peak at -10 dBFS.
        const float notLoud = std::clamp((-3.0f - peakDb) / 5.0f, 0.0f, 1.0f);    // full below -8 dBFS
        // Steps have a heel thump too, so some low end is fine; explosions / vehicles are 30+ dB bass-heavy.
        const float crunchy = std::clamp((tiltDb + 20.0f) / 8.0f, 0.0f, 1.0f);
        const float audible = std::clamp((peakDb + 66.0f) / 6.0f, 0.0f, 1.0f);    // above -60 dBFS
        const float delayedOnset = onsetHist_[histPos_];
        onsetHist_[histPos_] = onset * audible;
        histPos_ = (histPos_ + 1) % kDelay;
        cand_ = delayedOnset * notLoud * crunchy;
        dbg_ = Debug{jumpDb, peakDb, tiltDb, onset, notLoud, crunchy, audible};
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
        cosAz_[c] = std::cos(r);
        sinAz_[c] = std::sin(r);
        lfe_[c] = lfe[c];
    }
}

void FootstepTracker::reset() noexcept {
    for (auto& d : det_) d.reset();
    phase_ = 0;
    now_ = 0;
    high_ = measuring_ = false;
    impactStart_ = lastRise_ = -1000000;
    vetoUntil_ = 0;
    shortRun_ = 0;
    evE_.fill(0.0f);
    walks_ = {};
    act_ = {};
    max_ = {};
    total_ = fresh_ = {};
}

float FootstepTracker::takeMax(std::size_t c, int level) noexcept {
    if (c >= n_ || level < 0 || level >= kLevels) return 0.0f;
    const float m = max_[level][c];
    max_[level][c] = act_[level][c];
    return m;
}

void FootstepTracker::process(const float* const* ch, std::size_t frames) noexcept {
    if (n_ == 0) return;
    std::size_t off = 0;
    while (off < frames) {
        // All channels advance together, one decision (kDecim samples) at a time.
        const std::size_t len =
            std::min<std::size_t>(frames - off, static_cast<std::size_t>(FootstepDetector::kDecim - phase_));
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

    bool rise = false;
    if (!high_ && cand >= 0.4f) {
        high_ = rise = true;
        const double gap = static_cast<double>(now_ - lastRise_) / tickRate_;
        if (gap >= kRiseMergeSec) {
            shortRun_ = gap <= kBurstGapSec ? shortRun_ + 1 : 0;
            lastRise_ = now_;
            if (shortRun_ >= 2) vetoUntil_ = now_ + static_cast<std::int64_t>(kBurstHoldSec * tickRate_);
        }
    } else if (high_ && cand < 0.2f) {
        high_ = false;
    }
    if (rise && !measuring_ && static_cast<double>(now_ - impactStart_) >= kMergeSec * tickRate_) {
        measuring_ = true;
        impactStart_ = now_;
        evE_.fill(0.0f);
    }
    if (measuring_) {
        // What the impact added on each speaker (its direction and spread).
        for (std::size_t c = 0; c < n_; ++c)
            if (!lfe_[c]) evE_[c] = std::max(evE_[c], det_[c].excessEnergy());
        if (static_cast<double>(now_ - impactStart_) >= kMeasureSec * tickRate_) {
            measuring_ = false;
            classify();
        }
    }

    for (int l = 0; l < kLevels; ++l)
        for (std::size_t c = 0; c < n_; ++c) {
            act_[l][c] *= cRel_;
            max_[l][c] = std::max(max_[l][c], act_[l][c]);
        }
}

void FootstepTracker::light(int level) noexcept {
    float peak = 0.0f;
    for (std::size_t c = 0; c < n_; ++c) peak = std::max(peak, evE_[c]);
    if (peak <= 0.0f) return;
    for (std::size_t c = 0; c < n_; ++c) {
        if (lfe_[c]) continue;
        const float w = std::min(1.0f, std::sqrt(evE_[c] / peak));  // amplitude share
        act_[level][c] = std::max(act_[level][c], w);
        max_[level][c] = std::max(max_[level][c], act_[level][c]);
    }
}

void FootstepTracker::classify() noexcept {
    float total = 0.0f, peak = 0.0f, x = 0.0f, y = 0.0f;
    for (std::size_t c = 0; c < n_; ++c) {
        total += evE_[c];
        peak = std::max(peak, evE_[c]);
        x += evE_[c] * cosAz_[c];
        y += evE_[c] * sinAz_[c];
    }
    if (peak <= 0.0f) return;
    count(&Stats::impacts);
    const float az = std::atan2(y, x) * 180.0f / std::numbers::pi_v<float>;
    const float focus = std::sqrt(x * x + y * y) / total;  // 1 = one direction, 0 = all around
    auto log = [&](Verdict v) {
        if (log_)
            log_->push_back(Event{static_cast<double>(impactStart_) / tickRate_, az, focus,
                                  10.0f * std::log10(peak + 1e-20f), v});
    };

    // Your own sounds: straight ahead, or no clear direction.
    if (focus < kMinFocus || angleDiff(az, 0.0f) < kCentreDeg) {
        count(&Stats::own);
        log(Verdict::Own);
        return;
    }
    light(0);
    if (now_ < vetoUntil_) {  // a burst: not walking
        count(&Stats::single);
        log(Verdict::Burst);
        return;
    }

    // The walk it continues: same direction, a step's gap.
    const std::int64_t t = impactStart_;
    for (auto& w : walks_)  // stopped walking
        if (w.last >= 0 && static_cast<double>(t - w.last) > kMaxGapSec * tickRate_) w.last = -1;
    Walk* best = nullptr;
    for (auto& w : walks_) {
        if (w.last < 0) continue;
        const double gap = static_cast<double>(t - w.last);
        if (gap < kMinGapSec * tickRate_ || angleDiff(az, w.az) > kMaxTurnDeg) continue;
        if (!best || w.last > best->last) best = &w;
    }
    if (best) {
        const std::int64_t gap = t - best->last;
        double k = 0.0;  // steps this gap spans at the walk's pace (0 = not steady)
        if (best->gap > 0)
            for (double m : {1.0, 2.0}) {  // once walking steadily, one missed step is allowed
                if (m > 1.0 && best->steady < 3) break;
                const double ratio = static_cast<double>(gap) / (static_cast<double>(best->gap) * m);
                if (ratio <= kGapRatio && ratio >= 1.0 / kGapRatio) {
                    k = m;
                    break;
                }
            }
        best->steady = k > 0.0 ? std::max(best->steady, 2) + 1 : 2;
        best->gap = k > 0.0 ? static_cast<std::int64_t>(static_cast<double>(gap) / k) : gap;
        best->last = t;
        best->az = az;
        ++best->count;
    } else {
        Walk* slot = &walks_[0];
        for (auto& w : walks_)
            if (w.last < slot->last) slot = &w;
        *slot = Walk{t, 0, az, 1, 1};
        best = slot;
        count(&Stats::single);
    }

    if (best->count >= 2) {
        light(1);
        count(&Stats::balanced);
    }
    if (best->steady >= 3) {
        light(2);
        count(&Stats::strict);
    }
    log(best->steady >= 3 ? Verdict::Steady : best->count >= 2 ? Verdict::Walk : Verdict::First);
}

}  // namespace tac

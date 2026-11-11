#include "core/footstep_detector.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace tac {

namespace {
float coef(double ms, double fs) { return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * fs))); }

// Walk rules (see the header).
constexpr double kMinGapSec = 0.1, kMaxGapSec = 1.2;    // between two steps (sprinting can be 0.12 s)
constexpr double kGapRatio = 1.43;                      // steady pace: each gap within 0.7-1.43x of the last
constexpr float kMaxTurnDeg = 60.0f;                    // a walker moves slowly around you
constexpr double kMeasureSec = 0.04, kMergeSec = 0.15;  // one impact
constexpr float kCentreDeg = 12.0f;                     // straight ahead = your own sounds
constexpr float kMinFocus = 0.25f;                      // less = no clear direction
constexpr double kRiseMergeSec = 0.035, kBurstGapSec = 0.15, kBurstHoldSec = 0.5;
constexpr double kConfirmSec = 0.25;  // Balanced / Strict wait this long for a burst to show
constexpr double kFadeFromSec = 0.1;   // a step has faded (10 dB) somewhere 0.1-0.25 s after it starts
constexpr float kFadeRatio = 10.0f;
constexpr int kBurstGaps = 3;
constexpr double kBurstEven = 0.12;  // burst gaps within 12 % of each other
constexpr std::int64_t kLongAgo = -1000000;

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
      cPeak_(coef(50.0, fs)),
      cFloorUp_(coef(500.0, fs / kDecim)),
      cFloorDown_(coef(30.0, fs / kDecim)) {}

void FootstepDetector::reset() noexcept {
    hp_.reset();
    lp_.reset();
    low_.reset();
    fast_ = slow_ = bandE_ = lowE_ = peak_ = cand_ = floor_ = lowFloor_ = 0.0f;
    tick_ = 0;
    std::fill(std::begin(onsetHist_), std::end(onsetHist_), 0.0f);
    std::fill(std::begin(floorHist_), std::end(floorHist_), 0.0f);
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
        // Background = the quiet level between sounds: falls fast (30 ms), rises slowly (0.5 s), so
        // a run of steps does not raise it but steady rain / wind does.
        floor_ = fast_ + (floor_ - fast_) * (fast_ < floor_ ? cFloorDown_ : cFloorUp_);
        lowFloor_ = lowE_ + (lowFloor_ - lowE_) * (lowE_ < lowFloor_ ? cFloorDown_ : cFloorUp_);
        const float jumpDb = 10.0f * std::log10((fast_ + 1e-14f) / (floor_ + 1e-14f));
        const float peakDb = 20.0f * std::log10(peak_ + 1e-9f);
        // What the sound added in the crunch band vs in the bass (a steady rumble on the same
        // speaker does not count against a step).
        const float addBand = std::max(0.0f, bandE_ - floor_), addLow = std::max(0.0f, lowE_ - lowFloor_);
        const float tiltDb = 10.0f * std::log10((addBand + 1e-14f) / (addLow + 1e-3f * lowFloor_ + 1e-14f));
        const float onset = std::clamp((jumpDb - 3.0f) / 5.0f, 0.0f, 1.0f);       // full at +8 dB
        // Only near-full-scale hits (your own gun, explosions on top of you) are too loud: close
        // enemies in a loud game mix can peak at -10 dBFS.
        const float notLoud = std::clamp((-3.0f - peakDb) / 5.0f, 0.0f, 1.0f);    // full below -8 dBFS
        // Steps have a heel thump too, so some low end is fine; explosions / vehicles are 30+ dB bass-heavy.
        const float crunchy = std::clamp((tiltDb + 20.0f) / 8.0f, 0.0f, 1.0f);
        const float audible = std::clamp((peakDb + 66.0f) / 6.0f, 0.0f, 1.0f);    // above -60 dBFS
        const float delayedOnset = onsetHist_[histPos_];
        onsetHist_[histPos_] = onset * audible;
        floorHist_[histPos_] = floor_;
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
    lastImpact_.fill(kLongAgo);
    lastRise_.fill(kLongAgo);
    burstAt_.fill(kLongAgo);
}

void FootstepTracker::reset() noexcept {
    for (auto& d : det_) d.reset();
    phase_ = 0;
    now_ = 0;
    high_.fill(false);
    part_.fill(false);
    lastImpact_.fill(kLongAgo);
    lastRise_.fill(kLongAgo);
    vetoUntil_.fill(0);
    shortRun_.fill(0);
    burstAt_.fill(kLongAgo);
    prevGap_.fill(0.0);
    pendHead_ = pendCount_ = 0;
    measuring_ = false;
    impactStart_ = 0;
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
    const auto merge = static_cast<std::int64_t>(kMergeSec * tickRate_);
    for (std::size_t c = 0; c < n_; ++c) {
        if (lfe_[c]) continue;
        const float cand = det_[c].candidate();
        bool rise = false;
        if (!high_[c] && cand >= 0.4f) {
            high_[c] = rise = true;
            const double gap = static_cast<double>(now_ - lastRise_[c]) / tickRate_;
            if (gap >= kRiseMergeSec) {
                // A gun fires at a fixed rate; sprinting steps are just as fast but uneven.
                const bool even = prevGap_[c] > 0.0 && std::abs(gap / prevGap_[c] - 1.0) <= kBurstEven;
                shortRun_[c] = gap > kBurstGapSec ? 0 : even ? shortRun_[c] + 1 : 1;
                prevGap_[c] = gap;
                lastRise_[c] = now_;
                if (shortRun_[c] >= kBurstGaps) {
                    vetoUntil_[c] = now_ + static_cast<std::int64_t>(kBurstHoldSec * tickRate_);
                    burstAt_[c] = now_;
                }
            }
        } else if (high_[c] && cand < 0.2f) {
            high_[c] = false;
        }
        if (rise && now_ - lastImpact_[c] >= merge) {
            if (!measuring_) {
                measuring_ = true;
                impactStart_ = now_;
                evE_.fill(0.0f);
                part_.fill(false);
            }
            part_[c] = true;  // this speaker carries the impact
            lastImpact_[c] = now_;
            startFloor_[c] = det_[c].onsetFloor();
        }
        if (measuring_ && cand >= 0.2f && !part_[c] && now_ - lastImpact_[c] >= merge) {
            part_[c] = true;  // the same sound, panned between two speakers
            lastImpact_[c] = now_;
            startFloor_[c] = det_[c].onsetFloor();
        }
    }
    if (measuring_) {
        // What the impact added on the speakers that carry it (its direction and spread).
        for (std::size_t c = 0; c < n_; ++c)
            if (part_[c]) evE_[c] = std::max(evE_[c], det_[c].excessEnergy());
        if (static_cast<double>(now_ - impactStart_) >= kMeasureSec * tickRate_) {
            measuring_ = false;
            classify();
        }
    }
    for (std::size_t i = 0; i < pendCount_; ++i) {  // does it fade like a step?
        Pending& p = pending_[(pendHead_ + i) % pending_.size()];
        if (static_cast<double>(now_ - p.start) >= kFadeFromSec * tickRate_)
            p.minAfter = std::min(p.minAfter, det_[p.dom].bandEnergy());
    }
    while (pendCount_ > 0 &&
           static_cast<double>(now_ - pending_[pendHead_].start) >= kConfirmSec * tickRate_) {
        confirm(pending_[pendHead_]);
        pendHead_ = (pendHead_ + 1) % pending_.size();
        --pendCount_;
    }

    for (int l = 0; l < kLevels; ++l)
        for (std::size_t c = 0; c < n_; ++c) {
            act_[l][c] *= cRel_;
            max_[l][c] = std::max(max_[l][c], act_[l][c]);
        }
}

void FootstepTracker::light(int level, const Levels& e) noexcept {
    float peak = 0.0f;
    for (std::size_t c = 0; c < n_; ++c) peak = std::max(peak, e[c]);
    if (peak <= 0.0f) return;
    for (std::size_t c = 0; c < n_; ++c) {
        if (lfe_[c]) continue;
        const float w = std::min(1.0f, std::sqrt(e[c] / peak));  // amplitude share
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
    const float levelDb = 10.0f * std::log10(peak + 1e-20f);

    // Your own sounds: straight ahead, or no clear direction.
    if (focus < kMinFocus || angleDiff(az, 0.0f) < kCentreDeg) {
        count(&Stats::own);
        if (log_) log_->push_back(Event{static_cast<double>(impactStart_) / tickRate_, az, focus, levelDb, Verdict::Own});
        return;
    }
    light(0, evE_);  // Sensitive: at once
    std::size_t dom = 0;
    for (std::size_t c = 0; c < n_; ++c)
        if (evE_[c] > evE_[dom]) dom = c;
    if (pendCount_ == pending_.size()) {  // full (cannot happen at real impact rates): decide the oldest now
        confirm(pending_[pendHead_]);
        pendHead_ = (pendHead_ + 1) % pending_.size();
        --pendCount_;
    }
    pending_[(pendHead_ + pendCount_) % pending_.size()] =
        Pending{impactStart_, az, focus, levelDb, dom, evE_, startFloor_[dom], 3.0e38f};
    ++pendCount_;
}

void FootstepTracker::confirm(const Pending& p) noexcept {
    const float az = p.az;
    auto log = [&](Verdict v) {
        if (log_) log_->push_back(Event{static_cast<double>(p.start) / tickRate_, az, p.focus, p.levelDb, v});
    };
    // A burst on its speaker (already going, or seen since): not walking.
    if (p.start < vetoUntil_[p.dom] || burstAt_[p.dom] >= p.start) {
        count(&Stats::single);
        log(Verdict::Burst);
        return;
    }
    // A sound that keeps going (rain or wind starting, an engine): not a step.
    if (p.minAfter > kFadeRatio * p.floor0 + 1e-12f) {
        count(&Stats::single);
        log(Verdict::Sustained);
        return;
    }

    // The walk it continues: same direction, a step's gap.
    const std::int64_t t = p.start;
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
        light(1, p.e);
        count(&Stats::balanced);
    }
    if (best->steady >= 3) {
        light(2, p.e);
        count(&Stats::strict);
    }
    log(best->steady >= 3 ? Verdict::Steady : best->count >= 2 ? Verdict::Walk : Verdict::First);
}

}  // namespace tac

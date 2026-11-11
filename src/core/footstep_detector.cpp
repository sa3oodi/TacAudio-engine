#include "core/footstep_detector.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace tac {

namespace {
float coef(double ms, double fs) { return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * fs))); }
constexpr int kDecim = 16;  // the decision is made every 16 samples (0.33 ms)
}  // namespace

FootstepDetector::FootstepDetector(double fs)
    : hp_(Biquad::highpass(fs, 1500.0)),
      lp_(Biquad::lowpass(fs, 6000.0)),
      low_(Biquad::lowpass(fs, 250.0)),
      cFast_(coef(5.0, fs)),
      cSlow_(coef(300.0, fs)),
      c20_(coef(20.0, fs)),
      cPeak_(coef(50.0, fs)),
      cAtt_(coef(8.0, fs / kDecim)),  // needs ~10 ms of footstep-like sound (not a single click)
      cRel_(coef(250.0, fs / kDecim)) {}

void FootstepDetector::reset() noexcept {
    hp_.reset();
    lp_.reset();
    low_.reset();
    fast_ = slow_ = bandE_ = lowE_ = peak_ = act_ = max_ = want_ = 0.0f;
    tick_ = 0;
    std::fill(std::begin(onsetHist_), std::end(onsetHist_), 0.0f);
    histPos_ = 0;
}

void FootstepDetector::process(const float* x, std::size_t n) noexcept {
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
        const float want = delayedOnset * notLoud * crunchy;
        act_ = want + (act_ - want) * (want > act_ ? cAtt_ : cRel_);
        max_ = std::max(max_, act_);
    }
}

}  // namespace tac

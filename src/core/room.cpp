#include "core/room.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace tac {

RoomParams roomPreset(const std::string& name) {
    RoomParams p;
    p.name = name;
    if (name == "neutral" || name == "off" || name == "dry") {
        p.name = "neutral";
        p.enabled = false;
    } else if (name == "balanced") {
        p.enabled = true;
        p.size = 1.0f;
        p.preDelayMs = 6.0f;
        p.earlyDb = -15.0f;
        p.lateDb = -21.0f;
        p.rt60 = 0.28f;
        p.dampingHz = 6000.0f;
    } else if (name == "spacious") {
        p.enabled = true;
        p.size = 1.5f;
        p.preDelayMs = 10.0f;
        p.earlyDb = -13.0f;
        p.lateDb = -16.0f;
        p.rt60 = 0.5f;
        p.dampingHz = 5000.0f;
        p.width = 1.2f;
    } else {
        throw std::invalid_argument("unknown room preset '" + name + "' (neutral | balanced | spacious)");
    }
    return p;
}

namespace {
float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
}  // namespace

RoomReverb::RoomReverb(double fs, const RoomParams& params) : p_(params) {
    lowCutL_ = lowCutR_ = Biquad::highpass(fs, p_.lowCutHz);
    highCutL_ = highCutR_ = Biquad::lowpass(fs, std::min<double>(p_.highCutHz, 0.45 * fs));

    auto ms = [&](double t) { return std::max<std::size_t>(1, static_cast<std::size_t>(t * p_.size * 0.001 * fs)); };

    // Early reflections: same-side and opposite-side taps with slightly different times per ear,
    // so the two ears get decorrelated reflection patterns (externalisation without smearing).
    earlyLL_ = {{ms(7.3), 0.55f}, {ms(11.9), 0.42f}, {ms(17.1), 0.33f}, {ms(23.9), 0.22f}};
    earlyRL_ = {{ms(9.7), 0.38f}, {ms(14.3), 0.30f}, {ms(21.1), 0.24f}};
    earlyRR_ = {{ms(8.1), 0.55f}, {ms(12.7), 0.42f}, {ms(18.3), 0.33f}, {ms(25.3), 0.22f}};
    earlyLR_ = {{ms(10.3), 0.38f}, {ms(15.1), 0.30f}, {ms(22.7), 0.24f}};
    double tapEnergy = 0;
    for (const auto* v : {&earlyLL_, &earlyRL_})
        for (const auto& t : *v) tapEnergy += static_cast<double>(t.gain) * t.gain;
    earlyGain_ = dbToGain(p_.earlyDb) / static_cast<float>(std::sqrt(tapEnergy));  // normalise reflection energy

    preDelay_ = ms(25.0) + static_cast<std::size_t>(p_.preDelayMs * 0.001 * fs);  // late starts after the early part
    std::size_t maxTap = preDelay_;
    for (const auto* v : {&earlyLL_, &earlyRL_, &earlyRR_, &earlyLR_})
        for (const auto& t : *v) maxTap = std::max(maxTap, t.delay);
    inL_.init(maxTap + 1);
    inR_.init(maxTap + 1);

    // Late reverb: 4 delay lines with mutually prime-ish lengths.
    const double lineMs[4] = {29.7, 37.1, 41.3, 43.9};
    double gSq = 0;
    for (int i = 0; i < 4; ++i) {
        const std::size_t len = ms(lineMs[i]);
        lines_[i].init(len);
        // Per-pass gain so the loop decays 60 dB in rt60 seconds.
        lineGain_[i] = static_cast<float>(std::pow(10.0, -3.0 * static_cast<double>(len) / (p_.rt60 * fs)));
        gSq += static_cast<double>(lineGain_[i]) * lineGain_[i] / 4.0;
    }
    dampCoef_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * p_.dampingHz / fs));
    lateIn_ = static_cast<float>(std::sqrt(std::max(1e-6, 1.0 - gSq)));  // steady-state energy ~ input energy
    lateGain_ = dbToGain(p_.lateDb);
}

void RoomReverb::reset() noexcept {
    for (auto* d : {&inL_, &inR_}) std::fill(d->buf.begin(), d->buf.end(), 0.0f);
    for (auto& l : lines_) std::fill(l.buf.begin(), l.buf.end(), 0.0f);
    damp_.fill(0.0f);
    lowCutL_.reset();
    lowCutR_.reset();
    highCutL_.reset();
    highCutR_.reset();
}

void RoomReverb::process(const float* inL, const float* inR, float* outL, float* outR, std::size_t n) noexcept {
    if (!p_.enabled) return;
    for (std::size_t i = 0; i < n; ++i) {
        inL_.push(highCutL_.process(lowCutL_.process(inL[i])));
        inR_.push(highCutR_.process(lowCutR_.process(inR[i])));

        // Early reflections.
        float eL = 0, eR = 0;
        for (const auto& t : earlyLL_) eL += t.gain * inL_.tap(t.delay);
        for (const auto& t : earlyRL_) eL += t.gain * inR_.tap(t.delay);
        for (const auto& t : earlyRR_) eR += t.gain * inR_.tap(t.delay);
        for (const auto& t : earlyLR_) eR += t.gain * inL_.tap(t.delay);
        eL *= earlyGain_;
        eR *= earlyGain_;

        // Late reverb (FDN): read, damp, attenuate, mix through a normalised 4x4 Hadamard matrix.
        const float xl = inL_.tap(preDelay_) * lateIn_, xr = inR_.tap(preDelay_) * lateIn_;
        float y[4];
        for (int k = 0; k < 4; ++k) {
            const float raw = lines_[k].tap(lines_[k].buf.size());
            damp_[k] = raw + (damp_[k] - raw) * dampCoef_;  // one-pole low-pass: highs die faster
            y[k] = damp_[k] * lineGain_[k];
        }
        const float h0 = 0.5f * (y[0] + y[1] + y[2] + y[3]);
        const float h1 = 0.5f * (y[0] - y[1] + y[2] - y[3]);
        const float h2 = 0.5f * (y[0] + y[1] - y[2] - y[3]);
        const float h3 = 0.5f * (y[0] - y[1] - y[2] + y[3]);
        lines_[0].push(h0 + xl);
        lines_[1].push(h1 + xr);
        lines_[2].push(h2 + xl);
        lines_[3].push(h3 - xr);
        const float lL = (y[0] + y[2]) * lateGain_;
        const float lR = (y[1] + y[3]) * lateGain_;

        // Wet = early + late, with M/S width.
        float wL = eL + lL, wR = eR + lR;
        const float mid = 0.5f * (wL + wR), side = 0.5f * (wL - wR) * p_.width;
        outL[i] += mid + side;
        outR[i] += mid - side;
    }
}

}  // namespace tac

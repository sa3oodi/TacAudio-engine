#include "core/scene.h"

#include <algorithm>
#include <cmath>

#include "core/biquad.h"
#include "core/panner.h"

namespace tac {

namespace {

struct Rng {
    std::uint32_t s = 0x12345678u;
    float next() noexcept {  // white noise in [-1, 1)
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 2147483648.0f - 1.0f;
    }
};

// One footstep: heel thump + toe/scuff crunch ~55 ms later.
std::vector<float> synthFootstep(double fs, Rng& rng, float variation) {
    const std::size_t len = static_cast<std::size_t>(0.25 * fs);
    std::vector<float> out(len, 0.0f);

    auto hit = [&](double offsetSec, double decaySec, Biquad filt, float amp) {
        const std::size_t off = static_cast<std::size_t>(offsetSec * fs);
        for (std::size_t n = off; n < len; ++n) {
            const double t = static_cast<double>(n - off) / fs;
            const float env = static_cast<float>(std::exp(-t / decaySec) * std::min(1.0, t * fs / 24.0));
            out[n] += amp * env * filt.process(rng.next());
        }
    };
    hit(0.000, 0.018, Biquad::lowpass(fs, 380.0 + 60.0 * variation, 1.2), 0.9f);   // heel thump
    hit(0.004, 0.030, Biquad::bandpass(fs, 2600.0 + 500.0 * variation, 0.9), 0.55f);  // gravel crunch
    hit(0.055, 0.035, Biquad::bandpass(fs, 3200.0 - 400.0 * variation, 0.8), 0.45f);  // toe scuff
    return out;
}

// A gunshot: broadband crack + low boom. Much louder than footsteps, like in games.
std::vector<float> synthGunshot(double fs, Rng& rng) {
    const std::size_t len = static_cast<std::size_t>(0.6 * fs);
    std::vector<float> out(len, 0.0f);
    Biquad crackHp = Biquad::highpass(fs, 900.0);
    Biquad boomLp = Biquad::lowpass(fs, 140.0, 1.4);
    for (std::size_t n = 0; n < len; ++n) {
        const double t = static_cast<double>(n) / fs;
        const float x = rng.next();
        const float crack = crackHp.process(x) * static_cast<float>(std::exp(-t / 0.012));
        const float tail = x * static_cast<float>(0.25 * std::exp(-t / 0.18));
        const float boom = boomLp.process(x) * static_cast<float>(3.0 * std::exp(-t / 0.09));
        out[n] = crack * 1.6f + boom + tail;
    }
    return out;
}

void place(Scene& scene, const Vbap2D& vbap, const std::vector<float>& sound, double timeSec, float azDeg,
           float gain, const char* label) {
    auto& ch = scene.audio.channels;
    std::vector<float> g = vbap.gains(azDeg);
    const auto start = static_cast<std::size_t>(timeSec * scene.audio.sampleRate);
    for (std::size_t c = 0; c < ch.size(); ++c) {
        if (g[c] == 0.0f) continue;
        for (std::size_t n = 0; n < sound.size() && start + n < ch[c].size(); ++n)
            ch[c][start + n] += g[c] * gain * sound[n];
    }
    scene.events.push_back({timeSec, azDeg, label});
}

void normalizePeak(AudioBuffer& a, float targetPeak) {
    float peak = 0;
    for (const auto& ch : a.channels)
        for (float v : ch) peak = std::max(peak, std::abs(v));
    if (peak <= 0) return;
    const float k = targetPeak / peak;
    for (auto& ch : a.channels)
        for (float& v : ch) v *= k;
}

}  // namespace

const char* sceneName(SceneKind kind) noexcept {
    switch (kind) {
        case SceneKind::Compass: return "compass";
        case SceneKind::Circle: return "circle";
        case SceneKind::Battle: return "battle";
    }
    return "?";
}

Scene makeScene(SceneKind kind, const SpeakerLayout& layout, std::uint32_t sampleRate) {
    const double fs = sampleRate;
    const Vbap2D vbap(layout);
    Rng rng;
    Scene scene;
    scene.audio.sampleRate = sampleRate;
    scene.audio.channelMask = layout.channelMask;

    double duration = 0;
    switch (kind) {
        case SceneKind::Compass: duration = 16.5; break;
        case SceneKind::Circle: duration = 12.5; break;
        case SceneKind::Battle: duration = 12.5; break;
    }
    scene.audio.resize(layout.size(), static_cast<std::size_t>(duration * fs));

    constexpr double kStepInterval = 0.5;  // walking pace: 2 steps per second
    if (kind == SceneKind::Compass) {
        const float dirs[8] = {0, 45, 90, 135, 180, -135, -90, -45};
        const char* names[8] = {"front", "front-right", "right", "back-right",
                                "back", "back-left", "left", "front-left"};
        for (int d = 0; d < 8; ++d)
            for (int s = 0; s < 4; ++s)
                place(scene, vbap, synthFootstep(fs, rng, static_cast<float>(s % 2)), 0.25 + d * 2.0 + s * kStepInterval,
                      dirs[d], 1.0f, names[d]);
    } else {
        // Full clockwise circle in 12 s, starting in front.
        const int steps = 24;
        for (int s = 0; s < steps; ++s) {
            const float az = static_cast<float>(std::fmod(s * 360.0 / steps + 180.0, 360.0) - 180.0);
            place(scene, vbap, synthFootstep(fs, rng, static_cast<float>(s % 2)), 0.25 + s * kStepInterval, az, 1.0f,
                  "footstep");
        }
        if (kind == SceneKind::Battle) {
            for (int burst = 0; burst < 5; ++burst)
                for (int shot = 0; shot < 3; ++shot)
                    place(scene, vbap, synthGunshot(fs, rng), 1.1 + burst * 2.3 + shot * 0.11, 40.0f, 6.0f, "gunshot");
        }
    }
    normalizePeak(scene.audio, 0.5f);  // -6 dBFS
    return scene;
}

}  // namespace tac

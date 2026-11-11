// TacAudio unit tests. Each DSP block is checked against a slow, obviously-correct reference.

#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <numbers>
#include <string>
#include <vector>

#include "core/binaural_renderer.h"
#include "core/biquad.h"
#include "core/dynamics.h"
#include "core/footstep_detector.h"
#include "core/footstep_synth.h"
#include "core/headphone_eq.h"
#include "core/convolver.h"
#include "core/fft.h"
#include "core/hrtf.h"
#include "core/layout.h"
#include "core/panner.h"
#include "core/scene.h"
#include "core/limiter.h"
#include "core/ring_buffer.h"
#include "core/varispeed.h"
#include "core/wav.h"
#include "engine/engine.h"

#ifdef TAC_HAS_SOFA
#include "sofa/sofa_import.h"
#endif

using namespace tac;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        ++g_checks;                                                                                    \
        const double va_ = (a), vb_ = (b);                                                             \
        if (!(std::abs(va_ - vb_) <= (tol))) {                                                         \
            ++g_failures;                                                                              \
            std::printf("    FAIL %s:%d  %s = %g, expected %g (tol %g)\n", __FILE__, __LINE__, #a, va_, vb_, \
                        static_cast<double>(tol));                                                     \
        }                                                                                              \
    } while (0)

struct Rng {
    std::uint32_t s = 0xC0FFEEu;
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s) / 2147483648.0f - 1.0f;
    }
    std::vector<float> vec(std::size_t n) {
        std::vector<float> v(n);
        for (auto& x : v) x = next();
        return v;
    }
};

double energy(const std::vector<float>& v) {
    double e = 0;
    for (float x : v) e += static_cast<double>(x) * x;
    return e;
}

std::size_t firstAbove(const std::vector<float>& v, float threshold) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::abs(v[i]) > threshold) return i;
    return v.size();
}

// ---------------------------------------------------------------- FFT
void testFftMatchesDft() {
    Rng rng;
    for (std::size_t n : {2u, 8u, 64u, 512u}) {
        Fft fft(n);
        std::vector<cpx> x(n);
        for (auto& v : x) v = cpx(rng.next(), rng.next());
        std::vector<cpx> y = x;
        fft.forward(y.data());
        double maxErr = 0;
        for (std::size_t k = 0; k < n; ++k) {
            std::complex<double> ref = 0;
            for (std::size_t t = 0; t < n; ++t)
                ref += std::complex<double>(x[t]) *
                       std::polar(1.0, -2.0 * std::numbers::pi * static_cast<double>(k * t) / static_cast<double>(n));
            maxErr = std::max(maxErr, std::abs(std::complex<double>(y[k]) - ref));
        }
        CHECK(maxErr < 1e-3 * std::sqrt(static_cast<double>(n)));
        fft.inverse(y.data());
        double rtErr = 0;
        for (std::size_t i = 0; i < n; ++i) rtErr = std::max(rtErr, static_cast<double>(std::abs(y[i] - x[i])));
        CHECK(rtErr < 1e-5);
    }
}

void testFftRealRoundTrip() {
    Rng rng;
    const std::size_t n = 256;
    Fft fft(n);
    std::vector<float> x = rng.vec(n), back(n);
    std::vector<cpx> half(fft.halfBins()), scratch(n);
    fft.forwardReal(x.data(), half.data(), scratch.data());
    fft.inverseReal(half.data(), back.data(), scratch.data());
    double err = 0;
    for (std::size_t i = 0; i < n; ++i) err = std::max(err, static_cast<double>(std::abs(back[i] - x[i])));
    CHECK(err < 1e-5);
}

// ---------------------------------------------------------------- convolver
std::vector<float> directConvolve(const std::vector<float>& x, const std::vector<float>& h, std::size_t outLen) {
    std::vector<float> y(outLen, 0.0f);
    for (std::size_t n = 0; n < outLen; ++n) {
        double acc = 0;
        for (std::size_t k = 0; k < h.size() && k <= n; ++k)
            if (n - k < x.size()) acc += static_cast<double>(h[k]) * x[n - k];
        y[n] = static_cast<float>(acc);
    }
    return y;
}

void testConvolverMatchesDirect() {
    Rng rng;
    struct Case {
        std::size_t block, irLen;
    };
    for (const Case c : {Case{64, 1}, Case{64, 40}, Case{64, 64}, Case{128, 700}, Case{256, 256}, Case{32, 1000}}) {
        const std::vector<float> irA = rng.vec(c.irLen);
        const std::vector<float> irB = rng.vec(c.irLen / 2 + 1);
        const std::size_t blocks = 12;
        const std::vector<float> x = rng.vec(c.block * blocks);

        PartitionedConvolver conv(c.block, {irA, irB});
        std::vector<float> yA(x.size()), yB(x.size());
        for (std::size_t b = 0; b < blocks; ++b) {
            float* outs[2] = {yA.data() + b * c.block, yB.data() + b * c.block};
            conv.process(x.data() + b * c.block, outs);
        }
        const auto refA = directConvolve(x, irA, x.size());
        const auto refB = directConvolve(x, irB, x.size());
        double errA = 0, errB = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            errA = std::max(errA, static_cast<double>(std::abs(yA[i] - refA[i])));
            errB = std::max(errB, static_cast<double>(std::abs(yB[i] - refB[i])));
        }
        const double tol = 2e-4 * std::sqrt(static_cast<double>(c.irLen));
        CHECK(errA < tol);
        CHECK(errB < tol);
    }
}

void testConvolverAccumulateAndReset() {
    Rng rng;
    const std::size_t B = 64;
    const std::vector<float> ir = rng.vec(100);
    const std::vector<float> x = rng.vec(B);
    PartitionedConvolver conv(B, {ir});
    std::vector<float> first(B), acc(B, 1.0f);
    float* o1[1] = {first.data()};
    conv.process(x.data(), o1);
    conv.reset();
    float* o2[1] = {acc.data()};
    conv.process(x.data(), o2, true);
    double err = 0;
    for (std::size_t i = 0; i < B; ++i) err = std::max(err, static_cast<double>(std::abs(acc[i] - (first[i] + 1.0f))));
    CHECK(err < 1e-5);
}

// ---------------------------------------------------------------- WAV
void testWavRoundTrip() {
    Rng rng;
    AudioBuffer a;
    a.sampleRate = 48000;
    a.channelMask = 0x63F;
    a.resize(8, 1000);
    for (auto& ch : a.channels)
        for (auto& v : ch) v = rng.next() * 0.9f;

    const auto dir = std::filesystem::temp_directory_path() / "tacaudio_tests";
    std::filesystem::create_directories(dir);
    for (auto fmt : {WavSampleFormat::Float32, WavSampleFormat::Pcm24, WavSampleFormat::Pcm16}) {
        const auto path = dir / "rt.wav";
        writeWav(path, a, fmt);
        const AudioBuffer b = readWav(path);
        CHECK(b.sampleRate == 48000);
        CHECK(b.channelMask == 0x63F);
        CHECK(b.numChannels() == 8);
        CHECK(b.numFrames() == 1000);
        const double tol = fmt == WavSampleFormat::Float32 ? 0.0 : fmt == WavSampleFormat::Pcm24 ? 2e-7 : 5e-5;
        double err = 0;
        for (std::size_t c = 0; c < 8; ++c)
            for (std::size_t i = 0; i < 1000; ++i)
                err = std::max(err, static_cast<double>(std::abs(a.channels[c][i] - b.channels[c][i])));
        CHECK(err <= tol);
    }
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------- layout & panner
void testLayout71() {
    const SpeakerLayout l = layout71();
    CHECK(l.size() == 8);
    CHECK(l.speakers[0].name == "FL" && l.speakers[0].azimuthDeg == -30.0f);
    CHECK(l.speakers[1].name == "FR" && l.speakers[1].azimuthDeg == 30.0f);
    CHECK(l.speakers[3].isLfe);
    CHECK(l.speakers[4].name == "BL" && l.speakers[4].azimuthDeg == -140.0f);
    CHECK(l.speakers[6].name == "SL" && l.speakers[6].azimuthDeg == -90.0f);
    // 5.1 "back" variant: BL/BR without sides moves to 110 deg
    const SpeakerLayout l51 = layoutFromChannelMask(0x3F);
    CHECK(l51.speakers[4].azimuthDeg == -110.0f);
}

void testVbap() {
    const SpeakerLayout l = layout71();
    const Vbap2D v(l);
    // exactly on a speaker -> only that speaker
    auto g = v.gains(90.0f);  // SR = channel 7
    CHECK_NEAR(g[7], 1.0, 1e-5);
    for (std::size_t c = 0; c < 8; ++c)
        if (c != 7) CHECK_NEAR(g[c], 0.0, 1e-5);
    // power is constant everywhere, LFE never used
    for (float az = -180.0f; az < 180.0f; az += 7.0f) {
        g = v.gains(az);
        double p = 0;
        for (float x : g) p += static_cast<double>(x) * x;
        CHECK_NEAR(p, 1.0, 1e-4);
        CHECK(g[3] == 0.0f);
    }
    // halfway between FC (0) and FR (30): equal gains
    g = v.gains(15.0f);
    CHECK_NEAR(g[2], g[1], 1e-4);
    CHECK(g[2] > 0.6f);
}

// ---------------------------------------------------------------- HRTF
void testSphericalHeadCues() {
    const HrirSet h = HrirSet::makeSphericalHead(48000);
    CHECK(!h.measurements.empty());

    const auto& right = h.nearest(90, 0);
    CHECK_NEAR(right.azimuthDeg, 90.0, 1e-3);
    // source on the right: right ear first (ITD) and louder (ILD)
    CHECK(firstAbove(right.right, 0.05f) < firstAbove(right.left, 0.05f));
    CHECK(energy(right.right) > 2.0 * energy(right.left));

    const auto& left = h.nearest(-90, 0);
    CHECK(firstAbove(left.left, 0.05f) < firstAbove(left.right, 0.05f));

    // front: symmetric
    const auto& front = h.nearest(0, 0);
    CHECK_NEAR(energy(front.left), energy(front.right), 1e-3 * energy(front.left));

    // front vs back at the same lateral angle: back has less high-frequency energy
    const auto& f30 = h.nearest(30, 0);
    const auto& b150 = h.nearest(150, 0);
    auto hfEnergy = [](const std::vector<float>& ir) {
        double e = 0;
        for (std::size_t i = 1; i < ir.size(); ++i) {
            const double d = ir[i] - ir[i - 1];  // crude high-pass
            e += d * d;
        }
        return e;
    };
    CHECK(hfEnergy(b150.right) < hfEnergy(f30.right));

    // max ITD of a ~17.5 cm head is ~0.66 ms (~32 samples at 48 kHz)
    const auto itd = static_cast<double>(firstAbove(right.left, 0.02f)) - static_cast<double>(firstAbove(right.right, 0.02f));
    CHECK(itd > 20 && itd < 40);
}

void testThrirRoundTrip() {
    SphericalHeadParams p;
    p.azimuthStepDeg = 30;
    p.elevationStepDeg = 60;
    const HrirSet a = HrirSet::makeSphericalHead(48000, p);
    const auto path = std::filesystem::temp_directory_path() / "tacaudio_test.thrir";
    a.save(path);
    const HrirSet b = HrirSet::load(path);
    std::filesystem::remove(path);
    CHECK(b.measurements.size() == a.measurements.size());
    CHECK(b.irLength == a.irLength);
    CHECK(b.sampleRate == 48000);
    CHECK(b.measurements[3].left == a.measurements[3].left);
    CHECK(b.measurements[3].azimuthDeg == a.measurements[3].azimuthDeg);
}

// ---------------------------------------------------------------- renderer
void testRendererDirection() {
    const SpeakerLayout l = layout71();
    const HrirSet h = HrirSet::makeSphericalHead(48000);
    AudioBuffer in;
    in.sampleRate = 48000;
    in.channelMask = l.channelMask;

    auto renderImpulseOn = [&](std::size_t ch) {
        in.resize(8, 2048);
        in.channels[ch][100] = 1.0f;
        return renderBinaural(in, l, h);
    };

    const AudioBuffer sl = renderImpulseOn(6);  // SL
    CHECK(energy(sl.channels[0]) > 2.0 * energy(sl.channels[1]));
    CHECK(firstAbove(sl.channels[0], 0.05f) < firstAbove(sl.channels[1], 0.05f));

    const AudioBuffer sr = renderImpulseOn(7);  // SR
    CHECK(energy(sr.channels[1]) > 2.0 * energy(sr.channels[0]));

    const AudioBuffer fc = renderImpulseOn(2);  // FC: centred
    CHECK_NEAR(energy(fc.channels[0]), energy(fc.channels[1]), 1e-3 * energy(fc.channels[0]));

    // LFE is mono into both ears, low-passed (sine at 1 kHz is strongly attenuated)
    in.resize(8, 48000);
    for (std::size_t i = 0; i < 48000; ++i)
        in.channels[3][i] = static_cast<float>(std::sin(2 * std::numbers::pi * 1000.0 * i / 48000.0));
    const AudioBuffer lfe = renderBinaural(in, l, h);
    CHECK_NEAR(energy(lfe.channels[0]), energy(lfe.channels[1]), 1e-6);
    CHECK(energy(lfe.channels[0]) < 0.01 * energy(in.channels[3]));

    // output length includes the HRIR tail
    CHECK(lfe.numFrames() == 48000 + h.irLength);
}

void testRendererRejectsWrongRate() {
    bool threw = false;
    try {
        BinauralRenderer r(layout71(), HrirSet::makeSphericalHead(44100), 48000);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void testScenes() {
    const SpeakerLayout l = layout71();
    const Scene s = makeScene(SceneKind::Compass, l);
    CHECK(s.audio.numChannels() == 8);
    CHECK(s.events.size() == 32);
    float peak = 0;
    for (const auto& ch : s.audio.channels)
        for (float v : ch) peak = std::max(peak, std::abs(v));
    CHECK_NEAR(peak, 0.5, 1e-4);
    // First compass direction is front: only FC (ch2) has signal in the first second
    CHECK(energy(std::vector<float>(s.audio.channels[2].begin(), s.audio.channels[2].begin() + 48000)) > 0);
    CHECK(energy(std::vector<float>(s.audio.channels[6].begin(), s.audio.channels[6].begin() + 48000)) == 0);
}

// ---------------------------------------------------------------- M2: real-time building blocks
void testRingBuffer() {
    SpscRingBuffer rb(2, 100);  // rounds up to 128 frames
    CHECK(rb.capacityFrames() == 128);

    // Wrap around many times with odd chunk sizes: frame k carries (k, -k); must come out in order.
    float next = 0, expect = 0;
    bool ordered = true;
    std::vector<float> chunk(2 * 37), out(2 * 29);
    for (int round = 0; round < 50; ++round) {
        for (std::size_t i = 0; i < 37; ++i) {
            chunk[2 * i] = next + static_cast<float>(i);
            chunk[2 * i + 1] = -(next + static_cast<float>(i));
        }
        next += static_cast<float>(rb.write(chunk.data(), 37));
        const std::size_t got = rb.read(out.data(), 29);
        for (std::size_t i = 0; i < got; ++i, expect += 1) ordered &= out[2 * i] == expect && out[2 * i + 1] == -expect;
    }
    CHECK(ordered);
    CHECK(expect == 50 * 29);

    SpscRingBuffer r2(1, 8);
    const float a[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(r2.write(a, 10) == 8);  // full: only capacity is accepted
    CHECK(r2.availableRead() == 8);
    float b[8];
    CHECK(r2.read(b, 5) == 5 && b[0] == 1 && b[4] == 5);
    CHECK(r2.write(a, 4) == 4);  // wraps
    CHECK(r2.read(b, 7) == 7);
    CHECK(b[0] == 6 && b[2] == 8 && b[3] == 1 && b[6] == 4);
    CHECK(r2.discard(100) == 0);
    CHECK(r2.writeSilence(3) == 3 && r2.read(b, 3) == 3 && b[0] == 0 && b[2] == 0);
}

void testVarispeed() {
    // ratio 1: exact pass-through across block boundaries
    {
        SpscRingBuffer rb(2, 4096);
        std::vector<float> in(2 * 1000);
        Rng rng;
        for (auto& v : in) v = rng.next();
        rb.write(in.data(), 1000);
        VarispeedReader vr(2, 64);
        std::vector<float> l(64), r(64);
        float* outs[2] = {l.data(), r.data()};
        double err = 0;
        for (int b = 0; b < 10; ++b) {
            CHECK(vr.read(rb, outs, 64, 1.0));
            for (std::size_t i = 0; i < 64; ++i) {
                err = std::max(err, static_cast<double>(std::abs(l[i] - in[2 * (b * 64 + i)])));
                err = std::max(err, static_cast<double>(std::abs(r[i] - in[2 * (b * 64 + i) + 1])));
            }
        }
        CHECK(err == 0.0);
    }
    // ratio != 1: a 1 kHz sine read 0.3 % fast equals the analytically resampled sine
    {
        const double fs = 48000, f = 1000, ratio = 1.003;
        SpscRingBuffer rb(1, 16384);
        std::vector<float> in(10000);
        for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(std::sin(2 * std::numbers::pi * f * i / fs));
        rb.write(in.data(), in.size());
        VarispeedReader vr(1, 256);
        std::vector<float> out(256);
        float* o[1] = {out.data()};
        double err = 0;
        std::size_t n = 0;
        for (int b = 0; b < 30; ++b) {
            CHECK(vr.read(rb, o, 256, ratio));
            for (std::size_t i = 0; i < 256; ++i, ++n) {
                const double t = static_cast<double>(n) * ratio;  // input position of this output sample
                err = std::max(err, std::abs(out[i] - std::sin(2 * std::numbers::pi * f * t / fs)));
            }
        }
        CHECK(err < 2e-3);
        CHECK(n * ratio < 10000);
    }
    // not enough input -> false, and nothing is lost
    {
        SpscRingBuffer rb(1, 64);
        const float x[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        rb.write(x, 10);
        VarispeedReader vr(1, 32);
        std::vector<float> out(32);
        float* o[1] = {out.data()};
        CHECK(!vr.read(rb, o, 32, 1.0));
        std::vector<float> more(40, 0.5f);
        rb.write(more.data(), 40);
        CHECK(vr.read(rb, o, 32, 1.0));
        CHECK(out[0] == 1.0f && out[9] == 10.0f && out[10] == 0.5f);
    }
}

void testLimiter() {
    const double fs = 48000;
    Limiter lim(fs);  // -1 dBFS ceiling, 48-sample look-ahead
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    Rng rng;
    const std::size_t n = 96000;
    std::vector<float> L(n), R(n), inL(n), inR(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float burst = (i > 10000 && i < 10200) ? 4.0f : (i > 30000 && i < 30003) ? 8.0f : 0.1f;
        inL[i] = L[i] = rng.next() * burst;
        inR[i] = R[i] = rng.next() * burst * 0.5f;
    }
    lim.process(L.data(), R.data(), n);
    float peak = 0;
    for (std::size_t i = 0; i < n; ++i) peak = std::max({peak, std::abs(L[i]), std::abs(R[i])});
    CHECK(peak <= ceiling + 1e-6f);
    CHECK(lim.takeMinGain() < 0.2f);

    // Quiet passages are untouched (only delayed by the look-ahead) once released.
    const std::size_t d = lim.latency();
    CHECK(d == 48);
    double err = 0;
    for (std::size_t i = 1000; i < 9000; ++i) err = std::max(err, static_cast<double>(std::abs(L[i] - inL[i - d])));
    CHECK(err < 1e-6);
    // ...and recover after a burst: exponential release (tau 80 ms) is back to unity
    // within ~1e-4 after ~9 tau; check from 1 s after the last burst.
    err = 0;
    for (std::size_t i = 80000; i < 95000; ++i) err = std::max(err, static_cast<double>(std::abs(L[i] - inL[i - d])));
    CHECK(err < 1e-4);
    // Shortly after a burst it is still releasing (gain < 1), i.e. no instant pumping.
    double ratioAt = std::abs(L[31000 + 200] / inL[31000 + 200 - d]);
    CHECK(ratioAt < 0.999);
}

// Real-time path == offline path when drift compensation and limiter are off.
void testEngineMatchesOffline() {
    const SpeakerLayout l = layout71();
    const HrirSet h = HrirSet::makeSphericalHead(48000);
    const Scene scene = makeScene(SceneKind::Circle, l);

    EngineConfig cfg;
    cfg.driftCompensation = false;
    cfg.limiter.enabled = false;
    cfg.targetLatencyMs = 10;
    Engine eng(l, h, cfg);

    const std::size_t total = 48000 * 2;  // 2 s
    const std::size_t C = 8;
    std::vector<float> chunk;
    std::vector<float> out(total * 2), tmp(441 * 2);
    std::size_t pushed = 0, pulled = 0;
    while (pulled + 441 <= total) {
        // Producer stays ~1000 frames ahead: enough to never underrun, below the resync threshold.
        while (pushed < pulled + 1000 && pushed < scene.audio.numFrames()) {
            const std::size_t n = std::min<std::size_t>(480, scene.audio.numFrames() - pushed);
            chunk.resize(n * C);
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t c = 0; c < C; ++c) chunk[i * C + c] = scene.audio.channels[c][pushed + i];
            eng.pushInput(chunk.data(), n);
            pushed += n;
        }
        eng.pullOutput(tmp.data(), 441);  // deliberately not a multiple of the block size
        std::copy(tmp.begin(), tmp.end(), out.begin() + static_cast<std::ptrdiff_t>(2 * pulled));
        pulled += 441;
    }

    const AudioBuffer ref = renderBinaural(scene.audio, l, h, cfg.binaural);
    // Engine output = offline output delayed by its start-up wait (a whole number of blocks;
    // zero here because input is already waiting when the first block is rendered).
    std::size_t bestOffset = 0;
    double bestErr = 1e30;
    for (std::size_t off = 0; off <= 4096; off += cfg.binaural.blockSize) {
        double e = 0;
        for (std::size_t i = off; i < pulled; ++i) {
            e = std::max(e, static_cast<double>(std::abs(out[2 * i] - ref.channels[0][i - off])));
            e = std::max(e, static_cast<double>(std::abs(out[2 * i + 1] - ref.channels[1][i - off])));
            if (e > bestErr) break;
        }
        if (e < bestErr) {
            bestErr = e;
            bestOffset = off;
        }
    }
    CHECK(bestErr < 1e-6);
    CHECK(bestOffset == 0);
    const EngineStats s = eng.takeStats();
    CHECK(s.underruns == 0 && s.overruns == 0 && s.resyncs == 0);
}

// Two devices whose clocks differ by `ppm`: the engine must settle without glitches.
void runDriftScenario(double ppm) {
    const SpeakerLayout l = layout71();
    SphericalHeadParams hp;
    hp.irLength = 64;  // short HRIRs keep this long simulation fast
    const HrirSet h = HrirSet::makeSphericalHead(48000, hp);
    EngineConfig cfg;
    Engine eng(l, h, cfg);

    std::vector<float> in(600 * 8, 0.01f), out(480 * 2);
    double producerAcc = 0;
    EngineStats afterSettle{};
    const int ticks = 100 * 90;  // 90 s of 10 ms device periods
    for (int t = 0; t < ticks; ++t) {
        producerAcc += 480.0 * (1.0 + ppm * 1e-6);
        const auto n = static_cast<std::size_t>(producerAcc);
        producerAcc -= static_cast<double>(n);
        eng.pushInput(in.data(), n);
        eng.pullOutput(out.data(), 480);
        if (t == 100 * 30) afterSettle = eng.takeStats();  // 30 s: controller settled
    }
    const EngineStats end = eng.takeStats();
    CHECK(end.underruns == afterSettle.underruns);  // no glitches after settling
    CHECK(end.overruns == 0);
    CHECK(end.resyncs == 0);
    CHECK(std::abs(end.driftPpm - ppm) < 60.0);   // ratio tracks the clock difference
    CHECK(std::abs(end.bufferMs - 20.0) < 8.0);   // buffer stays near the 20 ms target
    CHECK(end.running);
}

void testEngineDriftFast() { runDriftScenario(+400.0); }
void testEngineDriftSlow() { runDriftScenario(-400.0); }

void testEngineUnderrunRecovers() {
    const SpeakerLayout l = layout71();
    SphericalHeadParams hp;
    hp.irLength = 64;
    Engine eng(l, HrirSet::makeSphericalHead(48000, hp), EngineConfig{});
    std::vector<float> in(480 * 8, 0.01f), out(480 * 2);
    for (int t = 0; t < 200; ++t) {
        if (t < 50 || t > 80) eng.pushInput(in.data(), 480);  // input stalls for 300 ms
        eng.pullOutput(out.data(), 480);
    }
    const EngineStats s = eng.takeStats();
    CHECK(s.underruns >= 1);
    CHECK(s.running);  // resumed after input came back
    // During the stall the output decays to silence (no stuck buffer / noise).
    double tail = 0;
    for (float v : out) tail = std::max(tail, static_cast<double>(std::abs(v)));
    CHECK(tail < 0.5);
}

// Diffuse-field EQ: the average becomes flat; per-frequency interaural level differences stay put.
void checkDiffuseFieldEq(HrirSet h, double tolDb) {
    const std::size_t N = 4096;
    auto bandDb = [&](const std::vector<double>& P, double lo, double hi) {
        double s = 0;
        int n = 0;
        for (std::size_t k = 0; k < P.size(); ++k) {
            const double f = k * h.sampleRate / N;
            if (f >= lo && f < hi) {
                s += P[k];
                ++n;
            }
        }
        return 10 * std::log10(s / n);
    };
    auto ildDb = [&](const HrirMeasurement& m, double f) {
        Fft fft(N);
        std::vector<float> b(N);
        std::vector<cpx> L(N / 2 + 1), R(N / 2 + 1), sc(N);
        std::fill(b.begin(), b.end(), 0.0f);
        std::copy(m.left.begin(), m.left.end(), b.begin());
        fft.forwardReal(b.data(), L.data(), sc.data());
        std::fill(b.begin(), b.end(), 0.0f);
        std::copy(m.right.begin(), m.right.end(), b.begin());
        fft.forwardReal(b.data(), R.data(), sc.data());
        const auto k = static_cast<std::size_t>(f * N / h.sampleRate);
        return 10 * std::log10(std::norm(R[k]) / std::norm(L[k]));
    };
    const double ild1kBefore = ildDb(h.nearest(60, 0), 1000), ild4kBefore = ildDb(h.nearest(60, 0), 4000);
    const std::size_t lenBefore = h.irLength;
    const auto rep = h.diffuseFieldEqualize();
    CHECK(h.irLength > lenBefore);
    CHECK(rep.maxBoostDb <= 10.0 + 1e-9);

    const auto P = h.diffuseFieldPower(N);
    const double bands[][2] = {{300, 600}, {600, 1200}, {1200, 2400}, {2400, 4800}, {4800, 9600}, {9600, 14000}};
    for (const auto& b : bands) CHECK_NEAR(bandDb(P, b[0], b[1]), 0.0, tolDb);
    CHECK_NEAR(ildDb(h.nearest(60, 0), 1000), ild1kBefore, 0.2);
    CHECK_NEAR(ildDb(h.nearest(60, 0), 4000), ild4kBefore, 0.2);
}

void testDiffuseFieldEqSpherical() { checkDiffuseFieldEq(HrirSet::makeSphericalHead(48000), 1.5); }

// Producer that stalls 20 ms every second (game load): the adaptive buffer must grow until it stops glitching.
void testEngineAdaptiveJitter() {
    const SpeakerLayout l = layout71();
    SphericalHeadParams hp;
    hp.irLength = 64;
    EngineConfig cfg;
    cfg.targetLatencyMs = 8.0;
    Engine eng(l, HrirSet::makeSphericalHead(48000, hp), cfg);
    std::vector<float> in(480 * 4 * 8, 0.01f), out(144 * 2);
    // 1 ms ticks: consumer pulls 48 frames/ms (exclusive-style small periods), producer 480 every 10 ms.
    std::uint64_t stalled = 0;
    EngineStats mid{};
    for (int ms = 0; ms < 120000; ++ms) {
        // Once per second one packet is held back and delivered together with the next (20 ms gap).
        const bool stall = (ms % 1000) >= 990;
        if (ms % 10 == 0) {
            if (stall) stalled += 480;
            else {
                eng.pushInput(in.data(), 480 + static_cast<std::size_t>(stalled));
                stalled = 0;
            }
        }
        if (ms % 3 == 0) eng.pullOutput(out.data(), 144);
        if (ms == 90000) mid = eng.takeStats();
    }
    const EngineStats end = eng.takeStats();
    CHECK(mid.underruns > 0);                      // it did glitch at first...
    CHECK(end.underruns == mid.underruns);         // ...but not in the last 30 s
    CHECK(end.targetMs > 8.0 && end.targetMs < 60.0);
}

void testRoom() {
    const double fs = 48000;
    const SpeakerLayout l = layout71();
    const HrirSet h = HrirSet::makeSphericalHead(fs);
    AudioBuffer in;
    in.sampleRate = 48000;
    in.channelMask = l.channelMask;
    in.resize(8, 48000);
    in.channels[1][100] = 1.0f;  // impulse on FR

    BinauralConfig dry, bal;
    bal.room = roomPreset("balanced");
    const AudioBuffer a = renderBinaural(in, l, h, dry);
    const AudioBuffer b = renderBinaural(in, l, h, bal);
    // neutral preset is exactly the dry renderer
    BinauralConfig neu;
    neu.room = roomPreset("neutral");
    const AudioBuffer n = renderBinaural(in, l, h, neu);
    CHECK(n.channels[0] == a.channels[0] && n.channels[1] == a.channels[1]);

    // Direct sound arrives at the same time (room only adds later energy).
    CHECK(firstAbove(b.channels[1], 0.05f) == firstAbove(a.channels[1], 0.05f));
    // Nothing in the first 5 ms after the direct sound comes from the room.
    const std::size_t onset = firstAbove(a.channels[1], 0.05f);
    double diffEarly = 0;
    for (std::size_t i = 0; i < onset + 240; ++i) diffEarly = std::max(diffEarly, static_cast<double>(std::abs(b.channels[1][i] - a.channels[1][i])));
    CHECK(diffEarly < 1e-6);

    // Wet-only response: decays, stays well below the direct sound, L/R decorrelated.
    std::vector<float> wl(b.numFrames()), wr(b.numFrames());
    for (std::size_t i = 0; i < wl.size(); ++i) {
        wl[i] = b.channels[0][i] - (i < a.numFrames() ? a.channels[0][i] : 0.0f);
        wr[i] = b.channels[1][i] - (i < a.numFrames() ? a.channels[1][i] : 0.0f);
    }
    auto winE = [&](double t0, double t1) {
        double e = 0;
        for (std::size_t i = static_cast<std::size_t>(t0 * fs); i < static_cast<std::size_t>(t1 * fs) && i < wl.size(); ++i)
            e += static_cast<double>(wl[i]) * wl[i] + static_cast<double>(wr[i]) * wr[i];
        return e;
    };
    const double e1 = winE(0.06, 0.10), e2 = winE(0.16, 0.20);
    // RT60 0.28 s -> 60 dB per 0.28 s -> ~21 dB per 0.1 s (allow a wide band: damping, early part)
    const double dropDb = 10 * std::log10(e1 / std::max(e2, 1e-30));
    CHECK(dropDb > 12.0 && dropDb < 35.0);
    CHECK(winE(0.0, 1.0) < 0.3 * (energy(a.channels[0]) + energy(a.channels[1])));
    double sxy = 0, sxx = 0, syy = 0;
    for (std::size_t i = static_cast<std::size_t>(0.05 * fs); i < static_cast<std::size_t>(0.3 * fs); ++i) {
        sxy += static_cast<double>(wl[i]) * wr[i];
        sxx += static_cast<double>(wl[i]) * wl[i];
        syy += static_cast<double>(wr[i]) * wr[i];
    }
    CHECK(std::abs(sxy / std::sqrt(sxx * syy)) < 0.6);

    // Stable on long loud noise (spacious, the longest decay).
    RoomReverb r(fs, roomPreset("spacious"));
    Rng rng;
    std::vector<float> x(4800), y(4800), z(4800);
    double peak = 0;
    for (int blk = 0; blk < 200; ++blk) {
        for (auto& v : x) v = rng.next();
        std::fill(y.begin(), y.end(), 0.0f);
        std::fill(z.begin(), z.end(), 0.0f);
        r.process(x.data(), x.data(), y.data(), z.data(), x.size());
        for (std::size_t i = 0; i < y.size(); ++i) peak = std::max({peak, std::abs(static_cast<double>(y[i])), std::abs(static_cast<double>(z[i]))});
    }
    CHECK(std::isfinite(peak) && peak < 2.0);

    bool threw = false;
    try {
        roomPreset("cathedral");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

// Steady-state output level (dBFS RMS) of a 1 kHz sine at `inDb` through the given preset.
double dynamicsOutDb(const DynamicsParams& p, double inDb, std::size_t channels = 1, std::size_t ch = 0) {
    const double fs = 48000;
    ChannelDynamics d(fs, channels, p);
    const std::size_t n = 48000;
    std::vector<std::vector<float>> buf(channels, std::vector<float>(n, 0.0f));
    const double amp = std::pow(10.0, inDb / 20.0) * std::sqrt(2.0);  // sine with this RMS
    for (std::size_t i = 0; i < n; ++i) buf[ch][i] = static_cast<float>(amp * std::sin(2 * std::numbers::pi * 1000.0 * i / fs));
    std::vector<float*> ptrs;
    for (auto& b : buf) ptrs.push_back(b.data());
    for (std::size_t off = 0; off < n; off += 128) {
        std::vector<float*> blk;
        for (auto* p2 : ptrs) blk.push_back(p2 + off);
        d.process(blk.data(), 128);
    }
    double e = 0;
    for (std::size_t i = n / 2; i < n; ++i) e += static_cast<double>(buf[ch][i]) * buf[ch][i];
    return 10 * std::log10(e / (n / 2) + 1e-30);
}

void testDynamics() {
    const DynamicsParams med = dynamicsPreset("medium");
    const ChannelDynamics probe(48000, 1, med);
    // Static curve: loud reduced (above threshold by 18 dB at 4:1 -> -13.5 dB), mid untouched,
    // quiet lifted (9 dB below boostBelow at 2:1 -> +4.5 dB), silence not lifted.
    CHECK_NEAR(probe.curveDb(-6.0f), -13.5, 0.01);
    CHECK_NEAR(probe.curveDb(-31.0f), 0.0, 0.01);
    CHECK_NEAR(probe.curveDb(-47.0f), 4.5, 0.01);
    CHECK_NEAR(probe.curveDb(-80.0f), 0.0, 0.01);
    CHECK(probe.curveDb(-30.0f) <= 0.0f && probe.curveDb(-30.0f) > -0.5f);  // soft knee edge

    // Real signals (the detector follows the sine peak, ~3 dB above RMS).
    const double loud = dynamicsOutDb(med, -6.0);   // gunfire level
    const double quiet = dynamicsOutDb(med, -48.0);  // distant footsteps
    CHECK(loud < -6.0 - 8.0);                         // tamed by well over 8 dB
    CHECK(quiet > -48.0 + 2.0);                       // lifted by a few dB (plus makeup)
    // The gap between gunfire and footsteps shrinks a lot (42 dB in).
    CHECK((loud - quiet) < 30.0);

    // Off is bit-exact pass-through (renderer skips it entirely, but check the class too).
    CHECK_NEAR(dynamicsOutDb(dynamicsPreset("off"), -6.0), -6.0, 1e-3);

    // Channels are independent: a gunshot level on ch0 leaves a quiet ch1 alone.
    {
        const double fs = 48000;
        ChannelDynamics d(fs, 2, med);
        std::vector<float> a(48000), b(48000);
        for (std::size_t i = 0; i < a.size(); ++i) {
            a[i] = static_cast<float>(0.7 * std::sin(2 * std::numbers::pi * 300.0 * i / fs));
            b[i] = static_cast<float>(0.004 * std::sin(2 * std::numbers::pi * 2000.0 * i / fs));
        }
        const std::vector<float> bIn = b;
        float* p[2] = {a.data(), b.data()};
        d.process(p, a.size());
        // ch1 gain = what a lone quiet channel gets
        const double ratio = energy(std::vector<float>(b.begin() + 24000, b.end())) /
                             energy(std::vector<float>(bIn.begin() + 24000 - 72, bIn.end() - 72));
        const double gainDb = 10 * std::log10(ratio);
        CHECK(gainDb > 3.0);  // lifted, not ducked by the loud channel
    }

    // A sudden loud burst after silence (a gunshot onset), at punch 0 and at the default punch.
    auto burst = [](const DynamicsParams& p, float& onsetPeak, double& sustainDb) {
        const double fs = 48000;
        ChannelDynamics d(fs, 1, p);
        std::vector<float> x(4800 + 19200, 0.0f);
        for (std::size_t i = 4800; i < x.size(); ++i) x[i] = static_cast<float>(0.9 * std::sin(2 * std::numbers::pi * 1000.0 * i / fs));
        float* ptr[1] = {x.data()};
        d.process(ptr, x.size());
        const std::size_t on = 4800 + d.latency();
        onsetPeak = 0;
        for (std::size_t i = on; i < on + 240; ++i) onsetPeak = std::max(onsetPeak, std::abs(x[i]));  // first 5 ms
        double e = 0;
        for (std::size_t i = x.size() - 2400; i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];  // after ~350 ms
        sustainDb = 10 * std::log10(e / 2400 / (0.9 * 0.9 / 2));  // re. input RMS
    };
    {
        DynamicsParams flat = med;
        flat.punch = 0;
        float onset;
        double sustain;
        burst(flat, onset, sustain);
        CHECK(onset < 0.5f);       // punch 0: the onset is flattened too
        CHECK(sustain < -8.0);
        burst(med, onset, sustain);  // default punch 60
        CHECK(onset > 0.7f);       // the shot's crack gets through...
        CHECK(sustain < -8.0);     // ...but sustained loudness is still tamed
        CHECK(ChannelDynamics(48000, 1, med).latency() == 72);
    }

    bool threw = false;
    try {
        dynamicsPreset("ultra");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void testEngineLiveControls() {
    const SpeakerLayout l = layout71();
    SphericalHeadParams hp;
    hp.irLength = 128;
    const HrirSet h = HrirSet::makeSphericalHead(48000, hp);
    EngineConfig cfg;
    cfg.limiter.enabled = false;
    cfg.driftCompensation = false;
    cfg.targetLatencyMs = 10;

    // Steady pink-ish noise on all channels at a moderate level.
    Rng rng;
    auto feed = [&](Engine& e, std::size_t frames) {
        std::vector<float> in(frames * 8);
        for (auto& v : in) v = rng.next() * 0.05f;
        e.pushInput(in.data(), frames);
    };
    auto pullPeak = [&](Engine& e, std::size_t frames, double* rms = nullptr) {
        std::vector<float> out(frames * 2);
        e.pullOutput(out.data(), frames);
        double pk = 0, s = 0;
        for (float v : out) {
            pk = std::max(pk, static_cast<double>(std::abs(v)));
            s += static_cast<double>(v) * v;
        }
        if (rms) *rms = std::sqrt(s / out.size());
        return pk;
    };

    // 1) Renderer swap (dry -> balanced room + dynamics + footsteps): completes, no burst.
    {
        Engine e(l, h, cfg);
        feed(e, 4800);
        double before = 0;
        for (int i = 0; i < 20; ++i) {
            feed(e, 480);
            before = std::max(before, pullPeak(e, 480));
        }
        BinauralConfig nc = cfg.binaural;
        nc.room = roomPreset("balanced");
        nc.dynamics = dynamicsPreset("medium");
        nc.dynamics.footstepDb = 4.0f;
        CHECK(e.reconfigure(nc, h));
        CHECK(e.swapInProgress());
        CHECK(!e.reconfigure(nc, h));  // refused while the first swap runs
        double during = 0;
        for (int i = 0; i < 10; ++i) {  // 4800 frames >> (8 + 4) blocks of 256
            feed(e, 480);
            during = std::max(during, pullPeak(e, 480));
        }
        CHECK(!e.swapInProgress());
        CHECK(during < 3.0 * before);   // no click/burst during the crossfade
        CHECK(e.reconfigure(cfg.binaural, h));  // next change accepted; frees the retired renderer
        for (int i = 0; i < 10; ++i) {
            feed(e, 480);
            pullPeak(e, 480);
        }
        CHECK(!e.swapInProgress());
    }

    // 2) Output gain: -6 dB halves the level.
    {
        Engine a(l, h, cfg), b(l, h, cfg);
        b.setOutputGainDb(-6.0206f);
        double ra = 0, rb = 0;
        for (int i = 0; i < 30; ++i) {
            Rng r1 = rng;
            feed(a, 480);
            rng = r1;
            feed(b, 480);
            pullPeak(a, 480, &ra);
            pullPeak(b, 480, &rb);
        }
        CHECK_NEAR(rb / ra, 0.5, 0.01);
    }

    // 3) Bypass (3D off): output becomes the plain stereo downmix.
    {
        Engine e(l, h, cfg);
        e.setBypass(true);
        std::vector<float> in(480 * 8, 0.0f), out(480 * 2);
        for (std::size_t i = 0; i < 480; ++i) in[i * 8 + 6] = 0.4f;  // SL only (DC)
        for (int k = 0; k < 20; ++k) {
            e.pushInput(in.data(), 480);
            e.pullOutput(out.data(), 480);
        }
        CHECK_NEAR(out[2 * 479], 0.4 * 0.7071, 1e-4);  // left = SL at -3 dB
        CHECK_NEAR(out[2 * 479 + 1], 0.0, 1e-4);       // nothing on the right
    }
}

void testFootstepBoost() {
    // Quiet 3.5 kHz content (footstep crunch) is lifted; the same band when loud (gun crack) is not.
    DynamicsParams p = dynamicsPreset("off");
    p.footstepDb = 6.0f;
    p.footstepSmart = false;  // classic: any quiet sound in the band
    CHECK(p.active());
    auto gainAt = [&](double inDb, double hz) {
        const double fs = 48000;
        ChannelDynamics d(fs, 1, p);
        std::vector<float> x(48000);
        const double amp = std::pow(10.0, inDb / 20.0) * std::sqrt(2.0);
        for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(amp * std::sin(2 * std::numbers::pi * hz * i / fs));
        const std::vector<float> in = x;
        float* ptr[1] = {x.data()};
        d.process(ptr, x.size());
        const std::size_t L = d.latency();
        return 10 * std::log10(energy(std::vector<float>(x.begin() + 24000, x.end())) /
                               energy(std::vector<float>(in.begin() + 24000 - L, in.end() - L)));
    };
    CHECK_NEAR(gainAt(-45.0, 3500.0), 6.0, 0.6);  // quiet crunch: full +6 dB
    CHECK_NEAR(gainAt(-10.0, 3500.0), 0.0, 0.3);  // loud crack: untouched
    CHECK_NEAR(gainAt(-45.0, 200.0), 0.0, 0.5);   // quiet but off-band: untouched
}

void testHeadphoneEq() {
    // AutoEq / Equalizer APO text -> bands; compact round trip; response; renderer applies it.
    const std::string text =
        "Preamp: -6.2 dB\r\n"
        "Filter 1: ON PK Fc 105 Hz Gain -2.4 dB Q 0.70\n"
        "Filter 2: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71\n"
        "Filter 3: OFF PK Fc 3000 Hz Gain 9 dB Q 1\n"
        "Filter 4: ON HSC Fc 10000 Hz Gain 2.0 dB Q 0.71\n"
        "Filter 5: ON PK Fc 2500 Hz Gain 4.0 dB Q 2.0\n";
    const HeadphoneEq eq = HeadphoneEq::parseText(text);
    CHECK_NEAR(eq.preampDb, -6.2, 1e-4);
    CHECK(eq.bands.size() == 4);  // the OFF filter is skipped
    CHECK(eq.bands[1].type == EqBand::Type::LowShelf);
    const HeadphoneEq rt = HeadphoneEq::fromCompact(eq.toCompact());
    CHECK(rt.bands.size() == 4);
    CHECK_NEAR(rt.responseDb(2500.0), eq.responseDb(2500.0), 0.01);
    // A lone peak filter: its gain at Fc, nothing far away.
    HeadphoneEq one;
    one.bands.push_back({EqBand::Type::Peak, 2500.0f, 4.0f, 2.0f});
    CHECK_NEAR(one.responseDb(2500.0), 4.0, 0.05);
    CHECK_NEAR(one.responseDb(100.0), 0.0, 0.05);
    bool threw = false;
    try {
        (void)HeadphoneEq::parseText("hello world");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Renderer: a front sine at 2.5 kHz comes out 4 dB louder with the peak filter.
    const SpeakerLayout lay = layoutFromChannelMask(0x4);  // centre only
    const HrirSet h = HrirSet::makeSphericalHead(48000);
    auto level = [&](const HeadphoneEq& e) {
        BinauralConfig c;
        c.blockSize = 256;
        c.eq = e;
        AudioBuffer in;
        in.sampleRate = 48000;
        in.channelMask = 0x4;
        in.resize(1, 48000);
        for (std::size_t i = 0; i < 48000; ++i)
            in.channels[0][i] = static_cast<float>(0.1 * std::sin(2 * std::numbers::pi * 2500.0 * i / 48000.0));
        const AudioBuffer out = renderBinaural(in, lay, h, c);
        return 10 * std::log10(energy(std::vector<float>(out.channels[0].begin() + 24000, out.channels[0].begin() + 47000)));
    };
    CHECK_NEAR(level(one) - level(HeadphoneEq{}), 4.0, 0.1);
}

void testFootstepDetector() {
    // Enemy footsteps light the ring towards them; everything else that is short and crunchy
    // (reloads, single / automatic fire, own steps straight ahead) and loud or steady sounds do not.
    const double fs = 48000;
    Rng rng;
    const SpeakerLayout lay = layoutFromChannelMask(0x63F);  // 7.1: FL FR FC LFE BL BR SL SR
    std::vector<float> az;
    std::vector<bool> lfe;
    for (const auto& sp : lay.speakers) {
        az.push_back(sp.azimuthDeg);
        lfe.push_back(sp.isLfe);
    }
    auto ch = [&](const char* name) {
        for (std::size_t c = 0; c < lay.size(); ++c)
            if (lay.speakers[c].name == name) return c;
        return std::size_t{0};
    };
    auto atDb = [](std::vector<float> v, double peakDb) {
        float pk = 0;
        for (float s : v) pk = std::max(pk, std::abs(s));
        const float g = static_cast<float>(std::pow(10.0, peakDb / 20.0)) / std::max(pk, 1e-9f);
        for (auto& s : v) s *= g;
        return v;
    };
    // A scene: quiet ambience on every speaker plus `sound` on the given speakers (gain each).
    // Returns the strongest activity on any speaker, and on `want`.
    struct Out {
        float any[3] = {}, want[3] = {};  // per level: sensitive, balanced, strict
        FootstepTracker::Stats st;
    };
    float ambience = 0.0005f;  // background noise on every speaker
    auto run = [&](const std::vector<float>& sound, std::vector<std::pair<std::size_t, float>> on, std::size_t want) {
        const std::size_t lead = 24000, n = lead + sound.size() + 24000;
        std::vector<std::vector<float>> x(lay.size(), std::vector<float>(n));
        for (auto& c : x)
            for (auto& v : c) v = ambience * rng.next();
        for (auto [c, g] : on)
            for (std::size_t i = 0; i < sound.size(); ++i) x[c][lead + i] += g * sound[i];
        FootstepTracker t(fs, az, lfe);
        std::vector<const float*> ptr;
        for (auto& c : x) ptr.push_back(c.data());
        Out o;
        for (std::size_t off = 0; off < n; off += 480) {  // 10 ms blocks
            std::vector<const float*> p = ptr;
            for (auto& q : p) q += off;
            t.process(p.data(), std::min<std::size_t>(480, n - off));
            for (std::size_t c = 0; c < lay.size(); ++c)
                for (int l = 0; l < 3; ++l) {
                    const float a = t.takeMax(c, l);
                    o.any[l] = std::max(o.any[l], a);
                    if (c == want) o.want[l] = std::max(o.want[l], a);
                }
        }
        o.st = t.totals();
        return o;
    };
    const std::size_t FL = ch("FL"), FR = ch("FR"), FC = ch("FC"), SL = ch("SL"), SR = ch("SR"), BL = ch("BL");

    // Footsteps peaking at -26 dBFS (a nearby enemy in a game mix): walking and running.
    const std::vector<float> walk = atDb(synthFootsteps(fs, 5, 0.42, 7), -26.0);
    const std::vector<float> run4 = atDb(synthFootsteps(fs, 5, 0.3, 11), -26.0);
    const Out oWalk = run(walk, {{SL, 1.0f}, {BL, 0.35f}}, SL);
    const Out oRun = run(run4, {{FR, 0.7f}, {SR, 0.7f}}, SR);
    // Short crunchy impacts that are not steps.
    auto click = [&](double at, std::vector<float>& v) {  // metallic click: sharp tick + short ring
        Biquad hp = Biquad::highpass(fs, 2500.0), bp = Biquad::bandpass(fs, 4200.0, 8.0);
        const auto s0 = static_cast<std::size_t>(at * fs);
        for (std::size_t i = 0; i < 1200 && s0 + i < v.size(); ++i) {
            const double t = static_cast<double>(i) / fs;
            const float x = rng.next();
            v[s0 + i] += hp.process(x) * static_cast<float>(std::exp(-t / 0.003)) +
                         bp.process(x) * static_cast<float>(3.0 * std::exp(-t / 0.015));
        }
    };
    std::vector<float> reload(96000, 0.0f);  // mag out, mag in, slap, bolt: irregular
    for (double t : {0.05, 0.4, 0.95, 1.15, 1.7}) click(t, reload);
    reload = atDb(reload, -26.0);
    std::vector<float> ownReload(96000, 0.0f);  // a regular one (worst case), but your own: straight ahead
    for (double t : {0.05, 0.5, 0.95, 1.4}) click(t, ownReload);
    ownReload = atDb(ownReload, -24.0);
    auto shotAt = [&](double at, std::vector<float>& v) {  // crack + boom
        Biquad hp = Biquad::highpass(fs, 900.0), lp = Biquad::lowpass(fs, 140.0);
        const auto s0 = static_cast<std::size_t>(at * fs);
        for (std::size_t i = 0; i < 9600 && s0 + i < v.size(); ++i) {
            const double t = static_cast<double>(i) / fs;
            const float x = rng.next();
            v[s0 + i] += hp.process(x) * static_cast<float>(1.6 * std::exp(-t / 0.012)) +
                         lp.process(x) * static_cast<float>(3.0 * std::exp(-t / 0.09));
        }
    };
    std::vector<float> farShot(24000, 0.0f), autoFire(96000, 0.0f), loudShot(24000, 0.0f);
    shotAt(0.01, farShot);
    farShot = atDb(farShot, -26.0);
    for (int k = 0; k < 13; ++k) shotAt(0.05 + 0.1 * k, autoFire);  // 600 rounds / minute, far away
    autoFire = atDb(autoFire, -26.0);
    std::vector<float> slowFire(96000, 0.0f);  // 450 rounds / minute
    for (int k = 0; k < 11; ++k) shotAt(0.05 + 0.1333 * k, slowFire);
    slowFire = atDb(slowFire, -26.0);
    shotAt(0.01, loudShot);
    loudShot = atDb(loudShot, -1.0);
    std::vector<float> rain(96000);  // steady band noise
    {
        Biquad bp = Biquad::bandpass(fs, 3500.0, 0.7);
        for (auto& s : rain) s = bp.process(rng.next());
        rain = atDb(rain, -26.0);
    }
    std::vector<float> boom(48000, 0.0f);  // distant explosion: mostly rumble
    {
        Biquad lp = Biquad::lowpass(fs, 180.0, 1.0);
        for (std::size_t i = 0; i < boom.size(); ++i)
            boom[i] = lp.process(rng.next()) * static_cast<float>(std::exp(-static_cast<double>(i) / fs / 0.4));
        boom = atDb(boom, -24.0);
    }
    const Out oReload = run(reload, {{SR, 1.0f}}, SR), oOwnReload = run(ownReload, {{FL, 0.8f}, {FR, 0.8f}}, FL),
              oOwnSteps = run(walk, {{FL, 0.7f}, {FR, 0.7f}, {FC, 0.5f}}, FC), oFarShot = run(farShot, {{SR, 1.0f}}, SR),
              oAuto = run(autoFire, {{SR, 1.0f}}, SR), oSlowFire = run(slowFire, {{BL, 1.0f}}, BL),
              oLoud = run(loudShot, {{SR, 1.0f}}, SR), oRain = run(rain, {{SR, 1.0f}}, SR),
              oBoom = run(boom, {{SL, 1.0f}}, SL), oQuiet = run(std::vector<float>(48000, 0.0f), {}, SL);
    // A distant enemy in a busy mix: quieter steps over 10x louder ambience.
    ambience = 0.005f;
    const Out oBusy = run(atDb(synthFootsteps(fs, 6, 0.45, 23), -32.0), {{BL, 1.0f}, {SL, 0.5f}}, BL);
    ambience = 0.0005f;
    auto row = [](const char* name, const Out& o, bool want) {
        const float* v = want ? o.want : o.any;
        std::printf("    %-11s sensitive %.2f  balanced %.2f  strict %.2f  (%d impacts, %d own)\n", name, v[0], v[1], v[2],
                    o.st.impacts, o.st.own);
    };
    row("walk", oWalk, true);
    row("run", oRun, true);
    row("busy mix", oBusy, true);
    row("reload", oReload, false);
    row("own reload", oOwnReload, false);
    row("own steps", oOwnSteps, false);
    row("far shot", oFarShot, false);
    row("auto fire", oAuto, false);
    row("450 rpm", oSlowFire, false);
    row("loud shot", oLoud, false);
    row("rain", oRain, false);
    row("explosion", oBoom, false);
    row("silence", oQuiet, false);
    // Enemy steps: every level, on the right speaker.
    for (int l = 0; l < 3; ++l) {
        CHECK(oWalk.want[l] > 0.6f);
        CHECK(oRun.want[l] > 0.6f);
    }
    CHECK(oBusy.want[0] > 0.6f);
    CHECK(oBusy.want[1] > 0.6f);
    // Your own sounds: heard, ignored at every level.
    CHECK(oOwnReload.st.impacts >= 3);
    CHECK(oOwnSteps.st.impacts >= 3);
    for (int l = 0; l < 3; ++l) {
        CHECK(oOwnReload.any[l] < 0.05f);
        CHECK(oOwnSteps.any[l] < 0.05f);
        CHECK(oLoud.any[l] < 0.05f);
        CHECK(oBoom.any[l] < 0.05f);
        CHECK(oQuiet.any[l] < 0.01f);
    }
    // Single shot: not at Balanced / Strict. Reloads and automatic fire: not at Strict.
    CHECK(oReload.st.impacts >= 3);
    CHECK(oFarShot.any[1] < 0.05f);
    CHECK(oRain.any[1] < 0.05f);  // the rain starting is one impact (Sensitive), then nothing
    CHECK(oRain.any[2] < 0.05f);
    CHECK(oFarShot.any[2] < 0.05f);
    CHECK(oReload.any[2] < 0.05f);
    CHECK(oAuto.any[1] < 0.05f);
    CHECK(oSlowFire.any[1] < 0.05f);
    CHECK(oAuto.any[2] < 0.05f);
    CHECK(oSlowFire.any[2] < 0.05f);
}

void testSmartFootsteps() {
    // Smart mode: short impacts in the footstep band are lifted, a steady sound in the same band
    // (rain, engine hum) is not.
    DynamicsParams p = dynamicsPreset("off");
    p.footstepDb = 6.0f;
    p.footstepSmart = true;
    const double fs = 48000;
    Rng rng;
    auto bandNoise = [&](std::size_t n) {  // noise around 3.5 kHz
        Biquad bp = Biquad::bandpass(fs, 3500.0, 0.8);
        std::vector<float> v(n);
        for (auto& x : v) x = bp.process(rng.next());
        return v;
    };
    auto scale = [](std::vector<float>& v, double db) {
        const double g = std::pow(10.0, db / 20.0) / std::sqrt(energy(v) / static_cast<double>(v.size()));
        for (auto& x : v) x = static_cast<float>(x * g);
    };
    auto gainDb = [&](const std::vector<float>& in, std::size_t from) {
        ChannelDynamics d(fs, 1, p);
        std::vector<float> x = in;
        float* ptr[1] = {x.data()};
        d.process(ptr, x.size());
        const auto L = static_cast<std::ptrdiff_t>(d.latency());
        const auto f = static_cast<std::ptrdiff_t>(from);
        return 10 * std::log10(energy(std::vector<float>(x.begin() + f, x.end())) /
                               energy(std::vector<float>(in.begin() + f - L, in.end() - L)));
    };
    // Steady band noise at -45 dB (rain, engine): classic mode lifts it, smart mode does not.
    std::vector<float> steady = bandNoise(96000);
    scale(steady, -45.0);
    const double steadySmart = gainDb(steady, 48000);
    p.footstepSmart = false;
    const double steadyClassic = gainDb(steady, 48000);
    // Footsteps: 60 ms bursts every 400 ms at -40 dB: smart mode lifts them about as much as classic.
    std::vector<float> burst = bandNoise(2880);
    scale(burst, -40.0);
    std::vector<float> steps(96000, 0.0f);
    for (std::size_t s = 0; s + burst.size() < steps.size(); s += 19200)
        for (std::size_t i = 0; i < burst.size(); ++i)
            steps[s + i] = burst[i] * std::sin(std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(burst.size()));
    const double stepsClassic = gainDb(steps, 48000);
    p.footstepSmart = true;
    const double stepsSmart = gainDb(steps, 48000);
    std::printf("    steady: classic %+.1f dB, smart %+.1f dB | steps: classic %+.1f dB, smart %+.1f dB\n",
                steadyClassic, steadySmart, stepsClassic, stepsSmart);
    CHECK(steadyClassic > 3.0);
    CHECK(steadySmart < 1.0);
    CHECK(stepsSmart > stepsClassic - 1.0);
    CHECK(stepsSmart > 3.0);
}

#ifdef TAC_TEST_SOFA_FILE
// Real measured data: verifies the SOFA -> TacAudio coordinate flip and ear assignment.
void testSofaKemar() {
    SofaImportReport rep;
    const HrirSet h = importSofa(TAC_TEST_SOFA_FILE, {}, &rep);
    CHECK(h.sampleRate == 48000.0);
    CHECK_NEAR(rep.sourceRate, 44100.0, 0.5);  // MIT KEMAR is 44.1 kHz -> resampled
    CHECK(h.irLength > 64 && h.irLength <= 512);
    CHECK(h.measurements.size() > 500);

    const auto& right = h.nearest(90, 0);  // TacAudio +90 = RIGHT
    CHECK(std::abs(right.azimuthDeg - 90.0f) < 6.0f);
    CHECK(energy(right.right) > 4.0 * energy(right.left));
    CHECK(firstAbove(right.right, 0.05f) < firstAbove(right.left, 0.05f));

    const auto& left = h.nearest(-90, 0);
    CHECK(energy(left.left) > 4.0 * energy(left.right));

    // Elevation sign: a source above has elevation > 0 in the set.
    const auto& up = h.nearest(0, 60);
    CHECK(up.elevationDeg > 40.0f);

    // Front normalisation: frontal pair has roughly unit level (within 6 dB).
    const auto& front = h.nearest(0, 0);
    const double e = 0.5 * (energy(front.left) + energy(front.right));
    CHECK(e > 0.05 && e < 20.0);
}

void testDiffuseFieldEqKemar() { checkDiffuseFieldEq(importSofa(TAC_TEST_SOFA_FILE), 1.5); }
#endif

}  // namespace

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"fft matches DFT", testFftMatchesDft},
        {"fft real round trip", testFftRealRoundTrip},
        {"convolver matches direct convolution", testConvolverMatchesDirect},
        {"convolver accumulate + reset", testConvolverAccumulateAndReset},
        {"wav round trip (float/pcm24/pcm16, 7.1 mask)", testWavRoundTrip},
        {"7.1 layout from channel mask", testLayout71},
        {"vbap gains", testVbap},
        {"spherical head ITD/ILD cues", testSphericalHeadCues},
        {".thrir save/load", testThrirRoundTrip},
        {"renderer direction + LFE", testRendererDirection},
        {"renderer rejects sample-rate mismatch", testRendererRejectsWrongRate},
        {"test scenes", testScenes},
        {"M2 ring buffer", testRingBuffer},
        {"M2 varispeed reader (identity + resampling)", testVarispeed},
        {"M2 limiter ceiling / transparency / release", testLimiter},
        {"M2 engine == offline renderer", testEngineMatchesOffline},
        {"M2 engine drift +400 ppm", testEngineDriftFast},
        {"M2 engine drift -400 ppm", testEngineDriftSlow},
        {"M2 engine recovers from input stall", testEngineUnderrunRecovers},
        {"M2 adaptive jitter buffer stops glitches", testEngineAdaptiveJitter},
        {"diffuse-field EQ (spherical head)", testDiffuseFieldEqSpherical},
        {"room reverb presets", testRoom},
        {"M3 dynamics: tame loud, lift quiet, independent channels", testDynamics},
        {"M3 footstep boost: quiet band only", testFootstepBoost},
        {"smart footsteps: impacts lifted, steady sound not", testSmartFootsteps},
        {"footstep detector: enemy steps yes; reloads, fire, own sounds, rain, explosions no", testFootstepDetector},
        {"headphone EQ: parse, round trip, response, renderer", testHeadphoneEq},
        {"engine live controls: swap, volume, 3D bypass", testEngineLiveControls},
#ifdef TAC_TEST_SOFA_FILE
        {"SOFA import: MIT KEMAR conventions", testSofaKemar},
        {"diffuse-field EQ (MIT KEMAR): flat average, cues kept", testDiffuseFieldEqKemar},
#endif
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ ok ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

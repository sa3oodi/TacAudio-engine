#include "core/hrtf.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <map>
#include <numbers>
#include <stdexcept>

#include "core/fft.h"

namespace tac {

namespace {

constexpr char kMagic[8] = {'T', 'H', 'R', 'I', 'R', '0', '1', '\0'};
constexpr double kDeg = std::numbers::pi / 180.0;

template <typename T>
void writePod(std::ofstream& f, const T& v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
T readPod(std::ifstream& f) {
    T v{};
    f.read(reinterpret_cast<char*>(&v), sizeof(T));
    if (!f) throw std::runtime_error("truncated .thrir file");
    return v;
}

// Response of the Brown-Duda head-shadow filter H(s) = (alpha*s + beta) / (s + beta),
// discretised with the bilinear transform, to a unit impulse. DC gain is exactly 1.
std::vector<double> headShadowImpulse(double alpha, double beta, double fs, std::size_t len) {
    const double K = 2.0 * fs;
    const double a0 = K + beta;
    const double b0 = (alpha * K + beta) / a0;
    const double b1 = (beta - alpha * K) / a0;
    const double a1 = (beta - K) / a0;
    std::vector<double> h(len, 0.0);
    double xPrev = 0, yPrev = 0;
    for (std::size_t n = 0; n < len; ++n) {
        const double x = n == 0 ? 1.0 : 0.0;
        const double y = b0 * x + b1 * xPrev - a1 * yPrev;
        h[n] = y;
        xPrev = x;
        yPrev = y;
    }
    return h;
}

// Woodworth/Brown-Duda arrival delay (seconds) for incidence angle theta from the ear axis.
double earDelaySeconds(double theta, double a, double c) {
    if (theta < std::numbers::pi / 2) return (a / c) * (1.0 - std::cos(theta));
    return (a / c) * (1.0 + theta - std::numbers::pi / 2);
}

// Delays `h` by a fractional number of samples using a Hann-windowed sinc (32 taps).
std::vector<float> fractionalDelay(const std::vector<double>& h, double delaySamples, std::size_t outLen) {
    constexpr int kHalf = 16;
    std::vector<float> out(outLen, 0.0f);
    for (std::size_t k = 0; k < h.size(); ++k) {
        if (h[k] == 0.0) continue;
        const double centre = static_cast<double>(k) + delaySamples;
        const int first = static_cast<int>(std::floor(centre)) - kHalf + 1;
        for (int n = first; n < first + 2 * kHalf; ++n) {
            if (n < 0 || n >= static_cast<int>(outLen)) continue;
            const double t = static_cast<double>(n) - centre;
            const double sinc = std::abs(t) < 1e-9 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
            const double w = 0.5 * (1.0 + std::cos(std::numbers::pi * t / kHalf));
            out[static_cast<std::size_t>(n)] += static_cast<float>(h[k] * sinc * w);
        }
    }
    return out;
}

}  // namespace

Vec3 directionToVector(double azimuthDeg, double elevationDeg) noexcept {
    const double az = azimuthDeg * kDeg, el = elevationDeg * kDeg;
    return {std::cos(el) * std::sin(az), std::cos(el) * std::cos(az), std::sin(el)};
}

const HrirMeasurement& HrirSet::nearest(float azimuthDeg, float elevationDeg) const {
    if (measurements.empty()) throw std::runtime_error("HrirSet is empty");
    const Vec3 t = directionToVector(azimuthDeg, elevationDeg);
    const HrirMeasurement* best = &measurements.front();
    double bestDot = -2.0;
    for (const auto& m : measurements) {
        const Vec3 v = directionToVector(m.azimuthDeg, m.elevationDeg);
        const double d = v.x * t.x + v.y * t.y + v.z * t.z;
        if (d > bestDot) {
            bestDot = d;
            best = &m;
        }
    }
    return *best;
}

void HrirSet::save(const std::filesystem::path& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f.write(kMagic, sizeof(kMagic));
    writePod(f, static_cast<std::uint32_t>(sampleRate));
    writePod(f, static_cast<std::uint32_t>(measurements.size()));
    writePod(f, static_cast<std::uint32_t>(irLength));
    for (const auto& m : measurements) {
        writePod(f, m.azimuthDeg);
        writePod(f, m.elevationDeg);
        f.write(reinterpret_cast<const char*>(m.left.data()), static_cast<std::streamsize>(irLength * sizeof(float)));
        f.write(reinterpret_cast<const char*>(m.right.data()), static_cast<std::streamsize>(irLength * sizeof(float)));
    }
}

HrirSet HrirSet::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    char magic[8];
    f.read(magic, sizeof(magic));
    if (!f || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) throw std::runtime_error("not a .thrir file: " + path.string());

    HrirSet s;
    s.name = path.stem().string();
    s.sampleRate = readPod<std::uint32_t>(f);
    const auto count = readPod<std::uint32_t>(f);
    s.irLength = readPod<std::uint32_t>(f);
    if (count == 0 || s.irLength == 0 || s.irLength > 65536) throw std::runtime_error("invalid .thrir header");
    s.measurements.resize(count);
    for (auto& m : s.measurements) {
        m.azimuthDeg = readPod<float>(f);
        m.elevationDeg = readPod<float>(f);
        m.left.resize(s.irLength);
        m.right.resize(s.irLength);
        f.read(reinterpret_cast<char*>(m.left.data()), static_cast<std::streamsize>(s.irLength * sizeof(float)));
        f.read(reinterpret_cast<char*>(m.right.data()), static_cast<std::streamsize>(s.irLength * sizeof(float)));
        if (!f) throw std::runtime_error("truncated .thrir file");
    }
    return s;
}

std::vector<double> HrirSet::diffuseFieldPower(std::size_t fftSize) const {
    const std::size_t N = fftSize;
    Fft fft(N);
    const std::size_t bins = N / 2 + 1;
    std::vector<double> power(bins, 0.0);
    if (measurements.empty()) return power;

    // Equal-area weighting: each elevation ring gets weight cos(el), shared by its points.
    // Works for grids with equal or cos(el)-thinned azimuth spacing alike.
    std::map<int, int> ringCount;
    for (const auto& m : measurements) ringCount[static_cast<int>(std::lround(m.elevationDeg))]++;

    std::vector<float> buf(N);
    std::vector<cpx> half(bins), scratch(N);
    double wsum = 0;
    for (const auto& m : measurements) {
        const int ring = static_cast<int>(std::lround(m.elevationDeg));
        const double w = std::max(0.02, std::cos(m.elevationDeg * std::numbers::pi / 180.0)) / ringCount[ring];
        for (const auto* ir : {&m.left, &m.right}) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            std::copy_n(ir->begin(), std::min(ir->size(), N), buf.begin());
            fft.forwardReal(buf.data(), half.data(), scratch.data());
            for (std::size_t k = 0; k < bins; ++k) power[k] += 0.5 * w * std::norm(half[k]);
        }
        wsum += w;
    }
    for (auto& v : power) v /= wsum;
    return power;
}

DiffuseFieldEqReport HrirSet::diffuseFieldEqualize(const DiffuseFieldEqParams& p) {
    DiffuseFieldEqReport rep;
    if (measurements.empty()) return rep;
    const std::size_t newLen = irLength + p.extraTaps;
    const std::size_t N = nextPowerOfTwo(std::max<std::size_t>(4096, 2 * newLen));
    const std::size_t bins = N / 2 + 1;
    const double binHz = sampleRate / static_cast<double>(N);

    // 1) Direction-averaged power, smoothed in fractional octaves.
    const std::vector<double> P = diffuseFieldPower(N);
    std::vector<double> Ps(bins, 0.0);
    const double halfBand = std::pow(2.0, p.smoothingOctaves / 2.0);
    for (std::size_t k = 1; k < bins; ++k) {
        const double f = static_cast<double>(k) * binHz;
        const auto lo = std::max<std::size_t>(1, static_cast<std::size_t>(f / halfBand / binHz));
        const auto hi = std::min(bins - 1, static_cast<std::size_t>(std::ceil(f * halfBand / binHz)));
        double s = 0;
        for (std::size_t j = lo; j <= hi; ++j) s += P[j];
        Ps[k] = s / static_cast<double>(hi - lo + 1);
    }
    Ps[0] = Ps[1];

    // 2) Inverse magnitude (dB), held outside [lowHold, highHold] and limited.
    std::vector<double> gDb(bins);
    for (std::size_t k = 0; k < bins; ++k) {
        const double f = std::clamp(static_cast<double>(k) * binHz, p.lowHoldHz, p.highHoldHz);
        const auto kk = std::min(bins - 1, static_cast<std::size_t>(std::lround(f / binHz)));
        gDb[k] = std::clamp(-10.0 * std::log10(std::max(Ps[kk], 1e-20)), -p.maxCutDb, p.maxBoostDb);
        rep.maxBoostDb = std::max(rep.maxBoostDb, gDb[k]);
        rep.maxCutDb = std::max(rep.maxCutDb, -gDb[k]);
    }

    // 3) Minimum-phase filter from that magnitude (real-cepstrum folding).
    Fft fft(N);
    std::vector<cpx> c(N);
    for (std::size_t k = 0; k < N; ++k) {
        const std::size_t kk = k <= N / 2 ? k : N - k;
        c[k] = cpx(static_cast<float>(gDb[kk] * std::log(10.0) / 20.0), 0.0f);  // ln|G|
    }
    fft.inverse(c.data());  // real cepstrum
    for (std::size_t n = 1; n < N / 2; ++n) {
        c[n] *= 2.0f;
        c[N - n] = 0.0f;
    }
    fft.forward(c.data());  // log of the minimum-phase spectrum
    std::vector<cpx> G(N);
    for (std::size_t k = 0; k < N; ++k) G[k] = std::exp(c[k]);

    // 4) Apply to every HRIR (frequency domain, N >> irLength + filter decay: no wrap-around).
    const std::size_t fade = std::min<std::size_t>(64, newLen / 4);
    std::vector<cpx> X(N);
    for (auto& m : measurements) {
        for (auto* ir : {&m.left, &m.right}) {
            std::fill(X.begin(), X.end(), cpx{});
            for (std::size_t i = 0; i < ir->size() && i < N; ++i) X[i] = cpx((*ir)[i], 0.0f);
            fft.forward(X.data());
            for (std::size_t k = 0; k < N; ++k) X[k] *= G[k];
            fft.inverse(X.data());
            ir->assign(newLen, 0.0f);
            for (std::size_t i = 0; i < newLen; ++i) (*ir)[i] = X[i].real();
            for (std::size_t i = 0; i < fade; ++i)  // half-Hann fade-out of the truncated tail
                (*ir)[newLen - fade + i] *= static_cast<float>(0.5 * (1.0 + std::cos(std::numbers::pi * (i + 1) / fade)));
        }
    }
    irLength = newLen;
    return rep;
}

HrirSet HrirSet::makeSphericalHead(double fs, const SphericalHeadParams& p) {
    HrirSet s;
    s.name = "spherical-head (built-in)";
    s.sampleRate = fs;
    s.irLength = p.irLength;

    const double a = p.headRadiusM, c = p.speedOfSound;
    const double beta = 2.0 * c / a;
    constexpr double alphaMin = 0.1;
    constexpr double thetaMin = 150.0 * kDeg;
    constexpr double kPreDelay = 16.0;  // keeps the windowed-sinc delay causal

    // One-pole low-pass for the rear cue (~3 kHz).
    const double rearPole = std::exp(-2.0 * std::numbers::pi * 3000.0 / fs);

    const Vec3 leftEar{-1, 0, 0}, rightEar{1, 0, 0};

    for (double el = p.elevationMinDeg; el <= p.elevationMaxDeg + 1e-9; el += p.elevationStepDeg) {
        const bool pole = std::abs(el - 90.0) < 1e-9;
        const double azStep = pole ? 360.0 : p.azimuthStepDeg;
        for (double az = -180.0 + azStep; az <= 180.0 + 1e-9; az += azStep) {
            const Vec3 src = directionToVector(az, el);
            HrirMeasurement m;
            m.azimuthDeg = static_cast<float>(pole ? 0.0 : az);
            m.elevationDeg = static_cast<float>(el);

            for (int ear = 0; ear < 2; ++ear) {
                const Vec3& e = ear == 0 ? leftEar : rightEar;
                const double cosTheta = std::clamp(src.x * e.x + src.y * e.y + src.z * e.z, -1.0, 1.0);
                const double theta = std::acos(cosTheta);
                const double alpha = (1.0 + alphaMin / 2.0) + (1.0 - alphaMin / 2.0) * std::cos(theta / thetaMin * std::numbers::pi);

                std::vector<double> h = headShadowImpulse(alpha, beta, fs, p.irLength / 2);

                // Rear cue: blend in a low-passed version for sources behind the head.
                const double rear = p.rearHfCut * std::max(0.0, -src.y);
                if (rear > 0.0) {
                    double state = 0.0;
                    for (auto& v : h) {
                        state = (1.0 - rearPole) * v + rearPole * state;
                        v = (1.0 - rear) * v + rear * state;
                    }
                }

                const double delay = kPreDelay + earDelaySeconds(theta, a, c) * fs;
                auto ir = fractionalDelay(h, delay, p.irLength);
                (ear == 0 ? m.left : m.right) = std::move(ir);
            }
            s.measurements.push_back(std::move(m));
        }
    }
    return s;
}

}  // namespace tac

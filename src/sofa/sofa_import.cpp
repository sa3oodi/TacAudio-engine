#include "sofa/sofa_import.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>

#include "core/fft.h"

extern "C" {
#include "mysofa.h"
}

namespace tac {

namespace {

struct SofaDeleter {
    void operator()(MYSOFA_HRTF* h) const noexcept { mysofa_free(h); }
};
using SofaPtr = std::unique_ptr<MYSOFA_HRTF, SofaDeleter>;

double bandGain(const std::vector<float>& left, const std::vector<float>& right, double fs) {
    constexpr std::size_t n = 4096;
    Fft fft(n);
    std::vector<float> buf(n);
    std::vector<cpx> half(fft.halfBins()), scratch(n);
    double acc = 0;
    std::size_t count = 0;
    for (const auto* ir : {&left, &right}) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        std::copy_n(ir->begin(), std::min(ir->size(), n), buf.begin());
        fft.forwardReal(buf.data(), half.data(), scratch.data());
        for (std::size_t k = 0; k < half.size(); ++k) {
            const double f = static_cast<double>(k) * fs / n;
            if (f < 200.0 || f > 2000.0) continue;
            acc += std::norm(half[k]);
            ++count;
        }
    }
    return count ? std::sqrt(acc / static_cast<double>(count)) : 1.0;
}

}  // namespace

HrirSet importSofa(const std::filesystem::path& path, const SofaImportOptions& opt, SofaImportReport* report) {
    int err = 0;
    SofaPtr h(mysofa_load(path.string().c_str(), &err));
    if (!h || err != MYSOFA_OK) throw std::runtime_error("cannot load SOFA file " + path.string() + " (libmysofa error " + std::to_string(err) + ")");
    if ((err = mysofa_check(h.get())) != MYSOFA_OK)
        throw std::runtime_error("SOFA file is not a supported SimpleFreeFieldHRIR set (libmysofa error " + std::to_string(err) + ")");
    if (h->R != 2) throw std::runtime_error("SOFA file must have exactly 2 receivers (ears)");

    SofaImportReport rep;
    rep.sourceRate = h->DataSamplingRate.values[0];
    rep.sourceTaps = h->N;

    if (std::abs(rep.sourceRate - opt.sampleRate) > 0.5) {
        if ((err = mysofa_resample(h.get(), static_cast<float>(opt.sampleRate))) != MYSOFA_OK)
            throw std::runtime_error("SOFA resampling failed (libmysofa error " + std::to_string(err) + ")");
    }
    mysofa_tocartesian(h.get());  // SOFA axes: x front, y left, z up

    const std::size_t M = h->M, N = h->N;
    const float* ir = h->DataIR.values;
    const float* src = h->SourcePosition.values;

    // Left ear = receiver with larger y.
    std::size_t leftIdx = 0, rightIdx = 1;
    if (h->ReceiverPosition.values && h->ReceiverPosition.elements >= 6 && h->ReceiverPosition.values[1] < h->ReceiverPosition.values[4]) {
        leftIdx = 1;
        rightIdx = 0;
        rep.receiversSwapped = true;
    }

    // Common onset across every IR (keeps ITD intact), minus a small pre-roll.
    float peak = 0;
    for (std::size_t i = 0; i < M * 2 * N; ++i) peak = std::max(peak, std::abs(ir[i]));
    std::size_t onset = N;
    for (std::size_t m = 0; m < M; ++m)
        for (std::size_t r = 0; r < 2; ++r)
            for (std::size_t n = 0; n < N; ++n)
                if (std::abs(ir[(m * 2 + r) * N + n]) > peak * 0.01f) {
                    onset = std::min(onset, n);
                    break;
                }
    const std::size_t start = onset > 16 && onset < N ? onset - 16 : 0;
    const std::size_t taps = std::min(opt.maxTaps, N - start);
    const std::size_t fade = std::min<std::size_t>(32, taps / 4);
    rep.trimmedLeading = start;

    HrirSet set;
    set.name = path.stem().string();
    set.sampleRate = opt.sampleRate;
    set.irLength = taps;
    set.measurements.resize(M);
    for (std::size_t m = 0; m < M; ++m) {
        const double x = src[m * 3 + 0], y = src[m * 3 + 1], z = src[m * 3 + 2];
        const double r = std::sqrt(x * x + y * y + z * z);
        auto& meas = set.measurements[m];
        double az = -std::atan2(y, x) * 180.0 / std::numbers::pi;  // negate: SOFA +y(left) -> -90
        if (az <= -180.0) az += 360.0;
        meas.azimuthDeg = static_cast<float>(az);
        meas.elevationDeg = r > 0 ? static_cast<float>(std::asin(std::clamp(z / r, -1.0, 1.0)) * 180.0 / std::numbers::pi) : 0.0f;
        for (int ear = 0; ear < 2; ++ear) {
            const std::size_t rec = ear == 0 ? leftIdx : rightIdx;
            const float* p = ir + (m * 2 + rec) * N + start;
            std::vector<float> v(p, p + taps);
            for (std::size_t i = 0; i < fade; ++i)  // half-Hann fade-out
                v[taps - fade + i] *= static_cast<float>(0.5 * (1.0 + std::cos(std::numbers::pi * (i + 1) / fade)));
            (ear == 0 ? meas.left : meas.right) = std::move(v);
        }
    }

    if (opt.normalizeFront) {
        const auto& front = set.nearest(0.0f, 0.0f);
        const double g = 1.0 / bandGain(front.left, front.right, opt.sampleRate);
        for (auto& m : set.measurements) {
            for (float& v : m.left) v = static_cast<float>(v * g);
            for (float& v : m.right) v = static_cast<float>(v * g);
        }
        rep.frontGainDb = 20.0 * std::log10(g);
    }

    if (report) *report = rep;
    return set;
}

}  // namespace tac

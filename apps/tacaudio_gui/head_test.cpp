#include "head_test.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>

#include "core/hrtf.h"
#include "platform/wasapi.h"

namespace tac {

namespace {
constexpr double kRate = 48000.0;

// Three 150 ms bursts of pink noise (10 ms fades, 100 ms gaps): broadband, like footsteps, with
// enough onsets to localise.
std::vector<float> makeStimulus(std::mt19937& rng) {
    const auto burst = static_cast<std::size_t>(0.150 * kRate), gap = static_cast<std::size_t>(0.100 * kRate),
               fade = static_cast<std::size_t>(0.010 * kRate);
    std::vector<float> s(3 * burst + 2 * gap, 0.0f);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;  // Paul Kellet's pink filter
    for (int k = 0; k < 3; ++k) {
        const std::size_t start = static_cast<std::size_t>(k) * (burst + gap);
        for (std::size_t i = 0; i < burst; ++i) {
            const float w = u(rng);
            b0 = 0.99886f * b0 + w * 0.0555179f;
            b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f;
            b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f;
            b5 = -0.7616f * b5 - w * 0.0168980f;
            const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.11f;
            b6 = w * 0.115926f;
            float env = 1.0f;
            if (i < fade) env = 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * static_cast<float>(i) / fade);
            else if (i >= burst - fade)
                env = 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * static_cast<float>(burst - i) / fade);
            s[start + i] = pink * env;
        }
    }
    return s;
}

std::vector<float> convolve(const std::vector<float>& x, const std::vector<float>& h) {
    std::vector<float> y(x.size() + h.size() - 1, 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float v = x[i];
        if (v == 0.0f) continue;
        float* o = y.data() + i;
        for (std::size_t k = 0; k < h.size(); ++k) o[k] += v * h[k];
    }
    return y;
}

double rms(const std::vector<float>& v) {
    double e = 0;
    for (float x : v) e += static_cast<double>(x) * x;
    return std::sqrt(e / std::max<std::size_t>(1, v.size()));
}

double angleDiff(double a, double b) {
    double d = std::fmod(std::abs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}
}  // namespace

HeadTest::HeadTest() = default;
HeadTest::~HeadTest() { close(); }

void HeadTest::prepare(const std::vector<cli::HeadInfo>& heads, const std::string& outputDevice, unsigned seed) {
    if (heads.empty()) throw std::runtime_error("no heads installed");
    heads_ = heads;
    std::mt19937 rng(seed);
    const std::vector<float> stim = makeStimulus(rng);

    // Every head x every direction, in random order (so no head is always heard first).
    trials_.clear();
    for (int h = 0; h < static_cast<int>(heads_.size()); ++h)
        for (int d = 0; d < kDirs; ++d) trials_.push_back({h, d, -1});
    std::shuffle(trials_.begin(), trials_.end(), rng);

    std::vector<std::vector<std::vector<float>>> rendered(heads_.size());  // [head][dir] interleaved
    for (std::size_t h = 0; h < heads_.size(); ++h) {
        cli::Args a;
        a.opts["--head"] = heads_[h].name;
        const HrirSet set = cli::loadHrirs(a, kRate);
        std::vector<std::vector<float>> l(kDirs), r(kDirs);
        for (int d = 0; d < kDirs; ++d) {
            const auto& m = set.nearest(kAzimuth[d], 0.0f);
            l[static_cast<std::size_t>(d)] = convolve(stim, m.left);
            r[static_cast<std::size_t>(d)] = convolve(stim, m.right);
        }
        // Same loudness for every head (measured from the front), level differences between
        // directions are kept: they are part of the cue.
        const double front = 0.5 * (rms(l[0]) + rms(r[0]));
        const float g = front > 0 ? static_cast<float>(std::pow(10.0, -24.0 / 20.0) / front) : 1.0f;
        rendered[h].resize(kDirs);
        for (int d = 0; d < kDirs; ++d) {
            const auto& L = l[static_cast<std::size_t>(d)];
            const auto& R = r[static_cast<std::size_t>(d)];
            auto& o = rendered[h][static_cast<std::size_t>(d)];
            o.resize(2 * L.size());
            for (std::size_t i = 0; i < L.size(); ++i) {
                o[2 * i] = std::clamp(L[i] * g, -1.0f, 1.0f);
                o[2 * i + 1] = std::clamp(R[i] * g, -1.0f, 1.0f);
            }
        }
    }
    sounds_.clear();
    for (const auto& t : trials_) sounds_.push_back(rendered[static_cast<std::size_t>(t.head)][static_cast<std::size_t>(t.dir)]);

    const auto dev = wasapi::findDevice(outputDevice, /*render=*/true);
    if (!dev) throw std::runtime_error("headphones '" + outputDevice + "' not found");
    out_ = std::make_unique<wasapi::RenderStream>();
    wasapi::RenderOptions ro;
    ro.exclusive = false;
    ro.lowLatency = false;
    out_->open(*dev, static_cast<std::uint32_t>(kRate), 2, ro);
    out_->start([this](float* d, std::size_t n) {
        const float* src = data_.load(std::memory_order_acquire);
        const std::size_t len = len_.load(std::memory_order_acquire);
        std::size_t pos = pos_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i) {
            const bool on = src && 2 * pos + 1 < len;
            d[2 * i] = on ? src[2 * pos] : 0.0f;
            d[2 * i + 1] = on ? src[2 * pos + 1] : 0.0f;
            if (on) ++pos;
        }
        pos_.store(pos, std::memory_order_relaxed);
    });
}

void HeadTest::close() {
    data_.store(nullptr);
    if (out_) {
        out_->stop();
        out_.reset();
    }
}

void HeadTest::play(std::size_t i) {
    if (i >= sounds_.size()) return;
    data_.store(nullptr, std::memory_order_release);
    len_.store(sounds_[i].size(), std::memory_order_release);
    pos_.store(0, std::memory_order_relaxed);
    data_.store(sounds_[i].data(), std::memory_order_release);
}

bool HeadTest::playing() const {
    return data_.load() && 2 * pos_.load() + 1 < len_.load();
}

std::vector<HeadTest::Result> HeadTest::results() const {
    std::vector<Result> res(heads_.size());
    std::vector<double> sum(heads_.size(), 0.0);
    for (std::size_t h = 0; h < heads_.size(); ++h) {
        res[h].name = heads_[h].name;
        res[h].description = heads_[h].description;
    }
    for (const auto& t : trials_) {
        if (t.answer < 0) continue;
        const double played = kAzimuth[t.dir], heard = kAzimuth[t.answer];
        auto& r = res[static_cast<std::size_t>(t.head)];
        sum[static_cast<std::size_t>(t.head)] += angleDiff(played, heard);
        ++r.answered;
        const bool pf = std::abs(played) < 90, pb = std::abs(played) > 90;  // 90 = side: neither
        const bool hf = std::abs(heard) < 90, hb = std::abs(heard) > 90;
        if ((pf && hb) || (pb && hf)) ++r.frontBack;
    }
    for (std::size_t h = 0; h < heads_.size(); ++h)
        res[h].meanErrorDeg = res[h].answered ? sum[h] / res[h].answered : 180.0;
    std::stable_sort(res.begin(), res.end(), [](const Result& a, const Result& b) {
        const double sa = a.meanErrorDeg + 10.0 * a.frontBack / std::max(1, a.answered) * 8.0;
        const double sb = b.meanErrorDeg + 10.0 * b.frontBack / std::max(1, b.answered) * 8.0;
        return sa < sb;
    });
    return res;
}

}  // namespace tac

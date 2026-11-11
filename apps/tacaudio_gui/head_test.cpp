#include "head_test.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>

#include "core/footstep_synth.h"
#include "core/hrtf.h"
#include "platform/wasapi.h"

namespace tac {

namespace {
constexpr double kRate = 48000.0;

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
    // Game-style footsteps (3 steps at walking pace): a different walk for each direction, the same
    // walks for every head so the heads are compared fairly.
    std::vector<std::vector<float>> walk(kDirs);
    for (int d = 0; d < kDirs; ++d) walk[static_cast<std::size_t>(d)] = synthFootsteps(kRate, 3, 0.42, rng(), 0.5f);

    // Every head x every direction, in random order (so no head is always heard first).
    trials_.clear();
    for (int h = 0; h < static_cast<int>(heads_.size()); ++h)
        for (int d = 0; d < kDirs; ++d) trials_.push_back({h, d, -1});
    std::shuffle(trials_.begin(), trials_.end(), rng);

    std::vector<std::vector<std::vector<float>>> rendered(heads_.size());  // [head][dir] interleaved
    std::vector<std::pair<std::vector<std::vector<float>>, std::vector<std::vector<float>>>> raw(heads_.size());
    std::vector<float> gains(heads_.size(), 1.0f);
    float peakAll = 0.0f;
    for (std::size_t h = 0; h < heads_.size(); ++h) {
        cli::Args a;
        a.opts["--head"] = heads_[h].name;
        const HrirSet set = cli::loadHrirs(a, kRate);
        std::vector<std::vector<float>> l(kDirs), r(kDirs);
        for (int d = 0; d < kDirs; ++d) {
            const auto& m = set.nearest(kAzimuth[d], 0.0f);
            l[static_cast<std::size_t>(d)] = convolve(walk[static_cast<std::size_t>(d)], m.left);
            r[static_cast<std::size_t>(d)] = convolve(walk[static_cast<std::size_t>(d)], m.right);
        }
        // Same loudness for every head (averaged over the directions); level differences between
        // directions are kept: they are part of the cue.
        double avg = 0;
        for (int d = 0; d < kDirs; ++d) avg += 0.5 * (rms(l[static_cast<std::size_t>(d)]) + rms(r[static_cast<std::size_t>(d)])) / kDirs;
        gains[h] = avg > 0 ? static_cast<float>(std::pow(10.0, -32.0 / 20.0) / avg) : 1.0f;
        for (int d = 0; d < kDirs; ++d)
            for (const auto* ch : {&l[static_cast<std::size_t>(d)], &r[static_cast<std::size_t>(d)]})
                for (float v : *ch) peakAll = std::max(peakAll, std::abs(v) * gains[h]);
        raw[h] = {std::move(l), std::move(r)};
    }
    // Footsteps are sharp: if any sound would peak above -3 dBFS, turn everything down together.
    const float guard = peakAll > 0.7f ? 0.7f / peakAll : 1.0f;
    for (std::size_t h = 0; h < heads_.size(); ++h) {
        const auto& l = raw[h].first;
        const auto& r = raw[h].second;
        const float g = gains[h] * guard;
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

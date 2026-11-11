#include "apo_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>

#include "cli.h"
#include "core/wav.h"

namespace tac::apo {

void apoLog(const char* fmt, ...);  // tac_apo.cpp

namespace {
constexpr int kWarmBlocks = 8;  // new renderer runs silently first (fills its filters/reverb)
constexpr int kFadeBlocks = 4;  // then crossfades in over ~11 ms

float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

// Heads are shared by every APO instance in the audio engine process (one per game stream).
std::mutex g_headMutex;
std::map<std::string, std::shared_ptr<const HrirSet>> g_heads;
std::once_flag g_dirOnce;

std::shared_ptr<const HrirSet> loadHead(const std::string& name, double rate) {
    {
        std::lock_guard<std::mutex> l(g_headMutex);
        if (auto it = g_heads.find(name); it != g_heads.end()) return it->second;
    }
    cli::Args a;
    a.opts["--head"] = name;
    auto h = std::make_shared<const HrirSet>(cli::loadHrirs(a, rate));
    std::lock_guard<std::mutex> l(g_headMutex);
    return g_heads.emplace(name, h).first->second;
}
}  // namespace

ApoSettings readSettings(const std::filesystem::path& file) {
    ApoSettings s;
    bool dynOn = true, stepsOn = true;
    std::string preset = "medium";
    float steps = 3.0f;
    std::ifstream f(file);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "head") s.head = v;
            else if (k == "room") s.room = v;
            else if (k == "punch") s.punch = std::stof(v);
            else if (k == "volume") s.volumeDb = std::stof(v);
            else if (k == "3d") s.enabled3d = v == "1";
            else if (k == "dynOn") dynOn = v == "1";
            else if (k == "dynPreset") preset = v;
            else if (k == "stepsOn") stepsOn = v == "1";
            else if (k == "stepsLevel") steps = std::stof(v);
            else if (k == "bass") s.bassDb = std::stof(v);
            else if (k == "stepsSmart") s.footstepsSmart = v == "1";
            else if (k == "eq") s.eq = v;
            else if (k == "enabled") s.enabled = v == "1";
        } catch (const std::exception&) {
            // malformed value: keep the default
        }
    }
    s.dynamics = dynOn ? preset : "off";
    s.footstepsDb = stepsOn ? steps : 0.0f;
    return s;
}

ApoEngine::ApoEngine(double sampleRate, std::uint32_t inChannels, std::uint32_t inMask, std::filesystem::path dataDir,
                     std::filesystem::path settingsFile)
    : rate_(sampleRate),
      inCh_(inChannels),
      dataDir_(std::move(dataDir)),
      settingsFile_(std::move(settingsFile)),
      limiter_(sampleRate) {
    std::call_once(g_dirOnce, [this] { cli::setExeDirOverride(dataDir_); });
    const std::uint32_t mask = inMask ? inMask : defaultChannelMask(inChannels);
    layout_ = layoutFromChannelMask(mask);
    if (layout_.size() != inCh_) layout_ = layoutFromChannelMask(defaultChannelMask(inCh_));

    downL_.assign(inCh_, 0.0f);
    downR_.assign(inCh_, 0.0f);
    for (std::size_t c = 0; c < layout_.size() && c < inCh_; ++c) {
        const auto& s = layout_.speakers[c];
        if (s.isLfe) downL_[c] = downR_[c] = 0.5f;
        else if (s.name == "FL") downL_[c] = 1.0f;
        else if (s.name == "FR") downR_[c] = 1.0f;
        else if (std::abs(s.azimuthDeg) < 1.0f || std::abs(std::abs(s.azimuthDeg) - 180.0f) < 1.0f) downL_[c] = downR_[c] = 0.7071f;
        else (s.azimuthDeg < 0 ? downL_[c] : downR_[c]) = 0.7071f;
    }

    inBlock_.assign(inCh_, std::vector<float>(kBlock, 0.0f));
    {
        std::vector<float> az;
        std::vector<bool> lfe;
        for (const auto& s : layout_.speakers) {
            az.push_back(s.azimuthDeg);
            lfe.push_back(s.isLfe);
        }
        az.resize(std::min<std::size_t>(az.size(), inCh_));
        lfe.resize(az.size());
        steps_ = FootstepTracker(sampleRate, az, lfe);
    }
    for (auto& b : inBlock_) inPtrs_.push_back(b.data());
    qL_.assign(2 * kBlock + 1, 0.0f);
    qR_.assign(2 * kBlock + 1, 0.0f);
    qWrite_ = qCount_ = kBlock;  // one block of latency: output is always available
    tmpL_.assign(kBlock, 0.0f);
    tmpR_.assign(kBlock, 0.0f);
    nxtL_.assign(kBlock, 0.0f);
    nxtR_.assign(kBlock, 0.0f);

    const ApoSettings s = readSettings(settingsFile_);
    gain_ = dbToLin(s.volumeDb);
    targetGain_ = gain_;
    target3d_ = s.enabled3d;
    mix3d_ = s.enabled3d ? 1.0f : 0.0f;
    targetOn_ = s.enabled;
    mixOn_ = s.enabled ? 1.0f : 0.0f;
    thread_ = std::thread([this] { worker(); });
}

ApoEngine::~ApoEngine() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    delete cur_;
    delete next_;
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
}

void ApoEngine::worker() {
    using namespace std::chrono;
    ApoSettings have;
    bool built = false;
    std::filesystem::file_time_type seen{};
    auto lastCheck = steady_clock::now() - seconds(1);
    while (!stop_) {
        delete retired_.exchange(nullptr);
        std::this_thread::sleep_for(milliseconds(50));
        if (steady_clock::now() - lastCheck < milliseconds(100)) continue;
        lastCheck = steady_clock::now();

        std::error_code ec;
        const auto t = std::filesystem::last_write_time(settingsFile_, ec);
        if (built && (ec || t == seen)) continue;
        seen = ec ? std::filesystem::file_time_type{} : t;
        const ApoSettings s = readSettings(settingsFile_);
        targetGain_ = dbToLin(s.volumeDb);
        target3d_ = s.enabled3d;
        targetOn_ = s.enabled;
        if (built && !s.needsRebuild(have)) {
            have = s;
            continue;
        }
        try {
            const auto t0 = steady_clock::now();
            auto head = loadHead(s.head, rate_);
            cli::Args a;
            a.opts["--room"] = s.room;
            a.opts["--dynamics"] = s.dynamics;
            a.opts["--punch"] = std::to_string(s.punch);
            a.opts["--footsteps"] = std::to_string(s.footstepsDb);
            a.opts["--footsteps-smart"] = s.footstepsSmart ? "1" : "0";
            a.opts["--eq"] = s.eq;
            a.opts["--lfe-db"] = std::to_string(s.bassDb);
            a.opts["--gain-db"] = "0";
            const BinauralConfig cfg = cli::binauralFrom(a, 0.0f, kBlock, s.room, s.dynamics);
            auto* r = new BinauralRenderer(layout_, *head, rate_, cfg);
            delete pending_.exchange(r);  // an older renderer the audio thread never picked up
            apoLog("engine: head %s, room %s, dynamics %s, punch %.0f, footsteps %+.1f dB, bass %+.1f dB, volume %.1f dB, "
                   "3D %s (%s, ready in %lld ms)",
                   s.head.c_str(), s.room.c_str(), s.dynamics.c_str(), s.punch, s.footstepsDb, s.bassDb, s.volumeDb,
                   s.enabled3d ? "on" : "off", layout_.describe().c_str(),
                   static_cast<long long>(duration_cast<milliseconds>(steady_clock::now() - t0).count()));
            have = s;
            built = true;
        } catch (const std::exception& e) {
            // Keep the current sound (or the plain downmix); the next settings change retries.
            apoLog("engine: cannot build renderer for head '%s': %s", s.head.c_str(), e.what());
            built = true;
            have = s;
        }
    }
}

void ApoEngine::takeFootsteps(float* out8) noexcept {
    for (std::size_t c = 0; c < 8; ++c) out8[c] = stepAct_[c].exchange(0.0f);
}

void ApoEngine::renderBlock() noexcept {
    // Footstep detection on the game's channels (for the app's direction ring).
    steps_.process(inPtrs_.data(), kBlock);
    for (std::size_t c = 0; c < steps_.channels(); ++c) {
        const float a = steps_.takeMax(c);
        float cur = stepAct_[c].load(std::memory_order_relaxed);
        while (a > cur && !stepAct_[c].compare_exchange_weak(cur, a, std::memory_order_relaxed)) {
        }
    }

    // Pick up a new renderer (only once the previous old one has been collected).
    if (!next_ && !retired_.load(std::memory_order_acquire)) {
        if (BinauralRenderer* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            if (!cur_) {
                cur_ = p;
            } else {
                next_ = p;
                swapStep_ = 0;
            }
        }
    }

    if (cur_) {
        cur_->processBlock(inPtrs_.data(), tmpL_.data(), tmpR_.data());
    } else {
        std::fill(tmpL_.begin(), tmpL_.end(), 0.0f);
        std::fill(tmpR_.begin(), tmpR_.end(), 0.0f);
        for (std::uint32_t c = 0; c < inCh_; ++c)
            for (std::size_t i = 0; i < kBlock; ++i) {
                tmpL_[i] += downL_[c] * inBlock_[c][i];
                tmpR_[i] += downR_[c] * inBlock_[c][i];
            }
    }
    if (next_) {
        next_->processBlock(inPtrs_.data(), nxtL_.data(), nxtR_.data());
        ++swapStep_;
        if (swapStep_ > kWarmBlocks) {
            const int f = swapStep_ - kWarmBlocks;  // 1..kFadeBlocks
            for (std::size_t i = 0; i < kBlock; ++i) {
                const float t = (static_cast<float>(f - 1) + static_cast<float>(i + 1) / kBlock) / kFadeBlocks;
                tmpL_[i] += t * (nxtL_[i] - tmpL_[i]);
                tmpR_[i] += t * (nxtR_[i] - tmpR_[i]);
            }
            if (f >= kFadeBlocks) {
                retired_.store(cur_, std::memory_order_release);
                cur_ = next_;
                next_ = nullptr;
            }
        }
    }

    // 3D on/off blend with the plain downmix, and volume (both smoothed, click-free).
    // Power off = the plain downmix at unity gain (as if TacAudio were not there).
    const float tgt3d = target3d_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    const float tgtOn = targetOn_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    const float tgtGain = targetGain_.load(std::memory_order_relaxed);
    const float step = static_cast<float>(1.0 / (0.01 * rate_));
    const float coef = static_cast<float>(1.0 - std::exp(-1.0 / (0.01 * rate_)));
    auto ramp = [step](float& v, float target) {
        if (v != target) v = target > v ? std::min(target, v + step) : std::max(target, v - step);
    };
    for (std::size_t i = 0; i < kBlock; ++i) {
        ramp(mix3d_, tgt3d);
        ramp(mixOn_, tgtOn);
        gain_ += (tgtGain - gain_) * coef;
        float l = tmpL_[i], r = tmpR_[i];
        if ((mix3d_ < 1.0f || mixOn_ < 1.0f) && cur_) {
            float dl = 0.0f, dr = 0.0f;
            for (std::uint32_t c = 0; c < inCh_; ++c) {
                dl += downL_[c] * inBlock_[c][i];
                dr += downR_[c] * inBlock_[c][i];
            }
            l = (mix3d_ * l + (1.0f - mix3d_) * dl) * gain_;
            r = (mix3d_ * r + (1.0f - mix3d_) * dr) * gain_;
            l = mixOn_ * l + (1.0f - mixOn_) * dl;
            r = mixOn_ * r + (1.0f - mixOn_) * dr;
        } else {
            l *= gain_;
            r *= gain_;
        }
        tmpL_[i] = l;
        tmpR_[i] = r;
    }
    limiter_.process(tmpL_.data(), tmpR_.data(), kBlock);

    // Stats for the app (atomics only).
    float pl = 0.0f, pr = 0.0f;
    for (std::size_t i = 0; i < kBlock; ++i) {
        pl = std::max(pl, std::fabs(tmpL_[i]));
        pr = std::max(pr, std::fabs(tmpR_[i]));
    }
    auto raise = [](std::atomic<float>& a, float v) {
        float c = a.load(std::memory_order_relaxed);
        while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {
        }
    };
    raise(peakL_, pl);
    raise(peakR_, pr);
    if (cur_ && ++statBlocks_ >= 8) {
        statBlocks_ = 0;
        float mn = 0.0f, mx = 0.0f;
        cur_->takeDynamicsStats(mn, mx);
        float c = dynMin_.load(std::memory_order_relaxed);
        while (mn < c && !dynMin_.compare_exchange_weak(c, mn, std::memory_order_relaxed)) {
        }
        raise(dynMax_, mx);
    }

    for (std::size_t i = 0; i < kBlock; ++i) {
        qL_[qWrite_] = tmpL_[i];
        qR_[qWrite_] = tmpR_[i];
        qWrite_ = (qWrite_ + 1) % qL_.size();
    }
    qCount_ += kBlock;
}

void ApoEngine::takeStats(float& peakL, float& peakR, float& dynMinDb, float& dynMaxDb) noexcept {
    peakL = peakL_.exchange(0.0f);
    peakR = peakR_.exchange(0.0f);
    dynMinDb = dynMin_.exchange(0.0f);
    dynMaxDb = dynMax_.exchange(0.0f);
}

void ApoEngine::process(const float* in, float* out, std::uint32_t n, std::uint32_t outCh) noexcept {
    std::uint32_t done = 0;
    while (done < n) {
        const std::uint32_t chunk = std::min<std::uint32_t>(n - done, static_cast<std::uint32_t>(kBlock));
        for (std::uint32_t i = 0; i < chunk; ++i) {
            const float* f = in + static_cast<std::size_t>(done + i) * inCh_;
            for (std::uint32_t c = 0; c < inCh_; ++c) inBlock_[c][inPos_] = f[c];
            if (++inPos_ == kBlock) {
                renderBlock();
                inPos_ = 0;
            }
        }
        for (std::uint32_t i = 0; i < chunk && qCount_ > 0; ++i) {
            const float l = qL_[qRead_], r = qR_[qRead_];
            qRead_ = (qRead_ + 1) % qL_.size();
            --qCount_;
            float* o = out + static_cast<std::size_t>(done + i) * outCh;
            if (outCh == 1) {
                o[0] = 0.5f * (l + r);
            } else {
                o[0] = l;
                o[1] = r;
                for (std::uint32_t c = 2; c < outCh; ++c) o[c] = 0.0f;
            }
        }
        done += chunk;
    }
}

}  // namespace tac::apo

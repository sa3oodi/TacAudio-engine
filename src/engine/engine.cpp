#include "engine/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace tac {

namespace {
constexpr double kDriftGainPerFrame = 2e-6;  // 256 frames of excess -> +512 ppm
constexpr double kLevelSmoothingSec = 1.0;
}  // namespace

Engine::Engine(const SpeakerLayout& inputLayout, const HrirSet& hrirs, const EngineConfig& config)
    : cfg_(config),
      layout_(inputLayout),
      B_(config.binaural.blockSize),
      ring_(inputLayout.size(), static_cast<std::size_t>(config.sampleRate)),  // 1 s of headroom
      reader_(inputLayout.size(), config.binaural.blockSize),
      renderer_(std::make_unique<BinauralRenderer>(inputLayout, hrirs, config.sampleRate, config.binaural)),
      limiter_(config.sampleRate, config.limiter),
      planar_(inputLayout.size(), std::vector<float>(config.binaural.blockSize, 0.0f)),
      outL_(config.binaural.blockSize, 0.0f),
      outR_(config.binaural.blockSize, 0.0f),
      altL_(config.binaural.blockSize, 0.0f),
      altR_(config.binaural.blockSize, 0.0f),
      outPos_(config.binaural.blockSize),
      targetFrames_(std::max(config.targetLatencyMs, 1.0) * 0.001 * config.sampleRate),
      baseTargetFrames_(targetFrames_),
      blockSeconds_(static_cast<double>(config.binaural.blockSize) / config.sampleRate) {
    for (auto& p : planar_) planarPtrs_.push_back(p.data());
    rendererLatency_.store(renderer_->latencySamples());

    // Stereo downmix for the 3D-off (bypass) position: fronts straight, centre/LFE to both,
    // surrounds to their side at -3 dB.
    downmixL_.assign(layout_.size(), 0.0f);
    downmixR_.assign(layout_.size(), 0.0f);
    for (std::size_t c = 0; c < layout_.size(); ++c) {
        const auto& s = layout_.speakers[c];
        if (s.isLfe) {
            downmixL_[c] = downmixR_[c] = 0.5f;
        } else if (s.name == "FL") {
            downmixL_[c] = 1.0f;
        } else if (s.name == "FR") {
            downmixR_[c] = 1.0f;
        } else if (std::abs(s.azimuthDeg) < 1.0f || std::abs(std::abs(s.azimuthDeg) - 180.0f) < 1.0f) {
            downmixL_[c] = downmixR_[c] = 0.7071f;
        } else {
            (s.azimuthDeg < 0 ? downmixL_[c] : downmixR_[c]) = 0.7071f;
        }
    }
}

Engine::~Engine() {
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete next_;
}

bool Engine::reconfigure(const BinauralConfig& binaural, const HrirSet& hrirs) {
    if (swapping_.load(std::memory_order_acquire)) return false;
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);  // free the previous one here, not on audio
    BinauralConfig c = binaural;
    c.blockSize = B_;  // the engine's block size is fixed
    auto* r = new BinauralRenderer(layout_, hrirs, cfg_.sampleRate, c);  // expensive part, on this thread
    swapping_.store(true, std::memory_order_release);
    pending_.store(r, std::memory_order_release);
    return true;
}

void Engine::pushInput(const float* interleaved, std::size_t frames) noexcept {
    const std::size_t written = ring_.write(interleaved, frames);
    if (written < frames) overruns_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::pushSilence(std::size_t frames) noexcept {
    if (ring_.writeSilence(frames) < frames) overruns_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::renderBlock() noexcept {
    const auto t0 = std::chrono::steady_clock::now();

    // Start once the target is buffered. Any small excess (packet granularity) is drained
    // slowly by the drift controller; dropping it here would leave no jitter margin.
    const double avail = static_cast<double>(ring_.availableRead());
    if (!running_ && avail >= targetFrames_) {
        running_ = true;
        reader_.reset();
        fillAvg_ = avail;
    }

    bool haveInput = false;
    if (running_) {
        const double fill = avail + reader_.bufferedFrames();

        // Far too much backlog (stall, device hiccup): drop it instead of slowly catching up.
        // Margin: 2x target, at least 50 ms, so bursty devices do not trigger it.
        const double margin = std::max(2.0 * targetFrames_, 0.05 * cfg_.sampleRate);
        if (fill > targetFrames_ + margin + static_cast<double>(B_)) {
            ring_.discard(static_cast<std::size_t>(fill - targetFrames_));
            fillAvg_ = targetFrames_;
            resyncs_.fetch_add(1, std::memory_order_relaxed);
        } else {
            const double alpha = std::min(1.0, blockSeconds_ / kLevelSmoothingSec);
            fillAvg_ += (fill - fillAvg_) * alpha;
        }

        if (cfg_.driftCompensation) {
            const double maxDev = cfg_.maxDriftPpm * 1e-6;
            ratio_ = 1.0 + std::clamp(kDriftGainPerFrame * (fillAvg_ - targetFrames_), -maxDev, maxDev);
        } else {
            ratio_ = 1.0;
        }

        haveInput = reader_.read(ring_, planarPtrs_.data(), B_, ratio_);
        if (!haveInput) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            running_ = false;
            reader_.reset();
            // The input is burstier than the buffer allows: give it more room.
            if (cfg_.adaptiveLatency)
                targetFrames_ = std::min(targetFrames_ + cfg_.growStepMs * 0.001 * cfg_.sampleRate,
                                         std::max(baseTargetFrames_, cfg_.maxLatencyMs * 0.001 * cfg_.sampleRate));
            blocksSinceUnderrun_ = 0;
        }
    }

    // Steady for a while: give latency back, one small step at a time.
    if (cfg_.adaptiveLatency && running_ && targetFrames_ > baseTargetFrames_ &&
        static_cast<double>(++blocksSinceUnderrun_) * blockSeconds_ >= cfg_.shrinkAfterSec) {
        targetFrames_ = std::max(baseTargetFrames_, targetFrames_ - cfg_.shrinkStepMs * 0.001 * cfg_.sampleRate);
        blocksSinceUnderrun_ = 0;
    }
    if (!haveInput)
        for (auto& p : planar_) std::fill(p.begin(), p.end(), 0.0f);
    for (std::size_t c = 0; c < planar_.size() && c < kMaxMeterChannels; ++c) {  // input meters (per speaker)
        float pk = 0.0f;
        for (float x : planar_[c]) pk = std::max(pk, std::fabs(x));
        float cur = inPeak_[c].load(std::memory_order_relaxed);
        while (pk > cur && !inPeak_[c].compare_exchange_weak(cur, pk, std::memory_order_relaxed)) {
        }
    }

    // Always render: HRIR tails decay naturally across underruns.
    renderer_->processBlock(planarPtrs_.data(), outL_.data(), outR_.data());

    // Live swap: warm the new renderer up silently, then crossfade, then retire the old one.
    if (!next_) {
        if (BinauralRenderer* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            next_ = p;
            swapPhase_ = 0;
        }
    }
    if (next_) {
        next_->processBlock(planarPtrs_.data(), altL_.data(), altR_.data());
        ++swapPhase_;
        if (swapPhase_ > kWarmBlocks) {
            const int k = swapPhase_ - kWarmBlocks;  // 1..kFadeBlocks
            const float total = static_cast<float>(kFadeBlocks * B_);
            for (std::size_t n = 0; n < B_; ++n) {
                const float w = (static_cast<float>((k - 1) * B_ + n) + 1.0f) / total;
                outL_[n] += (altL_[n] - outL_[n]) * w;
                outR_[n] += (altR_[n] - outR_[n]) * w;
            }
            if (k == kFadeBlocks) {
                retired_.store(renderer_.release(), std::memory_order_release);  // freed by the control thread
                renderer_.reset(next_);
                next_ = nullptr;
                rendererLatency_.store(renderer_->latencySamples());
                swapping_.store(false, std::memory_order_release);
            }
        }
    }

    // 3D off (bypass): crossfade to a plain stereo downmix over ~4 blocks.
    const float mixTarget = bypass_.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    if (mixTarget > 0.0f || bypassMix_ > 0.0f) {
        const float step = 1.0f / static_cast<float>(4 * B_);
        for (std::size_t n = 0; n < B_; ++n) {
            bypassMix_ = mixTarget > bypassMix_ ? std::min(mixTarget, bypassMix_ + step) : std::max(mixTarget, bypassMix_ - step);
            float dl = 0, dr = 0;
            for (std::size_t c = 0; c < planar_.size(); ++c) {
                dl += downmixL_[c] * planar_[c][n];
                dr += downmixR_[c] * planar_[c][n];
            }
            outL_[n] += (dl - outL_[n]) * bypassMix_;
            outR_[n] += (dr - outR_[n]) * bypassMix_;
        }
    }

    // Output volume, ramped across the block.
    const float gTarget = outGainTarget_.load(std::memory_order_relaxed);
    if (gTarget != outGain_ || outGain_ != 1.0f) {
        const float g0 = outGain_, dg = (gTarget - g0) / static_cast<float>(B_);
        for (std::size_t n = 0; n < B_; ++n) {
            const float g = g0 + dg * static_cast<float>(n + 1);
            outL_[n] *= g;
            outR_[n] *= g;
        }
        outGain_ = gTarget;
    }

    limiter_.process(outL_.data(), outR_.data(), B_);
    outPos_ = 0;
    float pl = 0, pr = 0;
    for (std::size_t n = 0; n < B_; ++n) {
        pl = std::max(pl, std::abs(outL_[n]));
        pr = std::max(pr, std::abs(outR_[n]));
    }
    if (pl > peakL_.load(std::memory_order_relaxed)) peakL_.store(pl, std::memory_order_relaxed);
    if (pr > peakR_.load(std::memory_order_relaxed)) peakR_.store(pr, std::memory_order_relaxed);

    // Stats.
    const double load = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / blockSeconds_;
    const double prevAvg = cpuAvg_.load(std::memory_order_relaxed);
    cpuAvg_.store(prevAvg + (load - prevAvg) * 0.01, std::memory_order_relaxed);
    if (load > cpuMax_.load(std::memory_order_relaxed)) cpuMax_.store(load, std::memory_order_relaxed);
    const float g = limiter_.takeMinGain();
    if (g < limMin_.load(std::memory_order_relaxed)) limMin_.store(g, std::memory_order_relaxed);
    float dmin = 0, dmax = 0;
    renderer_->takeDynamicsStats(dmin, dmax);
    if (dmin < dynMin_.load(std::memory_order_relaxed)) dynMin_.store(dmin, std::memory_order_relaxed);
    if (dmax > dynMax_.load(std::memory_order_relaxed)) dynMax_.store(dmax, std::memory_order_relaxed);
    bufferMs_.store(fillAvg_ / cfg_.sampleRate * 1000.0, std::memory_order_relaxed);
    targetMs_.store(targetFrames_ / cfg_.sampleRate * 1000.0, std::memory_order_relaxed);
    driftPpm_.store((ratio_ - 1.0) * 1e6, std::memory_order_relaxed);
    runningFlag_.store(running_, std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);
}

void Engine::pullOutput(float* out, std::size_t frames) noexcept {
    while (frames > 0) {
        if (outPos_ >= B_) renderBlock();
        const std::size_t n = std::min(frames, B_ - outPos_);
        for (std::size_t i = 0; i < n; ++i) {
            out[2 * i] = outL_[outPos_ + i];
            out[2 * i + 1] = outR_[outPos_ + i];
        }
        out += 2 * n;
        outPos_ += n;
        frames -= n;
    }
}

EngineStats Engine::takeStats() noexcept {
    EngineStats s;
    s.blocks = blocks_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.resyncs = resyncs_.load(std::memory_order_relaxed);
    s.bufferMs = bufferMs_.load(std::memory_order_relaxed);
    s.targetMs = targetMs_.load(std::memory_order_relaxed);
    s.driftPpm = driftPpm_.load(std::memory_order_relaxed);
    s.cpuLoad = cpuAvg_.load(std::memory_order_relaxed);
    s.cpuLoadMax = cpuMax_.exchange(0.0, std::memory_order_relaxed);
    const float g = limMin_.exchange(1.0f, std::memory_order_relaxed);
    s.limiterMinGainDb = 20.0f * std::log10(std::max(g, 1e-6f));
    s.dynMinGainDb = dynMin_.exchange(0.0f, std::memory_order_relaxed);
    s.dynMaxGainDb = dynMax_.exchange(0.0f, std::memory_order_relaxed);
    s.running = runningFlag_.load(std::memory_order_relaxed);
    return s;
}

double Engine::nominalLatencyMs() const noexcept {
    return (targetFrames_ + static_cast<double>(B_ + limiter_.latency() + rendererLatency_.load())) /
           cfg_.sampleRate * 1000.0;
}

}  // namespace tac

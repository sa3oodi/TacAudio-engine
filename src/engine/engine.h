#pragma once
// Real-time TacAudio engine (platform independent).
//
//   producer thread (capture)  --pushInput()-->  [lock-free ring]
//   consumer thread (render)   <--pullOutput()-- [varispeed] -> [binaural] -> [limiter]
//
// * Starts (and restarts after an underrun) only once the ring holds the target
//   latency, so playback never stutters on startup.
// * Clock drift between the two devices is absorbed by a slowly adapting playback
//   ratio (P-controller on the smoothed buffer level, max +/-0.5 %).
// * A large backlog (e.g. after a stall) is dropped in one go ("resync").
// * Live changes (UI): reconfigure() builds a new renderer off the audio thread; the audio
//   thread runs it alongside the old one for ~20 ms (to fill its filter/reverb history), then
//   crossfades over ~10 ms. Volume and 3D on/off (stereo downmix bypass) ramp instantly.
// pullOutput() is allocation- and lock-free.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/binaural_renderer.h"
#include "core/footstep_detector.h"
#include "core/limiter.h"
#include "core/ring_buffer.h"
#include "core/varispeed.h"

namespace tac {

struct EngineConfig {
    double sampleRate = 48000.0;
    double targetLatencyMs = 20.0;   // input buffering (absorbs device packet jitter)
    bool driftCompensation = true;
    double maxDriftPpm = 5000.0;
    // Adaptive jitter buffer: grow the target after an underrun, shrink it back slowly
    // once input has been steady, never below targetLatencyMs.
    bool adaptiveLatency = true;
    double maxLatencyMs = 60.0;
    double growStepMs = 2.0;
    double shrinkStepMs = 0.5;
    double shrinkAfterSec = 20.0;
    BinauralConfig binaural;         // block size, LFE, master gain
    LimiterConfig limiter;
};

struct EngineStats {
    std::uint64_t blocks = 0;
    std::uint64_t underruns = 0;     // render wanted audio the input did not have yet
    std::uint64_t overruns = 0;      // input arrived faster than it could be stored
    std::uint64_t resyncs = 0;       // backlog dropped to restore the target latency
    double bufferMs = 0;             // smoothed input buffer level
    double targetMs = 0;             // current (adaptive) buffer target
    double driftPpm = 0;             // current playback-ratio correction
    double cpuLoad = 0;              // average block processing time / block duration
    double cpuLoadMax = 0;
    float limiterMinGainDb = 0;      // strongest limiting since the last takeStats()
    float dynMinGainDb = 0;          // strongest dynamics reduction (loud sounds tamed)
    float dynMaxGainDb = 0;          // strongest dynamics boost (quiet sounds lifted)
    bool running = false;
};

class Engine {
public:
    Engine(const SpeakerLayout& inputLayout, const HrirSet& hrirs, const EngineConfig& config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::size_t numInputChannels() const noexcept { return ring_.channels(); }
    const EngineConfig& config() const noexcept { return cfg_; }
    const SpeakerLayout& inputLayout() const noexcept { return layout_; }

    // ---- control thread (UI): live changes
    // Builds a renderer for `binaural` (block size is kept) and queues it for a click-free swap.
    // Returns false (nothing changed) while a previous swap is still in progress.
    bool reconfigure(const BinauralConfig& binaural, const HrirSet& hrirs);
    bool swapInProgress() const noexcept { return swapping_.load(std::memory_order_acquire); }
    void setOutputGainDb(float db) noexcept { outGainTarget_.store(std::pow(10.0f, db / 20.0f)); }
    // true = 3D off: plain stereo downmix of the input (for A/B against the binaural sound).
    void setBypass(bool bypass) noexcept { bypass_.store(bypass); }
    // Output peak per ear since the last call (linear, after the limiter). For level meters.
    void takeMeters(float& left, float& right) noexcept {
        left = peakL_.exchange(0.0f);
        right = peakR_.exchange(0.0f);
    }

    // Input peak per channel (speaker) since the last call (linear), up to kMaxMeterChannels.
    static constexpr std::size_t kMaxMeterChannels = 8;
    void takeInputPeaks(float* out, std::size_t n) noexcept {
        for (std::size_t c = 0; c < n && c < kMaxMeterChannels; ++c) out[c] = inPeak_[c].exchange(0.0f);
    }
    // Footstep activity per channel (0..1, core/footstep_detector.h), strongest since the last call.
    void takeFootsteps(float* out, std::size_t n) noexcept {
        for (std::size_t c = 0; c < n && c < kMaxMeterChannels; ++c) out[c] = stepAct_[c].exchange(0.0f);
    }

    // ---- producer thread
    void pushInput(const float* interleaved, std::size_t frames) noexcept;
    void pushSilence(std::size_t frames) noexcept;

    // ---- consumer thread: writes `frames` interleaved stereo frames
    void pullOutput(float* interleavedStereo, std::size_t frames) noexcept;

    // ---- any thread: snapshot; resets the max/min "since last call" values
    EngineStats takeStats() noexcept;

    // Engine-internal latency (input buffer + block + limiter look-ahead), excluding device buffers.
    double nominalLatencyMs() const noexcept;

private:
    void renderBlock() noexcept;

    EngineConfig cfg_;
    SpeakerLayout layout_;
    std::size_t B_;
    SpscRingBuffer ring_;
    VarispeedReader reader_;
    std::unique_ptr<BinauralRenderer> renderer_;
    Limiter limiter_;

    // live renderer swap
    static constexpr int kWarmBlocks = 8;  // new renderer runs silently first (fills its history)
    static constexpr int kFadeBlocks = 4;  // then crossfade
    std::atomic<BinauralRenderer*> pending_{nullptr};  // control -> audio
    std::atomic<BinauralRenderer*> retired_{nullptr};  // audio -> control (freed there)
    std::atomic<bool> swapping_{false};
    BinauralRenderer* next_ = nullptr;
    int swapPhase_ = 0;
    std::atomic<std::size_t> rendererLatency_{0};

    // output gain + bypass (stereo downmix)
    std::atomic<float> outGainTarget_{1.0f};
    float outGain_ = 1.0f;
    std::atomic<bool> bypass_{false};
    float bypassMix_ = 0.0f;  // 0 = 3D, 1 = stereo downmix
    std::vector<float> downmixL_, downmixR_;  // per input channel

    std::vector<std::vector<float>> planar_;
    std::vector<float*> planarPtrs_;
    std::vector<float> outL_, outR_, altL_, altR_;
    std::size_t outPos_;             // frames of the current output block already delivered

    double targetFrames_;
    double baseTargetFrames_;
    std::uint64_t blocksSinceUnderrun_ = 0;
    double fillAvg_ = 0;
    double ratio_ = 1.0;
    bool running_ = false;
    double blockSeconds_;

    // stats (written by the consumer, except overruns_)
    std::atomic<std::uint64_t> blocks_{0}, underruns_{0}, overruns_{0}, resyncs_{0};
    std::atomic<double> bufferMs_{0}, targetMs_{0}, driftPpm_{0}, cpuAvg_{0}, cpuMax_{0};
    std::atomic<float> limMin_{1.0f}, dynMin_{0.0f}, dynMax_{0.0f}, peakL_{0.0f}, peakR_{0.0f};
    std::atomic<bool> runningFlag_{false};
    std::atomic<float> inPeak_[kMaxMeterChannels] = {};
    std::atomic<float> stepAct_[kMaxMeterChannels] = {};
    FootstepTracker steps_;
};

}  // namespace tac

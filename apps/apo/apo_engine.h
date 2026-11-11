#pragma once
// TacAudio processing inside the APO: the same binaural renderer as the app (head, room, combat
// dynamics, footstep clarity, bass), plus volume, 3D on/off and a peak limiter.
//
// Threads:
//   * audio engine thread -> process(): real time, never allocates, locks or blocks.
//   * worker thread (owned here) -> watches the settings file, loads heads, builds renderers and
//     hands them to the audio thread through an atomic slot; old renderers come back through
//     another slot and are deleted here.
//
// Settings: %ProgramData%\TacAudio\apo.ini, same keys as the app's settings.ini (head, room,
// punch, volume, 3d, dynOn, dynPreset, stepsOn, stepsLevel, bass) plus "enabled". Re-read when it
// changes.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/binaural_renderer.h"
#include "core/footstep_detector.h"
#include "core/layout.h"
#include "core/limiter.h"

namespace tac::apo {

struct ApoSettings {
    std::string head = "kemar";
    std::string room = "balanced";
    std::string dynamics = "medium";  // "off" when combat dynamics are switched off
    float punch = 60.0f;
    float footstepsDb = 3.0f;         // 0 when footstep clarity is off
    bool footstepsSmart = true;       // short impacts only
    std::string eq;                   // headphone EQ (compact form), "" = off
    float bassDb = 0.0f;
    float volumeDb = -6.0f;
    bool enabled3d = true;
    bool enabled = true;              // TacAudio power: off = plain stereo downmix at unity gain

    bool needsRebuild(const ApoSettings& o) const {
        return head != o.head || room != o.room || dynamics != o.dynamics || punch != o.punch ||
               footstepsDb != o.footstepsDb || footstepsSmart != o.footstepsSmart || bassDb != o.bassDb ||
               eq != o.eq;
    }
};

ApoSettings readSettings(const std::filesystem::path& file);

class ApoEngine {
public:
    // dataDir: folder with hrtf/ (MIT KEMAR) and hrtf/sadie2/. inChannels/inMask: APO input.
    ApoEngine(double sampleRate, std::uint32_t inChannels, std::uint32_t inMask, std::filesystem::path dataDir,
              std::filesystem::path settingsFile);
    ~ApoEngine();
    ApoEngine(const ApoEngine&) = delete;
    ApoEngine& operator=(const ApoEngine&) = delete;

    // Real time. in: n interleaved frames of inChannels; out: n interleaved stereo frames.
    void process(const float* in, float* out, std::uint32_t n, std::uint32_t outChannels) noexcept;

    // Since the last call (any thread): output peaks per ear (linear) and the strongest dynamics
    // reduction / lift in dB.
    void takeStats(float& peakL, float& peakR, float& dynMinDb, float& dynMaxDb) noexcept;
    // Footstep activity per input channel (0..1) since the last call; up to 8 channels.
    void takeFootsteps(float* out8) noexcept;

    static constexpr std::size_t kBlock = 128;  // 2.7 ms at 48 kHz (same as the app)

private:
    void worker();
    void renderBlock() noexcept;  // inBlock_ -> one block of stereo into the output queue

    const double rate_;
    const std::uint32_t inCh_;
    SpeakerLayout layout_;
    std::filesystem::path dataDir_, settingsFile_;
    std::vector<float> downL_, downR_;  // 3D-off stereo downmix

    // Audio-thread state.
    std::vector<std::vector<float>> inBlock_;  // planar, kBlock per channel
    std::vector<const float*> inPtrs_;
    std::size_t inPos_ = 0;
    std::vector<float> qL_, qR_;               // output queue (ring), size 2 * kBlock
    std::size_t qRead_ = 0, qWrite_ = 0, qCount_ = 0;
    std::vector<float> tmpL_, tmpR_, nxtL_, nxtR_;
    BinauralRenderer* cur_ = nullptr;           // owned; renderer in use
    BinauralRenderer* next_ = nullptr;          // owned; warming up / fading in
    int swapStep_ = 0;
    float gain_ = 0.0f, mix3d_ = 1.0f;          // smoothed volume (linear) and 3D amount
    float mixOn_ = 1.0f;                        // smoothed power (1 = TacAudio, 0 = plain downmix)
    int statBlocks_ = 0;
    Limiter limiter_;
    std::vector<FootstepDetector> steps_;
    std::atomic<float> stepAct_[8] = {};

    // Hand-over slots between threads.
    std::atomic<BinauralRenderer*> pending_{nullptr};  // worker -> audio
    std::atomic<BinauralRenderer*> retired_{nullptr};  // audio -> worker (to delete)
    std::atomic<float> targetGain_{0.5f};
    std::atomic<bool> target3d_{true};
    std::atomic<bool> targetOn_{true};
    std::atomic<float> peakL_{0.0f}, peakR_{0.0f}, dynMin_{0.0f}, dynMax_{0.0f};

    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace tac::apo

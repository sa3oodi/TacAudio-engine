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
// punch, volume, 3d, dynOn, dynPreset, stepsOn, stepsLevel, bass) plus "enabled" and "stereoApps".
// Re-read when it changes.
//
// Game sound only: Windows runs one instance per app stream and hands every stream over as 7.1,
// stereo apps with only the front left / right channels filled. A stream gets TacAudio's sound
// only while it carries surround content (side, back, centre or LFE); stereo apps (voice chat,
// browser, music) pass through clean with just the headphone EQ, unless "stereoApps" is set.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/binaural_renderer.h"
#include "core/footstep_detector.h"
#include "core/wav.h"
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
    int captureId = 0;                // the app asks for a recording of the game sound by changing this
    bool stereoApps = false;          // also process streams without surround content

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
    // communications: a voice-call stream (always passed through clean).
    ApoEngine(double sampleRate, std::uint32_t inChannels, std::uint32_t inMask, std::filesystem::path dataDir,
              std::filesystem::path settingsFile, bool communications = false);
    ~ApoEngine();
    ApoEngine(const ApoEngine&) = delete;
    ApoEngine& operator=(const ApoEngine&) = delete;

    // Real time. in: n interleaved frames of inChannels; out: n interleaved stereo frames.
    void process(const float* in, float* out, std::uint32_t n, std::uint32_t outChannels) noexcept;

    // Since the last call (any thread): output peaks per ear (linear) and the strongest dynamics
    // reduction / lift in dB.
    void takeStats(float& peakL, float& peakR, float& dynMinDb, float& dynMaxDb) noexcept;
    // Footstep activity per input channel (0..1) since the last call; up to 8 channels.
    void takeFootsteps(float* out8, int level) noexcept;  // level: 0 sensitive, 1 balanced, 2 strict
    void takeStepStats(int* out5) noexcept;               // impacts, own, single, balanced, strict

    static constexpr std::size_t kBlock = 128;  // 2.7 ms at 48 kHz (same as the app)
    static constexpr double kStereoHoldSec = 10.0;  // surround-free time before a stream goes clean
    static constexpr float kAudible = 1e-4f;        // -80 dBFS: a channel "carries sound"

    static constexpr double kIdleSec = 5.0;         // digital silence before a stream stops rendering
    static constexpr float kSilent = 1e-7f;         // below this a block counts as digital silence

    // 1 = this stream gets TacAudio's sound, 0 = passed through clean, -1 = nothing heard yet.
    int mode() const noexcept { return mode_.load(std::memory_order_relaxed); }
    // Whether the last block ran the 3D renderer. It is skipped whenever its sound is not used:
    // clean streams (stereo apps, voice calls), TacAudio or 3D switched off, and streams silent
    // for kIdleSec. The output is the same; it just saves the CPU time.
    bool rendering() const noexcept { return rendering_.load(std::memory_order_relaxed); }

private:
    void worker();
    void renderBlock() noexcept;  // inBlock_ -> one block of stereo into the output queue

    // One renderer plus the headphone EQ alone (the clean sound for stereo apps), swapped together.
    struct Pipeline {
        std::unique_ptr<BinauralRenderer> renderer;
        std::vector<Biquad> eqL, eqR;
        float eqGain = 1.0f;
    };
    void cleanBlock(Pipeline* p, float* l, float* r) noexcept;  // downmix (+ EQ) of inBlock_

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
    std::vector<float> clnL_, clnR_, cnxL_, cnxR_;  // clean sound of cur_ / next_
    Pipeline* cur_ = nullptr;                   // owned; in use
    Pipeline* next_ = nullptr;                  // owned; warming up / fading in
    int swapStep_ = 0;
    float gain_ = 0.0f, mix3d_ = 1.0f;          // smoothed volume (linear) and 3D amount
    float mixOn_ = 1.0f;                        // smoothed power (1 = TacAudio, 0 = plain downmix)
    float mixProc_ = 0.0f;                      // smoothed game-sound amount (1 = TacAudio, 0 = clean)
    std::vector<std::size_t> frontCh_, surroundCh_;  // FL/FR vs every other input channel
    bool decided_ = false;                      // the first audible block sets mixProc_ directly
    bool surround_ = false;                     // surround content seen within the hold time
    std::size_t holdBlocks_ = 0, holdMax_ = 0;
    std::size_t silentBlocks_ = 0, idleMax_ = 0;  // consecutive digitally silent blocks
    bool renderedLast_ = false;                   // the renderers ran in the previous block
    std::atomic<bool> rendering_{false};
    const bool comms_;
    int statBlocks_ = 0;
    Limiter limiter_;
    FootstepTracker steps_;
    std::atomic<float> stepAct_[FootstepTracker::kLevels][8] = {};
    std::atomic<int> stepStat_[5] = {};

    // Hand-over slots between threads.
    std::atomic<Pipeline*> pending_{nullptr};  // worker -> audio
    std::atomic<Pipeline*> retired_{nullptr};  // audio -> worker (to delete)
    std::atomic<bool> stereoApps_{false};
    std::atomic<int> mode_{-1};
    std::atomic<float> targetGain_{0.5f};
    std::atomic<bool> target3d_{true};
    std::atomic<bool> targetOn_{true};
    std::atomic<float> peakL_{0.0f}, peakR_{0.0f}, dynMin_{0.0f}, dynMax_{0.0f};

    std::thread thread_;
    std::atomic<bool> stop_{false};

    // Recording of the game's sound (input) for tuning the footstep detector: the worker allocates
    // the buffer, the audio thread fills it once, the worker writes capture_<n>.wav.
    static constexpr double kCaptureSec = 15.0;
    void writeCapture();
    AudioBuffer capture_;
    std::atomic<bool> capArmed_{false}, capDone_{false};
    std::size_t capPos_ = 0;           // audio thread
    int capId_ = 0;
};

}  // namespace tac::apo

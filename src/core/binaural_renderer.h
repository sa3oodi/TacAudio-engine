#pragma once
// Channel-based binaural renderer: each input channel is a virtual speaker that
// is convolved with the HRIR pair for its direction; LFE is low-passed and fed
// to both ears. This is the "fixed virtual speakers + HRTF" architecture: a moving
// sound only changes how the game spreads it across channels, never the filters,
// so direction stays stable and nothing clicks.

#include <cstddef>
#include <memory>
#include <vector>

#include "core/biquad.h"
#include "core/convolver.h"
#include "core/dynamics.h"
#include "core/hrtf.h"
#include "core/layout.h"
#include "core/room.h"

namespace tac {

struct BinauralConfig {
    std::size_t blockSize = 256;   // 5.3 ms at 48 kHz
    float lfeGainDb = 0.0f;        // LFE level into both ears (0 dB = no bass boost)
    float lfeCutoffHz = 120.0f;
    float masterGainDb = 0.0f;
    RoomParams room;               // default: neutral (dry)
    DynamicsParams dynamics;       // default: off
};

class BinauralRenderer {
public:
    BinauralRenderer(const SpeakerLayout& layout, const HrirSet& hrirs, double sampleRate,
                     const BinauralConfig& config = {});

    std::size_t blockSize() const noexcept { return cfg_.blockSize; }
    std::size_t numInputs() const noexcept { return layout_.size(); }
    std::size_t tailSamples() const noexcept { return tail_; }
    std::size_t latencySamples() const noexcept { return dyn_ ? dyn_->latency() : 0; }
    // Strongest dynamics reduction / boost since the last call (0/0 when dynamics are off).
    void takeDynamicsStats(float& minGainDb, float& maxGainDb) noexcept;
    const SpeakerLayout& layout() const noexcept { return layout_; }

    // in[c]: blockSize samples per input channel; outL/outR: blockSize samples.
    void processBlock(const float* const* in, float* outL, float* outR) noexcept;
    void reset() noexcept;

    // Which measured direction each speaker actually uses (for diagnostics).
    struct SpeakerBinding {
        std::size_t channel;
        float requestedAz, requestedEl, usedAz, usedEl;
    };
    const std::vector<SpeakerBinding>& bindings() const noexcept { return bindings_; }

private:
    SpeakerLayout layout_;
    BinauralConfig cfg_;
    struct Channel {
        std::size_t index;
        std::unique_ptr<PartitionedConvolver> conv;
    };
    std::vector<Channel> spatial_;
    std::vector<std::size_t> lfeChannels_;
    std::vector<Biquad> lfeFilters_;  // 2nd-order x2 = 24 dB/oct
    std::unique_ptr<RoomReverb> room_;
    std::unique_ptr<ChannelDynamics> dyn_;
    std::vector<std::vector<float>> work_;  // dynamics-processed copy of the input block
    std::vector<float*> workPtrs_;
    std::vector<float> sendGainL_, sendGainR_;  // per input channel
    std::vector<float> sendL_, sendR_;
    float lfeGain_, masterGain_;
    std::size_t tail_ = 0;
    std::vector<SpeakerBinding> bindings_;
};

// Offline helper: renders a whole multichannel buffer (including the reverb/HRIR tail).
struct AudioBuffer;
AudioBuffer renderBinaural(const AudioBuffer& input, const SpeakerLayout& layout, const HrirSet& hrirs,
                           const BinauralConfig& config = {});

}  // namespace tac

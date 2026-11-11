#include "core/binaural_renderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include "core/wav.h"

namespace tac {

namespace {
float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
}  // namespace

BinauralRenderer::BinauralRenderer(const SpeakerLayout& layout, const HrirSet& hrirs, double sampleRate,
                                   const BinauralConfig& config)
    : layout_(layout), cfg_(config), lfeGain_(dbToGain(config.lfeGainDb)), masterGain_(dbToGain(config.masterGainDb)) {
    if (std::abs(hrirs.sampleRate - sampleRate) > 0.5)
        throw std::invalid_argument("HRIR sample rate (" + std::to_string(static_cast<int>(hrirs.sampleRate)) +
                                    ") does not match audio sample rate (" +
                                    std::to_string(static_cast<int>(sampleRate)) + ")");

    for (std::size_t c = 0; c < layout_.size(); ++c) {
        const auto& sp = layout_.speakers[c];
        if (sp.isLfe) {
            lfeChannels_.push_back(c);
            continue;
        }
        const auto& m = hrirs.nearest(sp.azimuthDeg, sp.elevationDeg);
        spatial_.push_back({c, std::make_unique<PartitionedConvolver>(
                                   cfg_.blockSize, std::vector<std::vector<float>>{m.left, m.right})});
        bindings_.push_back({c, sp.azimuthDeg, sp.elevationDeg, m.azimuthDeg, m.elevationDeg});
    }
    tail_ = hrirs.irLength;

    for (std::size_t i = 0; i < lfeChannels_.size() * 2; ++i)
        lfeFilters_.push_back(Biquad::lowpass(sampleRate, cfg_.lfeCutoffHz));

    if (cfg_.room.enabled) {
        room_ = std::make_unique<RoomReverb>(sampleRate, cfg_.room);
        // Room send: each speaker panned to the L/R send by its direction (constant power).
        sendGainL_.assign(layout_.size(), 0.0f);
        sendGainR_.assign(layout_.size(), 0.0f);
        for (std::size_t c = 0; c < layout_.size(); ++c) {
            const auto& sp = layout_.speakers[c];
            if (sp.isLfe) continue;
            const double pan = (std::sin(sp.azimuthDeg * std::numbers::pi / 180.0) + 1.0) * 0.25 * std::numbers::pi;
            sendGainL_[c] = static_cast<float>(std::cos(pan));
            sendGainR_[c] = static_cast<float>(std::sin(pan));
        }
        sendL_.assign(cfg_.blockSize, 0.0f);
        sendR_.assign(cfg_.blockSize, 0.0f);
        tail_ += static_cast<std::size_t>((cfg_.room.rt60 + 0.06) * sampleRate);
    }

    if (!cfg_.eq.empty()) {
        eqL_ = cfg_.eq.makeFilters(sampleRate);
        eqR_ = eqL_;
        masterGain_ *= dbToGain(cfg_.eq.preampDb);
    }

    if (cfg_.dynamics.active()) {
        dyn_ = std::make_unique<ChannelDynamics>(sampleRate, layout_.size(), cfg_.dynamics);
        work_.assign(layout_.size(), std::vector<float>(cfg_.blockSize, 0.0f));
        for (auto& w : work_) workPtrs_.push_back(w.data());
        tail_ += dyn_->latency();
    }
}

void BinauralRenderer::reset() noexcept {
    for (auto& ch : spatial_) ch.conv->reset();
    for (auto& f : lfeFilters_) f.reset();
    if (room_) room_->reset();
    if (dyn_) dyn_->reset();
    for (auto& f : eqL_) f.reset();
    for (auto& f : eqR_) f.reset();
}

void BinauralRenderer::takeDynamicsStats(float& minGainDb, float& maxGainDb) noexcept {
    minGainDb = dyn_ ? dyn_->takeMinGainDb() : 0.0f;
    maxGainDb = dyn_ ? dyn_->takeMaxGainDb() : 0.0f;
}

void BinauralRenderer::processBlock(const float* const* input, float* outL, float* outR) noexcept {
    const std::size_t B = cfg_.blockSize;
    // Dynamics run per channel on a copy, before anything spatial.
    const float* const* in = input;
    if (dyn_) {
        for (std::size_t c = 0; c < work_.size(); ++c) std::copy(input[c], input[c] + B, work_[c].begin());
        dyn_->process(workPtrs_.data(), B);
        in = workPtrs_.data();  // float** -> const float* const* (qualification conversion)
    }
    std::fill(outL, outL + B, 0.0f);
    std::fill(outR, outR + B, 0.0f);

    float* outs[2] = {outL, outR};
    for (auto& ch : spatial_) ch.conv->process(in[ch.index], outs, /*accumulate=*/true);

    for (std::size_t i = 0; i < lfeChannels_.size(); ++i) {
        const float* x = in[lfeChannels_[i]];
        Biquad& f1 = lfeFilters_[2 * i];
        Biquad& f2 = lfeFilters_[2 * i + 1];
        for (std::size_t n = 0; n < B; ++n) {
            const float y = f2.process(f1.process(x[n])) * lfeGain_;
            outL[n] += y;
            outR[n] += y;
        }
    }

    if (room_) {
        std::fill(sendL_.begin(), sendL_.end(), 0.0f);
        std::fill(sendR_.begin(), sendR_.end(), 0.0f);
        for (const auto& ch : spatial_) {
            const float gl = sendGainL_[ch.index], gr = sendGainR_[ch.index];
            const float* x = in[ch.index];
            for (std::size_t n = 0; n < B; ++n) {
                sendL_[n] += gl * x[n];
                sendR_[n] += gr * x[n];
            }
        }
        room_->process(sendL_.data(), sendR_.data(), outL, outR, B);
    }

    if (masterGain_ != 1.0f) {
        for (std::size_t n = 0; n < B; ++n) {
            outL[n] *= masterGain_;
            outR[n] *= masterGain_;
        }
    }
    for (auto& f : eqL_)
        for (std::size_t n = 0; n < B; ++n) outL[n] = f.process(outL[n]);
    for (auto& f : eqR_)
        for (std::size_t n = 0; n < B; ++n) outR[n] = f.process(outR[n]);
}

AudioBuffer renderBinaural(const AudioBuffer& input, const SpeakerLayout& layout, const HrirSet& hrirs,
                           const BinauralConfig& config) {
    if (input.numChannels() != layout.size())
        throw std::invalid_argument("input has " + std::to_string(input.numChannels()) + " channels, layout has " +
                                    std::to_string(layout.size()));

    BinauralRenderer r(layout, hrirs, input.sampleRate, config);
    const std::size_t B = r.blockSize();
    const std::size_t totalFrames = input.numFrames() + r.tailSamples();
    const std::size_t blocks = (totalFrames + B - 1) / B;

    AudioBuffer out;
    out.sampleRate = input.sampleRate;
    out.channelMask = 0x3;
    out.resize(2, blocks * B);

    std::vector<std::vector<float>> blockIn(input.numChannels(), std::vector<float>(B));
    std::vector<const float*> ptrs(input.numChannels());
    for (std::size_t c = 0; c < input.numChannels(); ++c) ptrs[c] = blockIn[c].data();

    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t start = b * B;
        for (std::size_t c = 0; c < input.numChannels(); ++c) {
            const auto& src = input.channels[c];
            for (std::size_t n = 0; n < B; ++n) {
                const std::size_t idx = start + n;
                blockIn[c][n] = idx < src.size() ? src[idx] : 0.0f;
            }
        }
        r.processBlock(ptrs.data(), out.channels[0].data() + start, out.channels[1].data() + start);
    }
    for (auto& ch : out.channels) ch.resize(totalFrames);
    return out;
}

}  // namespace tac

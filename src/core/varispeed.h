#pragma once
// Variable-rate reader for clock-drift compensation.
//
// Pulls interleaved frames from a ring buffer and produces planar blocks at a
// playback ratio close to 1.0 (e.g. 1.0001 = consume 100 ppm faster) using
// 4-point cubic Hermite interpolation. At ratio == 1 it is an exact pass-through.

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "core/ring_buffer.h"

namespace tac {

class VarispeedReader {
public:
    VarispeedReader(std::size_t channels, std::size_t maxBlockFrames)
        : ch_(channels), buf_((2 * maxBlockFrames + 16) * channels, 0.0f) {
        reset();
    }

    // Starts fresh: one frame of silent history, read position on the first real frame.
    void reset() noexcept {
        std::memset(buf_.data(), 0, ch_ * sizeof(float));
        count_ = 1;
        pos_ = 1.0;
    }

    // Input frames pulled from the ring but not yet consumed.
    double bufferedFrames() const noexcept { return static_cast<double>(count_) - pos_; }

    // Writes `frames` samples per channel to out[c]. Returns false (and writes nothing)
    // if the ring does not hold enough input yet.
    bool read(SpscRingBuffer& ring, float* const* out, std::size_t frames, double ratio) noexcept {
        const auto lastIdx = static_cast<std::size_t>(pos_ + static_cast<double>(frames - 1) * ratio);
        const std::size_t need = lastIdx + 3;  // Hermite needs idx-1 .. idx+2
        if (need * ch_ > buf_.size()) return false;
        if (count_ < need) {
            count_ += ring.read(&buf_[count_ * ch_], need - count_);
            if (count_ < need) return false;
        }

        double p = pos_;
        for (std::size_t i = 0; i < frames; ++i, p += ratio) {
            const auto idx = static_cast<std::size_t>(p);
            const float t = static_cast<float>(p - static_cast<double>(idx));
            const float* xm1 = &buf_[(idx - 1) * ch_];
            const float* x0 = xm1 + ch_;
            const float* x1 = x0 + ch_;
            const float* x2 = x1 + ch_;
            if (t == 0.0f) {
                for (std::size_t c = 0; c < ch_; ++c) out[c][i] = x0[c];
                continue;
            }
            for (std::size_t c = 0; c < ch_; ++c) {
                const float c1 = 0.5f * (x1[c] - xm1[c]);
                const float c2 = xm1[c] - 2.5f * x0[c] + 2.0f * x1[c] - 0.5f * x2[c];
                const float c3 = 0.5f * (x2[c] - xm1[c]) + 1.5f * (x0[c] - x1[c]);
                out[c][i] = ((c3 * t + c2) * t + c1) * t + x0[c];
            }
        }

        // Keep one frame of history before the new read position.
        const std::size_t keepFrom = static_cast<std::size_t>(p) - 1;
        std::memmove(buf_.data(), &buf_[keepFrom * ch_], (count_ - keepFrom) * ch_ * sizeof(float));
        count_ -= keepFrom;
        pos_ = p - static_cast<double>(keepFrom);
        return true;
    }

private:
    std::size_t ch_;
    std::vector<float> buf_;
    std::size_t count_ = 1;
    double pos_ = 1.0;
};

}  // namespace tac

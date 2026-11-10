#include "core/convolver.h"

#include <algorithm>
#include <stdexcept>

namespace tac {

PartitionedConvolver::PartitionedConvolver(std::size_t blockSize,
                                           const std::vector<std::vector<float>>& impulseResponses)
    : B_(blockSize), N_(2 * blockSize), bins_(blockSize + 1), P_(0), irLength_(0), fft_(2 * blockSize) {
    if (blockSize == 0 || !isPowerOfTwo(blockSize)) throw std::invalid_argument("blockSize must be a power of two");
    if (impulseResponses.empty()) throw std::invalid_argument("at least one impulse response required");

    for (const auto& ir : impulseResponses) irLength_ = std::max(irLength_, ir.size());
    if (irLength_ == 0) irLength_ = 1;
    P_ = (irLength_ + B_ - 1) / B_;

    scratch_.resize(N_);
    std::vector<float> segment(N_);
    irSpectra_.resize(impulseResponses.size());
    for (std::size_t m = 0; m < impulseResponses.size(); ++m) {
        const auto& ir = impulseResponses[m];
        auto& spec = irSpectra_[m];
        spec.assign(P_ * bins_, cpx{});
        for (std::size_t p = 0; p < P_; ++p) {
            std::fill(segment.begin(), segment.end(), 0.0f);
            for (std::size_t i = 0; i < B_; ++i) {
                const std::size_t idx = p * B_ + i;
                if (idx < ir.size()) segment[i] = ir[idx];
            }
            fft_.forwardReal(segment.data(), &spec[p * bins_], scratch_.data());
        }
    }

    fdl_.assign(P_ * bins_, cpx{});
    inputBuf_.assign(N_, 0.0f);
    acc_.assign(bins_, cpx{});
    timeOut_.assign(N_, 0.0f);
}

void PartitionedConvolver::reset() noexcept {
    std::fill(fdl_.begin(), fdl_.end(), cpx{});
    std::fill(inputBuf_.begin(), inputBuf_.end(), 0.0f);
    fdlPos_ = 0;
}

void PartitionedConvolver::process(const float* input, float* const* outputs, bool accumulate) noexcept {
    // Slide input window: [old current | new block]
    std::copy(inputBuf_.begin() + static_cast<std::ptrdiff_t>(B_), inputBuf_.end(), inputBuf_.begin());
    std::copy(input, input + B_, inputBuf_.begin() + static_cast<std::ptrdiff_t>(B_));

    fdlPos_ = (fdlPos_ + 1) % P_;
    fft_.forwardReal(inputBuf_.data(), &fdl_[fdlPos_ * bins_], scratch_.data());

    for (std::size_t m = 0; m < irSpectra_.size(); ++m) {
        std::fill(acc_.begin(), acc_.end(), cpx{});
        const cpx* H = irSpectra_[m].data();
        for (std::size_t p = 0; p < P_; ++p) {
            const std::size_t slot = (fdlPos_ + P_ - p) % P_;
            const cpx* X = &fdl_[slot * bins_];
            const cpx* Hp = H + p * bins_;
            for (std::size_t k = 0; k < bins_; ++k) acc_[k] += X[k] * Hp[k];
        }
        fft_.inverseReal(acc_.data(), timeOut_.data(), scratch_.data());

        // Overlap-save: the last B samples are the valid (non-aliased) output.
        float* out = outputs[m];
        const float* valid = timeOut_.data() + B_;
        if (accumulate) {
            for (std::size_t i = 0; i < B_; ++i) out[i] += valid[i];
        } else {
            std::copy(valid, valid + B_, out);
        }
    }
}

}  // namespace tac

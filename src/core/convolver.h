#pragma once
// Uniformly-partitioned overlap-save convolver (UPOLS).
//
// One input stream, M output streams, each with its own impulse response.
// The input spectrum is computed once per block and reused for every output,
// which is exactly what an HRTF stage needs (1 virtual speaker -> 2 ears).
//
//   block size B, FFT size 2B, partitions P = ceil(irLength / B)
//   no added latency: process() returns the output block for the input block.
//
// process() is allocation-free and safe to call from a real-time thread.

#include <cstddef>
#include <vector>

#include "core/fft.h"

namespace tac {

class PartitionedConvolver {
public:
    PartitionedConvolver(std::size_t blockSize, const std::vector<std::vector<float>>& impulseResponses);

    std::size_t blockSize() const noexcept { return B_; }
    std::size_t numOutputs() const noexcept { return irSpectra_.size(); }
    std::size_t numPartitions() const noexcept { return P_; }
    std::size_t irLength() const noexcept { return irLength_; }

    // input: blockSize samples. outputs[m]: blockSize samples.
    // accumulate=false overwrites outputs, true adds into them.
    void process(const float* input, float* const* outputs, bool accumulate = false) noexcept;

    void reset() noexcept;

private:
    std::size_t B_, N_, bins_, P_, irLength_;
    Fft fft_;
    std::vector<std::vector<cpx>> irSpectra_;  // [output][partition * bins + k]
    std::vector<cpx> fdl_;                     // frequency-domain delay line [partition * bins + k]
    std::size_t fdlPos_ = 0;                   // slot holding the newest input spectrum
    std::vector<float> inputBuf_;              // 2B samples: previous block | current block
    std::vector<cpx> acc_;                     // bins
    std::vector<cpx> scratch_;                 // N
    std::vector<float> timeOut_;               // N
};

}  // namespace tac

#pragma once
// Radix-2 FFT. Dependency-free reference implementation; can be swapped for
// pffft/KissFFT later behind the same interface without touching callers.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tac {

using cpx = std::complex<float>;

bool isPowerOfTwo(std::size_t n) noexcept;
std::size_t nextPowerOfTwo(std::size_t n) noexcept;

class Fft {
public:
    explicit Fft(std::size_t size);  // size must be a power of two >= 2

    std::size_t size() const noexcept { return n_; }
    std::size_t halfBins() const noexcept { return n_ / 2 + 1; }

    // In-place complex transforms. forward: no scaling. inverse: scaled by 1/N.
    void forward(cpx* data) const noexcept;
    void inverse(cpx* data) const noexcept;

    // Real signal (N samples) -> half spectrum (N/2+1 bins).
    // `scratch` must hold N complex values. No allocation.
    void forwardReal(const float* in, cpx* outHalf, cpx* scratch) const noexcept;
    // Half spectrum (N/2+1 bins, Hermitian assumed) -> real signal (N samples), scaled 1/N.
    void inverseReal(const cpx* inHalf, float* out, cpx* scratch) const noexcept;

private:
    void transform(cpx* data, bool inverse) const noexcept;

    std::size_t n_;
    std::vector<cpx> twiddles_;          // exp(-2*pi*i*k/N), k < N/2
    std::vector<std::uint32_t> bitrev_;  // bit-reversal permutation
};

}  // namespace tac

#include "core/fft.h"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace tac {

bool isPowerOfTwo(std::size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

std::size_t nextPowerOfTwo(std::size_t n) noexcept {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

Fft::Fft(std::size_t size) : n_(size) {
    if (size < 2 || !isPowerOfTwo(size)) throw std::invalid_argument("Fft size must be a power of two >= 2");

    twiddles_.resize(n_ / 2);
    for (std::size_t k = 0; k < n_ / 2; ++k) {
        const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n_);
        twiddles_[k] = cpx(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }

    unsigned bits = 0;
    while ((std::size_t{1} << bits) < n_) ++bits;
    bitrev_.resize(n_);
    for (std::size_t i = 0; i < n_; ++i) {
        std::uint32_t r = 0;
        for (unsigned b = 0; b < bits; ++b)
            if (i & (std::size_t{1} << b)) r |= 1u << (bits - 1 - b);
        bitrev_[i] = r;
    }
}

void Fft::transform(cpx* data, bool inverse) const noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
        const std::size_t j = bitrev_[i];
        if (j > i) std::swap(data[i], data[j]);
    }
    for (std::size_t len = 2; len <= n_; len <<= 1) {
        const std::size_t half = len / 2;
        const std::size_t step = n_ / len;
        for (std::size_t start = 0; start < n_; start += len) {
            for (std::size_t j = 0; j < half; ++j) {
                cpx w = twiddles_[j * step];
                if (inverse) w = std::conj(w);
                const cpx t = w * data[start + j + half];
                const cpx u = data[start + j];
                data[start + j] = u + t;
                data[start + j + half] = u - t;
            }
        }
    }
}

void Fft::forward(cpx* data) const noexcept { transform(data, false); }

void Fft::inverse(cpx* data) const noexcept {
    transform(data, true);
    const float s = 1.0f / static_cast<float>(n_);
    for (std::size_t i = 0; i < n_; ++i) data[i] *= s;
}

void Fft::forwardReal(const float* in, cpx* outHalf, cpx* scratch) const noexcept {
    for (std::size_t i = 0; i < n_; ++i) scratch[i] = cpx(in[i], 0.0f);
    transform(scratch, false);
    for (std::size_t k = 0; k <= n_ / 2; ++k) outHalf[k] = scratch[k];
}

void Fft::inverseReal(const cpx* inHalf, float* out, cpx* scratch) const noexcept {
    scratch[0] = cpx(inHalf[0].real(), 0.0f);
    scratch[n_ / 2] = cpx(inHalf[n_ / 2].real(), 0.0f);
    for (std::size_t k = 1; k < n_ / 2; ++k) {
        scratch[k] = inHalf[k];
        scratch[n_ - k] = std::conj(inHalf[k]);
    }
    transform(scratch, true);
    const float s = 1.0f / static_cast<float>(n_);
    for (std::size_t i = 0; i < n_; ++i) out[i] = scratch[i].real() * s;
}

}  // namespace tac

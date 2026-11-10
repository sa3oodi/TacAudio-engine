#pragma once
// Single-producer / single-consumer lock-free ring buffer of interleaved float frames.
// write() is called only by the producer thread (audio capture), read()/discard()
// only by the consumer thread (audio render). No locks, no allocation after construction.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

#include "core/fft.h"  // nextPowerOfTwo

namespace tac {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas(64): intended, keeps the indices on separate cache lines
#endif

class SpscRingBuffer {
public:
    SpscRingBuffer(std::size_t channels, std::size_t minCapacityFrames)
        : ch_(channels), cap_(nextPowerOfTwo(std::max<std::size_t>(minCapacityFrames, 2))), mask_(cap_ - 1),
          data_(cap_ * channels, 0.0f) {}

    std::size_t channels() const noexcept { return ch_; }
    std::size_t capacityFrames() const noexcept { return cap_; }

    // Consumer side.
    std::size_t availableRead() const noexcept {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
    }
    // Producer side.
    std::size_t availableWrite() const noexcept {
        return cap_ - (write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire));
    }

    // Producer: copies up to `frames`; returns frames written (less if full).
    std::size_t write(const float* interleaved, std::size_t frames) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t n = std::min(frames, availableWrite());
        copyIn(w, interleaved, n);
        write_.store(w + n, std::memory_order_release);
        return n;
    }
    // Producer: writes zeros.
    std::size_t writeSilence(std::size_t frames) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t n = std::min(frames, availableWrite());
        for (std::size_t i = 0; i < n; ++i) std::memset(&data_[((w + i) & mask_) * ch_], 0, ch_ * sizeof(float));
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer: copies up to `frames`; returns frames read.
    std::size_t read(float* interleaved, std::size_t frames) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t n = std::min(frames, availableRead());
        const std::size_t first = std::min(n, cap_ - (r & mask_));
        std::memcpy(interleaved, &data_[(r & mask_) * ch_], first * ch_ * sizeof(float));
        std::memcpy(interleaved + first * ch_, &data_[0], (n - first) * ch_ * sizeof(float));
        read_.store(r + n, std::memory_order_release);
        return n;
    }
    // Consumer: drops up to `frames` of the oldest audio.
    std::size_t discard(std::size_t frames) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t n = std::min(frames, availableRead());
        read_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    void copyIn(std::size_t w, const float* src, std::size_t n) noexcept {
        const std::size_t first = std::min(n, cap_ - (w & mask_));
        std::memcpy(&data_[(w & mask_) * ch_], src, first * ch_ * sizeof(float));
        std::memcpy(&data_[0], src + first * ch_, (n - first) * ch_ * sizeof(float));
    }

    std::size_t ch_, cap_, mask_;
    std::vector<float> data_;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace tac

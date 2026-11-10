#pragma once
// Minimal multichannel WAV reader/writer.
// Reads PCM 16/24/32-bit, IEEE float 32/64, incl. WAVE_FORMAT_EXTENSIBLE.
// Audio is held planar (one vector per channel) as float in [-1, 1].

#include <cstdint>
#include <filesystem>
#include <vector>

namespace tac {

struct AudioBuffer {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channelMask = 0;  // Windows SPEAKER_* mask, 0 = unknown/default
    std::vector<std::vector<float>> channels;

    std::size_t numChannels() const noexcept { return channels.size(); }
    std::size_t numFrames() const noexcept { return channels.empty() ? 0 : channels[0].size(); }
    void resize(std::size_t numChannels, std::size_t numFrames);
};

enum class WavSampleFormat { Pcm16, Pcm24, Float32 };

AudioBuffer readWav(const std::filesystem::path& path);
void writeWav(const std::filesystem::path& path, const AudioBuffer& audio,
              WavSampleFormat format = WavSampleFormat::Float32);

// Default Windows channel mask for a channel count (0 if no common default).
std::uint32_t defaultChannelMask(std::size_t numChannels) noexcept;

}  // namespace tac

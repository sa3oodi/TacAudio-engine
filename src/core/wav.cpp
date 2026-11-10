#include "core/wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace tac {

namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

// KSDATAFORMAT_SUBTYPE_{PCM,IEEE_FLOAT}: first two bytes carry the format tag, rest is fixed.
constexpr std::uint8_t kSubformatTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                             0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void putTag(std::vector<std::uint8_t>& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

}  // namespace

void AudioBuffer::resize(std::size_t numChannels, std::size_t numFrames) {
    channels.assign(numChannels, std::vector<float>(numFrames, 0.0f));
}

std::uint32_t defaultChannelMask(std::size_t numChannels) noexcept {
    switch (numChannels) {
        case 1: return 0x4;     // FC
        case 2: return 0x3;     // FL FR
        case 4: return 0x33;    // FL FR BL BR
        case 6: return 0x60F;   // FL FR FC LFE SL SR (Windows 5.1)
        case 8: return 0x63F;   // FL FR FC LFE BL BR SL SR (Windows 7.1)
        case 12: return 0x2D63F; // 7.1.4: + TFL TFR TBL TBR
        default: return 0;
    }
}

AudioBuffer readWav(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF/WAVE file: " + path.string());

    std::uint16_t formatTag = 0, numCh = 0, bits = 0, blockAlign = 0;
    std::uint32_t sampleRate = 0, mask = 0;
    const std::uint8_t* pcm = nullptr;
    std::size_t pcmBytes = 0;

    std::size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const std::uint8_t* hdr = data.data() + pos;
        const std::uint32_t size = rd32(hdr + 4);
        const std::size_t body = pos + 8;
        const std::size_t avail = std::min<std::size_t>(size, data.size() - body);
        if (std::memcmp(hdr, "fmt ", 4) == 0 && avail >= 16) {
            const std::uint8_t* p = data.data() + body;
            formatTag = rd16(p);
            numCh = rd16(p + 2);
            sampleRate = rd32(p + 4);
            blockAlign = rd16(p + 12);
            bits = rd16(p + 14);
            if (formatTag == kFormatExtensible && avail >= 40) {
                mask = rd32(p + 20);
                formatTag = rd16(p + 24);  // subformat GUID's leading tag
            }
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            pcm = data.data() + body;
            pcmBytes = avail;
        }
        pos = body + size + (size & 1);  // chunks are word-aligned
    }

    if (!pcm || numCh == 0 || blockAlign == 0) throw std::runtime_error("missing fmt/data chunk: " + path.string());
    const bool isFloat = formatTag == kFormatFloat;
    if (!isFloat && formatTag != kFormatPcm) throw std::runtime_error("unsupported WAV format tag");
    if (isFloat && bits != 32 && bits != 64) throw std::runtime_error("unsupported float bit depth");
    if (!isFloat && bits != 16 && bits != 24 && bits != 32) throw std::runtime_error("unsupported PCM bit depth");

    AudioBuffer out;
    out.sampleRate = sampleRate;
    out.channelMask = mask ? mask : defaultChannelMask(numCh);
    const std::size_t frames = pcmBytes / blockAlign;
    out.resize(numCh, frames);
    const std::size_t bytesPerSample = bits / 8;

    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < numCh; ++c) {
            const std::uint8_t* s = pcm + i * blockAlign + c * bytesPerSample;
            float v = 0.0f;
            if (isFloat && bits == 32) {
                std::memcpy(&v, s, 4);
            } else if (isFloat) {
                double d;
                std::memcpy(&d, s, 8);
                v = static_cast<float>(d);
            } else if (bits == 16) {
                v = static_cast<float>(static_cast<std::int16_t>(rd16(s))) / 32768.0f;
            } else if (bits == 24) {
                std::int32_t x = s[0] | (s[1] << 8) | (s[2] << 16);
                if (x & 0x800000) x |= ~0xFFFFFF;
                v = static_cast<float>(x) / 8388608.0f;
            } else {
                v = static_cast<float>(static_cast<std::int32_t>(rd32(s)) / 2147483648.0);
            }
            out.channels[c][i] = v;
        }
    }
    return out;
}

void writeWav(const std::filesystem::path& path, const AudioBuffer& audio, WavSampleFormat format) {
    const std::size_t numCh = audio.numChannels();
    const std::size_t frames = audio.numFrames();
    if (numCh == 0) throw std::runtime_error("writeWav: no channels");

    const std::uint16_t bits = format == WavSampleFormat::Pcm16 ? 16 : format == WavSampleFormat::Pcm24 ? 24 : 32;
    const std::uint16_t tag = format == WavSampleFormat::Float32 ? kFormatFloat : kFormatPcm;
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(numCh * bits / 8);
    const std::uint32_t mask = audio.channelMask ? audio.channelMask : defaultChannelMask(numCh);
    const bool extensible = numCh > 2 || (mask != 0 && mask != defaultChannelMask(numCh));

    std::vector<std::uint8_t> b;
    const std::size_t dataBytes = frames * blockAlign;
    b.reserve(80 + dataBytes);

    putTag(b, "RIFF");
    put32(b, 0);  // patched below
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    put32(b, extensible ? 40 : 16);
    put16(b, extensible ? kFormatExtensible : tag);
    put16(b, static_cast<std::uint16_t>(numCh));
    put32(b, audio.sampleRate);
    put32(b, audio.sampleRate * blockAlign);
    put16(b, blockAlign);
    put16(b, bits);
    if (extensible) {
        put16(b, 22);
        put16(b, bits);  // valid bits
        put32(b, mask);
        put16(b, tag);
        b.insert(b.end(), std::begin(kSubformatTail), std::end(kSubformatTail));
    }
    putTag(b, "data");
    put32(b, static_cast<std::uint32_t>(dataBytes));

    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < numCh; ++c) {
            const float v = audio.channels[c][i];
            if (format == WavSampleFormat::Float32) {
                std::uint8_t tmp[4];
                std::memcpy(tmp, &v, 4);
                b.insert(b.end(), tmp, tmp + 4);
            } else {
                const float clipped = std::clamp(v, -1.0f, 1.0f);
                if (format == WavSampleFormat::Pcm16) {
                    const auto x = static_cast<std::int16_t>(std::lround(clipped * 32767.0f));
                    put16(b, static_cast<std::uint16_t>(x));
                } else {
                    const auto x = static_cast<std::int32_t>(std::lround(clipped * 8388607.0f));
                    b.push_back(static_cast<std::uint8_t>(x & 0xFF));
                    b.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
                    b.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
                }
            }
        }
    }
    if (dataBytes & 1) b.push_back(0);

    const auto riffSize = static_cast<std::uint32_t>(b.size() - 8);
    for (int i = 0; i < 4; ++i) b[4 + i] = static_cast<std::uint8_t>((riffSize >> (8 * i)) & 0xFF);

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

}  // namespace tac

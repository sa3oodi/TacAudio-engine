#pragma once
// SOFA (AES69, SimpleFreeFieldHRIR) import via libmysofa.
//
// Converts to TacAudio's convention: azimuth 0 = front, +90 = RIGHT, elevation +up.
// (SOFA azimuth is counter-clockwise, +90 = left; the left ear is the receiver with +y.)

#include <cstddef>
#include <filesystem>

#include "core/hrtf.h"

namespace tac {

struct SofaImportOptions {
    double sampleRate = 48000.0;   // resampled to this rate if the file differs
    std::size_t maxTaps = 512;     // after trimming common leading silence
    bool normalizeFront = true;    // frontal source -> ~0 dB gain in 200 Hz..2 kHz
};

struct SofaImportReport {
    double sourceRate = 0;
    std::size_t sourceTaps = 0;
    std::size_t trimmedLeading = 0;
    double frontGainDb = 0;
    bool receiversSwapped = false;
};

HrirSet importSofa(const std::filesystem::path& path, const SofaImportOptions& options = {},
                   SofaImportReport* report = nullptr);

}  // namespace tac

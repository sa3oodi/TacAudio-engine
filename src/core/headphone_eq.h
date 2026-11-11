#pragma once
// Headphone correction EQ: a preamp plus parametric filters applied to both ears after the
// binaural stage. Reads the "Equalizer APO" parametric text format that AutoEq and most
// headphone-measurement sites publish:
//
//   Preamp: -6.2 dB
//   Filter 1: ON PK Fc 105 Hz Gain -2.4 dB Q 0.70
//   Filter 2: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71
//   Filter 3: ON HSC Fc 10000 Hz Gain 2.0 dB Q 0.71
//
// Inside TacAudio an EQ travels as one compact line (settings files, apo.ini):
//   "-6.2|PK:105:-2.4:0.7|LSC:105:5.5:0.71"   (preamp | type:Hz:dB:Q ...), "" = no EQ.

#include <string>
#include <vector>

#include "core/biquad.h"

namespace tac {

struct EqBand {
    enum class Type { Peak, LowShelf, HighShelf, LowPass, HighPass };
    Type type = Type::Peak;
    float freqHz = 1000.0f;
    float gainDb = 0.0f;
    float q = 0.7071f;
};

struct HeadphoneEq {
    float preampDb = 0.0f;
    std::vector<EqBand> bands;

    bool empty() const noexcept { return bands.empty() && preampDb == 0.0f; }
    std::string toCompact() const;
    static HeadphoneEq fromCompact(const std::string& s);  // invalid parts are skipped

    // Equalizer APO / AutoEq text. Throws std::invalid_argument if it holds no filter or preamp.
    static HeadphoneEq parseText(const std::string& text);

    std::vector<Biquad> makeFilters(double sampleRate) const;  // one chain (per ear)
    // Total response at freqHz, including the preamp (for the UI graph and tests).
    float responseDb(double freqHz, double sampleRate = 48000.0) const;
};

// Magnitude (dB) of one biquad at freqHz.
float biquadMagnitudeDb(const Biquad& b, double freqHz, double sampleRate);

}  // namespace tac

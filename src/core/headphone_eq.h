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

    // Volume and bass that suit headphones with this correction, from the app's own defaults:
    //  * volume: the correction (with its preamp) makes most sound quieter or louder; the
    //    suggestion gives back that change, measured as the average level over 200 Hz - 5 kHz
    //    (where game sound and hearing are), so the headphones sound as loud as without EQ.
    //    At most 0 dB (the limiter handles peaks).
    //  * bass: corrections towards the usual target add a bass shelf, and the game's subwoofer
    //    channel goes through it too. Half of that extra bass (30-120 Hz vs 300 Hz-3 kHz) is
    //    taken back off the subwoofer channel, at most 6 dB, so explosions do not cover footsteps.
    //    A correction that removes bass leaves the subwoofer channel as it is.
    // Both rounded to 0.5 dB.
    struct Suggestion {
        float volumeDb = 0.0f, bassDb = 0.0f;
    };
    Suggestion suggest(float defaultVolumeDb, float defaultBassDb) const;
};

// Magnitude (dB) of one biquad at freqHz.
float biquadMagnitudeDb(const Biquad& b, double freqHz, double sampleRate);

}  // namespace tac

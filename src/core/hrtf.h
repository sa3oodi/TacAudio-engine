#pragma once
// HRIR sets (head-related impulse responses).
//
// TacAudio direction convention (used everywhere in the engine):
//   azimuth   degrees, 0 = front, +90 = right, -90 = left, +/-180 = behind
//   elevation degrees, 0 = ear level, +90 = above
//
// File format ".thrir" (little-endian):
//   char[8]  "THRIR01\0"
//   u32      sampleRate
//   u32      numMeasurements
//   u32      irLength
//   repeat numMeasurements:
//     f32 azimuthDeg, f32 elevationDeg, f32 left[irLength], f32 right[irLength]
// Produced by tools/sofa_to_thrir.py from public SOFA files, or by
// HrirSet::makeSphericalHead().

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace tac {

struct HrirMeasurement {
    float azimuthDeg = 0;
    float elevationDeg = 0;
    std::vector<float> left;
    std::vector<float> right;
};

struct SphericalHeadParams {
    double headRadiusM = 0.0875;
    double speedOfSound = 343.0;
    std::size_t irLength = 256;
    double azimuthStepDeg = 5.0;
    double elevationMinDeg = -30.0;
    double elevationMaxDeg = 90.0;
    double elevationStepDeg = 15.0;
    double rearHfCut = 0.5;  // 0..1 strength of the simple front/back cue
};

// Diffuse-field equalisation: one minimum-phase filter, identical for both ears and all
// directions, that makes the direction-averaged response flat. Raw dummy-head data carries
// the head's own ear-canal resonance (e.g. KEMAR: ~+18 dB at 2-3 kHz on average), which
// sounds harsh and thin. Interaural cues are unchanged because both ears get the same filter.
struct DiffuseFieldEqParams {
    double smoothingOctaves = 1.0 / 3.0;  // smooth the average before inverting (no narrow notches/peaks)
    double maxBoostDb = 10.0;
    double maxCutDb = 24.0;
    double lowHoldHz = 150.0;     // below: hold the correction (short HRIRs carry little bass info)
    double highHoldHz = 16000.0;  // above: hold the correction
    std::size_t extraTaps = 512;  // length added to each HRIR by the EQ filter
};

struct DiffuseFieldEqReport {
    double maxBoostDb = 0;
    double maxCutDb = 0;
};

class HrirSet {
public:
    std::string name;
    double sampleRate = 48000.0;
    std::size_t irLength = 0;
    std::vector<HrirMeasurement> measurements;

    // Measurement closest to the requested direction (great-circle distance).
    const HrirMeasurement& nearest(float azimuthDeg, float elevationDeg) const;

    static HrirSet load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;

    // Flattens the direction-averaged (diffuse-field) response; see DiffuseFieldEqParams.
    DiffuseFieldEqReport diffuseFieldEqualize(const DiffuseFieldEqParams& p = {});

    // Direction-averaged power spectrum (both ears, equal-area weighting), `fftSize` bins/2+1.
    std::vector<double> diffuseFieldPower(std::size_t fftSize) const;

    // Built-in analytic fallback: Brown & Duda (1998) spherical-head model
    // (head-shadow filter + Woodworth ITD) with a simple rear high-frequency cut.
    // Good lateralisation, weak front/back and elevation cues. Use measured
    // SOFA data for real listening.
    static HrirSet makeSphericalHead(double sampleRate, const SphericalHeadParams& p = {});
};

// Unit vector for a TacAudio direction: x = right, y = front, z = up.
struct Vec3 {
    double x, y, z;
};
Vec3 directionToVector(double azimuthDeg, double elevationDeg) noexcept;

}  // namespace tac

#pragma once
// RBJ "Audio EQ Cookbook" biquads, transposed direct form II.

#include <cmath>
#include <numbers>

namespace tac {

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    float process(float x) noexcept {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }

    static Biquad lowpass(double fs, double f0, double q = 0.70710678) { return make(Type::LP, fs, f0, q, 0); }
    static Biquad highpass(double fs, double f0, double q = 0.70710678) { return make(Type::HP, fs, f0, q, 0); }
    static Biquad bandpass(double fs, double f0, double q) { return make(Type::BP, fs, f0, q, 0); }
    static Biquad peaking(double fs, double f0, double q, double gainDb) { return make(Type::Peak, fs, f0, q, gainDb); }
    static Biquad lowShelf(double fs, double f0, double gainDb, double q = 0.70710678) {
        return make(Type::LowShelf, fs, f0, q, gainDb);
    }
    static Biquad highShelf(double fs, double f0, double gainDb, double q = 0.70710678) {
        return make(Type::HighShelf, fs, f0, q, gainDb);
    }

private:
    enum class Type { LP, HP, BP, Peak, LowShelf, HighShelf };

    static Biquad make(Type t, double fs, double f0, double q, double gainDb) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * f0 / fs;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (t) {
            case Type::LP:
                b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::HP:
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::BP:  // constant 0 dB peak gain
                b0 = alpha; b1 = 0; b2 = -alpha;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::Peak:
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
                break;
            case Type::LowShelf: {
                const double s = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cw + s);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - s);
                a0 = (A + 1) + (A - 1) * cw + s;
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - s;
                break;
            }
            case Type::HighShelf: {
                const double s = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cw + s);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - s);
                a0 = (A + 1) - (A - 1) * cw + s;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - s;
                break;
            }
        }
        Biquad bq;
        bq.b0 = static_cast<float>(b0 / a0);
        bq.b1 = static_cast<float>(b1 / a0);
        bq.b2 = static_cast<float>(b2 / a0);
        bq.a1 = static_cast<float>(a1 / a0);
        bq.a2 = static_cast<float>(a2 / a0);
        return bq;
    }
};

}  // namespace tac

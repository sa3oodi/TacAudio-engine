#include "core/headphone_eq.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace tac {

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool typeFrom(const std::string& t, EqBand::Type& out) {
    const std::string u = upper(t);
    if (u == "PK" || u == "PEQ" || u == "PEAK") out = EqBand::Type::Peak;
    else if (u == "LS" || u == "LSC" || u == "LSQ" || u == "LOWSHELF") out = EqBand::Type::LowShelf;
    else if (u == "HS" || u == "HSC" || u == "HSQ" || u == "HIGHSHELF") out = EqBand::Type::HighShelf;
    else if (u == "LP" || u == "LPQ" || u == "LOWPASS") out = EqBand::Type::LowPass;
    else if (u == "HP" || u == "HPQ" || u == "HIGHPASS") out = EqBand::Type::HighPass;
    else return false;
    return true;
}

const char* typeName(EqBand::Type t) {
    switch (t) {
        case EqBand::Type::LowShelf: return "LSC";
        case EqBand::Type::HighShelf: return "HSC";
        case EqBand::Type::LowPass: return "LP";
        case EqBand::Type::HighPass: return "HP";
        default: return "PK";
    }
}

bool num(const std::string& s, float& v) {
    try {
        std::size_t used = 0;
        v = std::stof(s, &used);
        return used > 0 && std::isfinite(v);
    } catch (const std::exception&) {
        return false;
    }
}

bool valid(const EqBand& b) {
    return b.freqHz >= 10.0f && b.freqHz <= 22000.0f && b.q > 0.05f && b.q < 30.0f && std::abs(b.gainDb) <= 30.0f;
}
}  // namespace

std::string HeadphoneEq::toCompact() const {
    if (empty()) return {};
    std::string s;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%g", preampDb);
    s = buf;
    for (const auto& b : bands) {
        std::snprintf(buf, sizeof(buf), "|%s:%g:%g:%g", typeName(b.type), b.freqHz, b.gainDb, b.q);
        s += buf;
    }
    return s;
}

HeadphoneEq HeadphoneEq::fromCompact(const std::string& s) {
    HeadphoneEq eq;
    if (s.empty()) return eq;
    std::stringstream ss(s);
    std::string part;
    bool first = true;
    while (std::getline(ss, part, '|')) {
        if (first) {
            first = false;
            float p = 0;
            if (num(part, p)) eq.preampDb = std::clamp(p, -30.0f, 12.0f);
            continue;
        }
        std::stringstream ps(part);
        std::string t, f, g, q;
        std::getline(ps, t, ':');
        std::getline(ps, f, ':');
        std::getline(ps, g, ':');
        std::getline(ps, q, ':');
        EqBand b;
        if (typeFrom(t, b.type) && num(f, b.freqHz) && num(g, b.gainDb) && num(q, b.q) && valid(b)) eq.bands.push_back(b);
    }
    return eq;
}

HeadphoneEq HeadphoneEq::parseText(const std::string& text) {
    HeadphoneEq eq;
    bool any = false;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::stringstream ls(line);
        std::vector<std::string> tok;
        for (std::string t; ls >> t;) tok.push_back(t);
        if (tok.empty()) continue;
        const std::string head = upper(tok[0]);
        if (head == "PREAMP:" && tok.size() >= 2) {
            float p = 0;
            if (num(tok[1], p)) {
                eq.preampDb = std::clamp(p, -30.0f, 12.0f);
                any = true;
            }
            continue;
        }
        if (head.rfind("FILTER", 0) != 0) continue;
        // Filter N: ON <type> Fc <f> Hz Gain <g> dB Q <q>   (Gain / Q optional depending on type)
        std::size_t i = 1;
        while (i < tok.size() && upper(tok[i]) != "ON" && upper(tok[i]) != "OFF") ++i;
        if (i + 1 >= tok.size() || upper(tok[i]) == "OFF") continue;
        EqBand b;
        if (!typeFrom(tok[i + 1], b.type)) continue;
        bool haveF = false, haveQ = false;
        for (std::size_t k = i + 2; k + 1 < tok.size(); ++k) {
            const std::string key = upper(tok[k]);
            if (key == "FC") haveF = num(tok[k + 1], b.freqHz);
            else if (key == "GAIN") num(tok[k + 1], b.gainDb);
            else if (key == "Q") haveQ = num(tok[k + 1], b.q);
        }
        if (!haveQ) b.q = 0.7071f;
        if (haveF && valid(b)) {
            eq.bands.push_back(b);
            any = true;
        }
    }
    if (!any) throw std::invalid_argument("no EQ found - expected Equalizer APO / AutoEq parametric format "
                                          "(\"Preamp: -6 dB\", \"Filter 1: ON PK Fc 100 Hz Gain -2 dB Q 0.7\")");
    return eq;
}

std::vector<Biquad> HeadphoneEq::makeFilters(double fs) const {
    std::vector<Biquad> f;
    const double nyq = fs * 0.5 * 0.98;
    for (const auto& b : bands) {
        const double fc = std::min(static_cast<double>(b.freqHz), nyq);
        switch (b.type) {
            case EqBand::Type::Peak: f.push_back(Biquad::peaking(fs, fc, b.q, b.gainDb)); break;
            case EqBand::Type::LowShelf: f.push_back(Biquad::lowShelf(fs, fc, b.gainDb, b.q)); break;
            case EqBand::Type::HighShelf: f.push_back(Biquad::highShelf(fs, fc, b.gainDb, b.q)); break;
            case EqBand::Type::LowPass: f.push_back(Biquad::lowpass(fs, fc, b.q)); break;
            case EqBand::Type::HighPass: f.push_back(Biquad::highpass(fs, fc, b.q)); break;
        }
    }
    return f;
}

float biquadMagnitudeDb(const Biquad& b, double freqHz, double fs) {
    const double w = 2.0 * std::numbers::pi * freqHz / fs;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    const std::complex<double> h = (static_cast<double>(b.b0) + static_cast<double>(b.b1) * z1 + static_cast<double>(b.b2) * z2) /
                                   (1.0 + static_cast<double>(b.a1) * z1 + static_cast<double>(b.a2) * z2);
    return static_cast<float>(20.0 * std::log10(std::max(1e-12, std::abs(h))));
}

float HeadphoneEq::responseDb(double freqHz, double fs) const {
    float db = preampDb;
    for (const auto& f : makeFilters(fs)) db += biquadMagnitudeDb(f, freqHz, fs);
    return db;
}

HeadphoneEq::Suggestion HeadphoneEq::suggest(float defaultVolumeDb, float defaultBassDb) const {
    Suggestion s{defaultVolumeDb, defaultBassDb};
    if (empty()) return s;
    constexpr double fs = 48000.0;
    const std::vector<Biquad> filters = makeFilters(fs);
    auto avgDb = [&](double lo, double hi) {  // mean of the response on a log-frequency grid
        constexpr int kSteps = 48;
        double sum = 0;
        for (int i = 0; i < kSteps; ++i) {
            const double f = lo * std::pow(hi / lo, (i + 0.5) / kSteps);
            double db = preampDb;
            for (const auto& b : filters) db += biquadMagnitudeDb(b, f, fs);
            sum += db;
        }
        return sum / kSteps;
    };
    auto half = [](double db) { return static_cast<float>(std::round(db * 2.0) / 2.0); };
    const double vol = defaultVolumeDb, bass = defaultBassDb;
    s.volumeDb = half(std::clamp(vol - avgDb(200.0, 5000.0), -30.0, 0.0));
    const double extraBass = avgDb(30.0, 120.0) - avgDb(300.0, 3000.0);
    s.bassDb = half(std::clamp(bass - 0.5 * std::max(0.0, extraBass), bass - 6.0, bass));
    if (s.volumeDb == 0.0f) s.volumeDb = 0.0f;  // no "-0"
    if (s.bassDb == 0.0f) s.bassDb = 0.0f;
    return s;
}

}  // namespace tac

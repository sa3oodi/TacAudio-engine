#include "core/panner.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace tac {

namespace {
constexpr double kDeg = std::numbers::pi / 180.0;

double wrap360(double deg) {
    double d = std::fmod(deg, 360.0);
    return d < 0 ? d + 360.0 : d;
}
}  // namespace

Vbap2D::Vbap2D(const SpeakerLayout& layout) : numChannels_(layout.size()) {
    struct Ring {
        int ch;
        double az;  // 0..360
    };
    std::vector<Ring> ring;
    for (std::size_t i = 0; i < layout.size(); ++i) {
        const auto& s = layout.speakers[i];
        if (s.isLfe || std::abs(s.elevationDeg) > 10.0f) continue;
        ring.push_back({static_cast<int>(i), wrap360(s.azimuthDeg)});
    }
    if (ring.size() < 2) throw std::invalid_argument("Vbap2D needs at least two horizontal speakers");
    std::sort(ring.begin(), ring.end(), [](const Ring& x, const Ring& y) { return x.az < y.az; });

    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Ring& a = ring[i];
        const Ring& b = ring[(i + 1) % ring.size()];
        double span = b.az - a.az;
        if (span <= 0) span += 360.0;
        if (span >= 180.0) continue;  // gap too wide for a stable pair; neighbours cover it

        // Speaker unit vectors in (x=right, y=front).
        const double ax = std::sin(a.az * kDeg), ay = std::cos(a.az * kDeg);
        const double bx = std::sin(b.az * kDeg), by = std::cos(b.az * kDeg);
        const double det = ax * by - bx * ay;
        Pair p{};
        p.a = a.ch;
        p.b = b.ch;
        // L = [[ax, ay], [bx, by]] ; g = s * L^-1
        p.inv[0][0] = by / det;
        p.inv[0][1] = -ay / det;
        p.inv[1][0] = -bx / det;
        p.inv[1][1] = ax / det;
        p.azA = a.az;
        p.span = span;
        pairs_.push_back(p);
    }
    if (pairs_.empty()) throw std::invalid_argument("Vbap2D: no usable speaker pairs");
}

void Vbap2D::gains(float azimuthDeg, float* out) const noexcept {
    std::fill(out, out + numChannels_, 0.0f);
    const double az = wrap360(azimuthDeg);

    const Pair* chosen = nullptr;
    for (const auto& p : pairs_) {
        double off = az - p.azA;
        if (off < 0) off += 360.0;
        if (off <= p.span + 1e-9) {
            chosen = &p;
            break;
        }
    }
    if (!chosen) chosen = &pairs_.front();  // only reachable with a gap >= 180 deg

    const double sx = std::sin(az * kDeg), sy = std::cos(az * kDeg);
    double ga = sx * chosen->inv[0][0] + sy * chosen->inv[1][0];
    double gb = sx * chosen->inv[0][1] + sy * chosen->inv[1][1];
    ga = std::max(ga, 0.0);
    gb = std::max(gb, 0.0);
    const double norm = std::sqrt(ga * ga + gb * gb);
    if (norm <= 0) return;
    out[chosen->a] += static_cast<float>(ga / norm);
    out[chosen->b] += static_cast<float>(gb / norm);
}

std::vector<float> Vbap2D::gains(float azimuthDeg) const {
    std::vector<float> g(numChannels_);
    gains(azimuthDeg, g.data());
    return g;
}

}  // namespace tac

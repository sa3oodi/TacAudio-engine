#include "cable_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <thread>

#include "core/layout.h"
#include "core/wav.h"

namespace tac {

std::optional<wasapi::DeviceInfo> findCable(const std::vector<wasapi::DeviceInfo>& renderDevices) {
    for (const auto& d : renderDevices)
        if (d.name.rfind("CABLE Input", 0) == 0) return d;
    for (const auto& d : renderDevices)
        if (d.name.find("CABLE Input") != std::string::npos) return d;
    return std::nullopt;
}

namespace {
CableTestResult runOnce(const wasapi::DeviceInfo& dev) {
    constexpr std::uint32_t kRate = 48000;
    if (dev.channels < 2) throw std::runtime_error("device reports no channels");
    const std::size_t C = dev.channels;
    const SpeakerLayout layout = layoutFromChannelMask(dev.channelMask ? dev.channelMask : defaultChannelMask(C));

    // Test signal: 0.5 s of silence (stream start-up glitches land there), then channel k plays a
    // 1 kHz burst (LFE: 60 Hz) in slot k.
    const std::size_t slot = kRate * 4 / 10, fade = kRate / 50, lead = kRate / 2;
    const std::size_t total = lead + slot * C + kRate;  // + 1 s tail
    std::vector<float> signal(total * C, 0.0f);
    for (std::size_t k = 0; k < C; ++k) {
        const double f = layout.speakers[k].isLfe ? 60.0 : 1000.0;
        for (std::size_t i = 0; i < slot; ++i) {
            const double env = std::min({1.0, static_cast<double>(i) / fade, static_cast<double>(slot - i) / fade});
            signal[(lead + k * slot + i) * C + k] =
                static_cast<float>(0.25 * env * std::sin(2 * std::numbers::pi * f * i / kRate));
        }
    }

    wasapi::CaptureStream cap;
    const auto cf = cap.open(dev, /*loopback=*/true, kRate);
    if (cf.channels != C) throw std::runtime_error("loopback delivered " + std::to_string(cf.channels) + " channels");
    std::vector<float> captured((total + 2 * kRate) * C, 0.0f);
    std::atomic<std::size_t> capFrames{0};
    cap.start([&](const float* d, std::size_t n, bool silent, std::uint64_t) {
        const std::size_t at = capFrames.load();
        const std::size_t room = captured.size() / C - at;
        n = std::min(n, room);
        if (!silent && d) std::copy(d, d + n * C, captured.begin() + static_cast<std::ptrdiff_t>(at * C));
        capFrames.store(at + n);
    });

    wasapi::RenderStream ren;
    ren.open(dev, kRate, static_cast<std::uint32_t>(C));
    std::atomic<std::size_t> playPos{0};
    ren.start([&](float* out, std::size_t n) {
        const std::size_t p = playPos.load();
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t c = 0; c < C; ++c) out[i * C + c] = p + i < total ? signal[(p + i) * C + c] : 0.0f;
        playPos.store(p + n);
    });

    while (playPos.load() < total + kRate / 2 && !ren.failed() && !cap.failed())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ren.stop();
    cap.stop();
    if (ren.failed()) throw std::runtime_error("render: " + ren.error());
    if (cap.failed()) throw std::runtime_error("capture: " + cap.error());

    CableTestResult res;
    // Each burst is located on its own (a dropped capture packet then cannot shift the later
    // ones): the first sample above -30 dBFS that starts at least 10 ms of loud 1 ms blocks,
    // ignoring the channel the previous burst arrived on (its tail overlaps the search start).
    const std::size_t got = capFrames.load();
    constexpr std::size_t kBlock = kRate / 1000, kSustain = 10;
    constexpr std::size_t kNone = ~std::size_t{0};
    auto loudAt = [&](std::size_t i, std::size_t skip) {
        for (std::size_t c = 0; c < C; ++c)
            if (c != skip && std::abs(captured[i * C + c]) > 0.03f) return true;
        return false;
    };
    auto loudBlock = [&](std::size_t b, std::size_t skip) {
        for (std::size_t i = b * kBlock; i < (b + 1) * kBlock; ++i)
            if (loudAt(i, skip)) return true;
        return false;
    };
    auto findBurst = [&](std::size_t from, std::size_t skip) -> std::optional<std::size_t> {
        for (std::size_t b = from / kBlock, run = 0; (b + 1) * kBlock <= got; ++b) {
            run = loudBlock(b, skip) ? run + 1 : 0;
            if (run == kSustain)
                for (std::size_t i = (b + 1 - kSustain) * kBlock; i < got; ++i)
                    if (loudAt(i, skip)) return i;
        }
        return std::nullopt;
    };

    bool ok = true;
    std::size_t from = 0, prev = kNone;
    for (std::size_t k = 0; k < C; ++k) {
        CableChannelResult r;
        r.sent = layout.speakers[k].name;
        const auto onset = findBurst(from, prev);
        if (!onset) {
            if (k == 0) {
                res.message = "Nothing came back through the cable (is it muted, or is 'Spatial sound' enabled on it?)";
                return res;
            }
            r.received = "-";
            ok = false;
            res.channels.push_back(r);
            continue;
        }
        const std::size_t t0 = *onset > fade / 2 ? *onset - fade / 2 : 0;  // the burst fades in
        from = t0 + slot / 2;
        std::vector<double> e(C, 0.0);
        const std::size_t s = t0 + 2 * fade, end = t0 + slot - 4 * fade;
        for (std::size_t i = s; i < end && i < got; ++i)
            for (std::size_t c = 0; c < C; ++c) e[c] += static_cast<double>(captured[i * C + c]) * captured[i * C + c];
        const std::size_t best = static_cast<std::size_t>(std::max_element(e.begin(), e.end()) - e.begin());
        prev = best;
        double other = 0;
        for (std::size_t c = 0; c < C; ++c)
            if (c != best) other = std::max(other, e[c]);
        const double n = static_cast<double>(end - s);
        r.received = layout.speakers[best].name;
        r.levelDb = 10 * std::log10(std::max(e[best] / n, 1e-12) / (0.25 * 0.25 / 2));  // re. sent RMS
        r.crosstalkDb = 10 * std::log10(std::max(other, 1e-20) / std::max(e[best], 1e-20));
        r.pass = best == k && std::abs(r.levelDb) < 1.0 && r.crosstalkDb < -40.0;
        ok &= r.pass;
        res.channels.push_back(r);
    }
    res.pass = ok;
    res.message = ok ? "All " + std::to_string(C) + " channels arrive separately and unchanged."
                     : "Some channels are missing, swapped or mixed.";
    if (ok && C != 8) {
        res.pass = false;
        res.message = "The cable works, but it is set to " + std::to_string(C) + " channels instead of 7.1.";
    }
    return res;
}
}  // namespace

// A loopback capture occasionally loses a few milliseconds right after starting, which costs the
// first burst ~1 dB. Real problems (stereo cable, swapped or mixed channels) fail every time, so
// a failed run is repeated once.
CableTestResult runCableTest(const wasapi::DeviceInfo& dev) {
    CableTestResult r = runOnce(dev);
    if (!r.pass && !r.channels.empty() && dev.channels == 8) r = runOnce(dev);
    return r;
}

}  // namespace tac

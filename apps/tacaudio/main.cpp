// tacaudio — TacAudio real-time engine (milestone M2).
//
//   tacaudio devices
//   tacaudio run <source> [--out <device|default|null>] [options]
//
// Sources (pick one):
//   --in <device>         capture endpoint, e.g. a virtual 7.1 cable's output side
//   --loopback <device>   whatever a render device is playing (must not be the --out device)
//   --test <scene>        built-in 7.1 scene played live: compass | circle | battle (loops)
//
// Output:
//   --out <device>        render device (default: system default output)
//   --out null            no device; output consumed on a timer (for testing)
//   --mute                open the device but send silence (pipeline test without sound)
//
// Options:
//   --latency-ms <ms>     input buffer target (default 20)
//   --no-drift            disable clock-drift compensation
//   --gain-db <dB>        master gain (default -6, headroom for HRTF near-ear boost)
//   --limiter-db <dBFS>   limiter ceiling (default -1)   --no-limiter
//   --seconds <n>         stop after n seconds (default: run until Ctrl+C)
//   --block <n> --side <deg> --back <deg> --lfe-db <dB>
//   HRTF: --hrir <file.thrir> | --sofa <file.sofa> | --spherical (default: bundled MIT KEMAR)
//
// Devices are chosen by "default", their number from `tacaudio devices`, or part of their name.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cli.h"
#include "core/layout.h"
#include "core/scene.h"
#include "core/wav.h"
#include "engine/engine.h"
#include "platform/wasapi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace tac;
using cli::Args;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI onCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}

int usage() {
    std::printf(
        "tacaudio - TacAudio real-time binaural engine\n"
        "\n"
        "  tacaudio devices\n"
        "  tacaudio run (--in <dev> | --loopback <dev> | --test compass|circle|battle) [--out <dev>|null] [options]\n"
        "\n"
        "options: --mute --latency-ms <ms> --no-drift --gain-db <dB> --limiter-db <dBFS> --no-limiter\n"
        "         --seconds <n> --block <n> --side <deg> --back <deg> --lfe-db <dB>\n"
        "%s\n"
        "devices: \"default\", a number from `tacaudio devices`, or part of the name\n",
        cli::kHrtfHelp);
    return 2;
}

void printDevices(bool render) {
    const auto devs = wasapi::listDevices(render);
    std::printf("%s\n", render ? "Output devices (render, usable with --out / --loopback):"
                               : "Input devices (capture, usable with --in):");
    if (devs.empty()) std::printf("  (none)\n");
    for (std::size_t i = 0; i < devs.size(); ++i) {
        const auto& d = devs[i];
        std::printf("  %2zu%s %-48s %u ch  %u Hz  mask 0x%-5X", i + 1, d.isDefault ? "*" : " ", d.name.c_str(),
                    d.channels, d.sampleRate, d.channelMask);
        if (d.defaultPeriodFrames && d.sampleRate)
            std::printf("  period %.1f ms (min %.2f, exclusive min %.2f ms)", d.defaultPeriodFrames * 1000.0 / d.sampleRate,
                        d.minPeriodFrames * 1000.0 / d.sampleRate, d.minExclusivePeriodMs);
        std::printf("\n");
    }
}

int cmdDevices() {
    wasapi::ComScope com;
    printDevices(true);
    std::printf("\n");
    printDevices(false);
    std::printf(
        "\n* = system default\n"
        "For games: set the virtual cable's playback side to 7.1, send the game to it, then run e.g.\n"
        "  tacaudio run --loopback \"CABLE Input\" --out \"Game\"\n");
    return 0;
}

wasapi::DeviceInfo requireDevice(const std::string& query, bool render) {
    auto d = wasapi::findDevice(query, render);
    if (!d) throw std::runtime_error(std::string("no ") + (render ? "output" : "input") + " device matches '" + query +
                                     "' (see `tacaudio devices`)");
    return *d;
}

// Pushes a looping test scene into the engine at real-time pace (system clock).
class TestSource {
public:
    TestSource(Scene scene, Engine& engine) : scene_(std::move(scene)), engine_(engine) {}
    ~TestSource() { stop(); }

    void start() {
        thread_ = std::thread([this] {
            const auto& ch = scene_.audio.channels;
            const std::size_t n = scene_.audio.numFrames(), C = ch.size();
            const double fs = scene_.audio.sampleRate;
            std::vector<float> chunk;
            std::size_t pos = 0;
            std::uint64_t pushed = 0;
            const auto t0 = Clock::now();
            while (!stop_) {
                const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
                const auto due = static_cast<std::uint64_t>(elapsed * fs) + 480;  // stay ~10 ms ahead
                while (pushed < due) {
                    const std::size_t frames = static_cast<std::size_t>(std::min<std::uint64_t>(480, due - pushed));
                    chunk.resize(frames * C);
                    for (std::size_t i = 0; i < frames; ++i) {
                        for (std::size_t c = 0; c < C; ++c) chunk[i * C + c] = ch[c][pos];
                        if (++pos == n) pos = 0;
                    }
                    engine_.pushInput(chunk.data(), frames);
                    pushed += frames;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

private:
    Scene scene_;
    Engine& engine_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// Pulls output on a timer without a device (output discarded; peak tracked).
class NullSink {
public:
    explicit NullSink(Engine& engine, double fs) : engine_(engine), fs_(fs) {}
    ~NullSink() { stop(); }

    void start() {
        thread_ = std::thread([this] {
            std::vector<float> buf(480 * 2);
            std::uint64_t pulled = 0;
            const auto t0 = Clock::now();
            while (!stop_) {
                const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
                while (pulled + 480 <= static_cast<std::uint64_t>(elapsed * fs_)) {
                    engine_.pullOutput(buf.data(), 480);
                    for (float v : buf) peak_ = std::max(peak_.load(), std::abs(v));
                    pulled += 480;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    float takePeak() { return peak_.exchange(0.0f); }

private:
    Engine& engine_;
    double fs_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<float> peak_{0.0f};
};

// Plays one tone burst per channel into a render device while loopback-capturing the same
// device, then checks every channel arrived on the right channel with no cross-talk.
// The device is a virtual cable, so nothing is audible.
int cmdCableTest(const Args& a) {
    wasapi::ComScope com;
    constexpr std::uint32_t kRate = 48000;
    const auto dev = requireDevice(a.get("--device", "CABLE Input"), true);
    if (dev.channels < 2) throw std::runtime_error("device reports no channels");
    const std::size_t C = dev.channels;
    std::printf("device   : '%s'  %zu ch, mask 0x%X\n", dev.name.c_str(), C, dev.channelMask);
    const SpeakerLayout layout = layoutFromChannelMask(dev.channelMask ? dev.channelMask : defaultChannelMask(C));

    // Test signal: channel k plays a 1 kHz burst (LFE: 60 Hz) in slot k.
    const std::size_t slot = kRate * 4 / 10, fade = kRate / 50;
    const std::size_t total = slot * C + kRate;  // + 1 s tail
    std::vector<float> signal(total * C, 0.0f);
    for (std::size_t k = 0; k < C; ++k) {
        const double f = layout.speakers[k].isLfe ? 60.0 : 1000.0;
        for (std::size_t i = 0; i < slot; ++i) {
            const double env = std::min({1.0, static_cast<double>(i) / fade, static_cast<double>(slot - i) / fade});
            signal[(k * slot + i) * C + k] = static_cast<float>(0.25 * env * std::sin(2 * std::numbers::pi * f * i / kRate));
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

    std::printf("testing  : %zu channels x 0.4 s ...\n", C);
    while (playPos.load() < total + kRate / 2 && !ren.failed() && !cap.failed())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ren.stop();
    cap.stop();
    if (ren.failed()) throw std::runtime_error("render: " + ren.error());
    if (cap.failed()) throw std::runtime_error("capture: " + cap.error());

    // Align on the first captured sample above -30 dBFS.
    const std::size_t got = capFrames.load();
    std::size_t t0 = got;
    for (std::size_t i = 0; i < got && t0 == got; ++i)
        for (std::size_t c = 0; c < C; ++c)
            if (std::abs(captured[i * C + c]) > 0.03f) {
                t0 = i;
                break;
            }
    if (t0 == got) {
        std::printf("\nFAIL: nothing came back through loopback (is the device muted, or 'Spatial sound' enabled on it?)\n");
        return 1;
    }
    // Back up to the slot start (the burst fades in over `fade` samples).
    t0 = t0 > fade ? t0 - fade / 2 : 0;

    bool ok = true;
    std::printf("\n  sent on   received on     level      cross-talk\n");
    for (std::size_t k = 0; k < C; ++k) {
        std::vector<double> e(C, 0.0);
        const std::size_t s = t0 + k * slot + 2 * fade, end = t0 + (k + 1) * slot - 2 * fade;
        for (std::size_t i = s; i < end && i < got; ++i)
            for (std::size_t c = 0; c < C; ++c) e[c] += static_cast<double>(captured[i * C + c]) * captured[i * C + c];
        const std::size_t best = static_cast<std::size_t>(std::max_element(e.begin(), e.end()) - e.begin());
        double other = 0;
        std::size_t otherCh = best;
        for (std::size_t c = 0; c < C; ++c)
            if (c != best && e[c] > other) {
                other = e[c];
                otherCh = c;
            }
        if (a.has("--verbose")) std::printf("  [slot %zu: strongest leak on ch %zu]\n", k, otherCh);
        const double n = static_cast<double>(end - s);
        const double levelDb = 10 * std::log10(std::max(e[best] / n, 1e-12) / (0.25 * 0.25 / 2));  // re. sent RMS
        const double xtalkDb = 10 * std::log10(std::max(other, 1e-20) / std::max(e[best], 1e-20));
        const bool pass = best == k && std::abs(levelDb) < 1.0 && xtalkDb < -40.0;
        ok &= pass;
        std::printf("  %-4s (%zu) -> %-4s (%zu)   %+6.1f dB   %7.1f dB   %s\n", layout.speakers[k].name.c_str(), k,
                    layout.speakers[best].name.c_str(), best, levelDb, xtalkDb, pass ? "ok" : "FAIL");
    }
    std::printf("\n%s\n", ok ? "PASS: all channels arrive separately and unchanged - the 7.1 path works."
                              : "FAIL: some channels are missing, swapped or mixed (see above).");
    return ok ? 0 : 1;
}

// Detects the onsets of short clicks in a stream, timestamped with the audio clock (seconds).
class OnsetDetector {
public:
    explicit OnsetDetector(double rate, float threshold = 0.05f) : rate_(rate), thr_(threshold) {}

    void feed(const float* d, std::size_t frames, std::size_t channels, std::uint64_t qpc100ns) {
        if (!d || qpc100ns == 0) return;
        const double t0 = static_cast<double>(qpc100ns) * 1e-7;
        for (std::size_t i = 0; i < frames; ++i) {
            float v = 0;
            for (std::size_t c = 0; c < channels; ++c) v = std::max(v, std::abs(d[i * channels + c]));
            const double t = t0 + static_cast<double>(i) / rate_;
            if (v > thr_ * 0.5f) lastLoud_ = t;
            if (armed_ && v > thr_) {
                onsets_.push_back(t);
                armed_ = false;
            } else if (!armed_ && t - lastLoud_ > 0.15) {
                armed_ = true;
            }
        }
    }
    std::vector<double> take() {
        std::lock_guard<std::mutex> l(m_);
        return onsets_;
    }
    std::mutex m_;

private:
    double rate_;
    float thr_;
    bool armed_ = true;
    double lastLoud_ = 0;
    std::vector<double> onsets_;
};

// Current performance-counter time in 100 ns units (same clock as WASAPI's qpc positions).
std::uint64_t qpcNow100ns() {
    static const double toTicks = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e7 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<std::uint64_t>(static_cast<double>(c.QuadPart) * toTicks);
}

// Matches onsets of two stages and returns the per-click delay in ms.
std::vector<double> matchDelays(const std::vector<double>& from, const std::vector<double>& to) {
    std::vector<double> d;
    for (double a : from)
        for (double b : to)
            if (b >= a - 0.002 && b - a < 0.3) {
                d.push_back((b - a) * 1000.0);
                break;
            }
    std::sort(d.begin(), d.end());
    return d;
}

int cmdRun(const Args& a) {
    const int sources = static_cast<int>(a.has("--in")) + a.has("--loopback") + a.has("--test");
    if (sources != 1) {
        std::fprintf(stderr, "error: choose exactly one source: --in, --loopback or --test\n");
        return usage();
    }
    wasapi::ComScope com;
    constexpr std::uint32_t kRate = 48000;
    const bool latencyTest = a.has("--latency-test");

    // ---- output device first (so loopback can be checked against it, and its period sizes the buffers)
    const std::string outQuery = a.get("--out", "default");
    const bool nullOut = outQuery == "null";
    std::optional<wasapi::DeviceInfo> outDev;
    if (!nullOut) outDev = requireDevice(outQuery, true);
    if (latencyTest && (nullOut || !a.has("--loopback")))
        throw std::invalid_argument("--latency-test needs --loopback <device> and a real --out device");

    // ---- source
    std::unique_ptr<wasapi::CaptureStream> capture;
    std::optional<wasapi::DeviceInfo> srcDev;
    std::optional<SceneKind> testScene;
    SpeakerLayout layout;
    const LayoutAngles angles = cli::anglesFrom(a);
    std::string sourceDesc;
    std::uint32_t capPeriod = 480;  // frames per input packet

    if (a.has("--test")) {
        const std::string s = a.get("--test");
        if (s == "compass") testScene = SceneKind::Compass;
        else if (s == "circle") testScene = SceneKind::Circle;
        else if (s == "battle") testScene = SceneKind::Battle;
        else throw std::invalid_argument("unknown test scene '" + s + "' (compass | circle | battle)");
        layout = layout71(angles);
        sourceDesc = "test scene '" + s + "' (7.1, looping)";
    } else {
        const bool loop = a.has("--loopback");
        srcDev = requireDevice(a.get(loop ? "--loopback" : "--in"), /*render=*/loop);
        if (loop && outDev && srcDev->id == outDev->id)
            throw std::runtime_error("--loopback device is the same as --out: that would feed the output back into itself");
        // Packets arrive once per engine period of the source. (Measured: loopback stays at the
        // default 10 ms period even when the device engine runs faster, so no "boosting" here.)
        if (srcDev->defaultPeriodFrames) capPeriod = srcDev->defaultPeriodFrames;
        capture = std::make_unique<wasapi::CaptureStream>();
        const auto fmt = capture->open(*srcDev, loop, kRate);
        const std::uint32_t mask = fmt.channelMask ? fmt.channelMask : defaultChannelMask(fmt.channels);
        if (!mask) throw std::runtime_error("unsupported input channel count " + std::to_string(fmt.channels));
        layout = layoutFromChannelMask(mask, angles);
        char buf[96];
        std::snprintf(buf, sizeof(buf), " (%u ch, packets %.2f ms)", fmt.channels, capPeriod * 1000.0 / kRate);
        sourceDesc = std::string(loop ? "loopback of '" : "capture '") + srcDev->name + "'" + buf;
        if (fmt.channels == 2)
            std::printf("note     : input is stereo; set the source device to 7.1 for full surround\n");
    }

    // ---- output stream (opened before the engine: its period sizes the input buffer)
    std::unique_ptr<wasapi::RenderStream> render;
    wasapi::StreamFormat outFmt{};
    std::uint32_t outPeriod = 480;
    if (!nullOut) {
        render = std::make_unique<wasapi::RenderStream>();
        wasapi::RenderOptions ro;
        ro.lowLatency = !a.has("--no-low-latency");
        ro.targetPaddingMs = a.num("--out-queue-ms", 0.0);
        ro.exclusive = a.has("--exclusive");
        ro.exclusivePeriodMs = a.num("--exclusive-period-ms", 0.0);
        outFmt = render->open(*outDev, kRate, 2, ro);
        if (outFmt.periodFrames) outPeriod = outFmt.periodFrames;
    }

    // ---- engine
    const HrirSet hrirs = cli::loadHrirs(a, kRate);
    EngineConfig cfg;
    cfg.sampleRate = kRate;
    cfg.driftCompensation = !a.has("--no-drift");
    cfg.binaural = cli::binauralFrom(a, -6.0f, 128, "balanced", "medium");
    cfg.limiter.enabled = !a.has("--no-limiter");
    cfg.limiter.ceilingDb = static_cast<float>(a.num("--limiter-db", -1.0));
    cfg.limiter.lookahead = static_cast<std::size_t>(a.num("--limiter-ms", 1.0) * 0.001 * kRate);
    // Input buffer: half of each side's packet (they arrive/leave in bursts) + one block + 1.5 ms margin.
    const double autoTargetMs =
        ((capPeriod + outPeriod) / 2.0 + static_cast<double>(cfg.binaural.blockSize)) * 1000.0 / kRate + 1.5;
    cfg.targetLatencyMs = a.num("--latency-ms", autoTargetMs);
    Engine engine(layout, hrirs, cfg);

    std::printf("source   : %s\n", sourceDesc.c_str());
    std::printf("layout   : %s\n", layout.describe().c_str());

    std::unique_ptr<NullSink> nullSink;
    const bool mute = a.has("--mute");
    double outQueueMs = 0;
    if (nullOut) {
        nullSink = std::make_unique<NullSink>(engine, kRate);
        std::printf("output   : null sink (no device)\n");
    } else {
        outQueueMs = outFmt.targetPadding * 1000.0 / kRate;
        std::printf("output   : '%s' (%s, period %.2f ms, queue %.1f ms)%s\n", outDev->name.c_str(),
                    outFmt.exclusive ? ("EXCLUSIVE " + outFmt.sampleFormat).c_str()
                                     : outFmt.lowLatency ? "low-latency" : "standard",
                    outPeriod * 1000.0 / kRate, outQueueMs, mute ? "  [MUTED: pipeline runs, device gets silence]" : "");
        if (outFmt.exclusive)
            std::printf("note     : exclusive mode - other apps cannot play on this device while TacAudio runs\n");
    }
    const double capMs = capture ? capPeriod * 1000.0 / kRate : 0.0;
    const double blockMs = cfg.binaural.blockSize * 1000.0 / kRate;
    std::printf("latency  : input packet %.1f + buffer %.1f + block %.1f + limiter %.1f + output queue %.1f = ~%.0f ms added\n",
                capMs, cfg.targetLatencyMs, blockMs, cfg.limiter.enabled ? cfg.limiter.lookahead * 1000.0 / kRate : 0.0,
                outQueueMs, capMs + engine.nominalLatencyMs() + outQueueMs);
    std::printf("gain     : %+.1f dB, ", cfg.binaural.masterGainDb);
    if (cfg.limiter.enabled) std::printf("limiter %.1f dBFS, ", cfg.limiter.ceilingDb);
    else std::printf("limiter off, ");
    std::printf("drift compensation %s\n", cfg.driftCompensation ? "on" : "off");

    // ---- latency test: clicks into the source, timestamped on the way in and on the way out
    // Output detector threshold follows the master gain, so the test also works at very low volume.
    const float outThr = 0.04f * std::pow(10.0f, cfg.binaural.masterGainDb / 20.0f);
    OnsetDetector inDet(kRate), arriveDet(kRate), writeDet(kRate, outThr), outDet(kRate, outThr);
    std::unique_ptr<wasapi::RenderStream> injector;
    std::unique_ptr<wasapi::CaptureStream> probe;
    if (latencyTest && !outFmt.exclusive) {  // loopback cannot see an exclusive-mode stream
        probe = std::make_unique<wasapi::CaptureStream>();
        probe->open(*outDev, /*loopback=*/true, kRate);
        probe->start([&outDet](const float* d, std::size_t n, bool silent, std::uint64_t qpc) {
            if (silent) return;
            std::lock_guard<std::mutex> l(outDet.m_);
            outDet.feed(d, n, 2, qpc);
        });
    }
    if (latencyTest) {
        injector = std::make_unique<wasapi::RenderStream>();
        const auto inj = injector->open(*srcDev, kRate, srcDev->channels);
        const std::size_t C = inj.channels;
        std::size_t fc = 0;  // click on the front-centre channel if there is one
        for (std::size_t c = 0; c < layout.size(); ++c)
            if (layout.speakers[c].name == "FC") fc = c;
        auto pos = std::make_shared<std::uint64_t>(0);
        injector->start([C, fc, pos](float* d, std::size_t n) {
            std::fill(d, d + n * C, 0.0f);
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint64_t p = *pos + i;
                const std::uint64_t ph = p % (kRate * 2 / 5);  // every 400 ms
                if (ph < 96) d[i * C + fc] = 0.5f * static_cast<float>(std::sin(2 * std::numbers::pi * 2000.0 * ph / kRate));
            }
            *pos += n;
        });
        std::printf("probe    : clicks every 400 ms into '%s', measured at the output of '%s'\n", srcDev->name.c_str(),
                    outDev->name.c_str());
    }

    struct PacketStats {
        std::atomic<std::uint64_t> count{0}, frames{0}, maxFrames{0}, wakeups{0};
        std::atomic<double> maxGapMs{0}, windowGapMs{0};
        std::uint64_t last = 0;
    } pkt;

    // ---- start: consumer first, then producer
    std::unique_ptr<TestSource> test;
    if (render) {
        const std::uint32_t queue = outFmt.targetPadding;
        render->start([&engine, &writeDet, mute, latencyTest, queue](float* data, std::size_t frames) {
            engine.pullOutput(data, frames);
            if (latencyTest) {
                // Frame i will be consumed after the frames already queued ahead of it.
                const std::uint64_t now = qpcNow100ns();
                const double ahead = static_cast<double>(queue) - static_cast<double>(frames);
                std::lock_guard<std::mutex> l(writeDet.m_);
                writeDet.feed(data, frames, 2, now + static_cast<std::uint64_t>(std::max(0.0, ahead) * 1e7 / kRate));
            }
            if (mute) std::fill(data, data + frames * 2, 0.0f);
        });
    } else {
        nullSink->start();
    }
    if (testScene) {
        test = std::make_unique<TestSource>(makeScene(*testScene, layout, kRate), engine);
        test->start();
    } else {
        const std::size_t C = layout.size();
        capture->start([&engine, &inDet, &arriveDet, &pkt, latencyTest, C](const float* data, std::size_t frames,
                                                                            bool silent, std::uint64_t qpc) {
            {  // packet statistics (capture thread only)
                const std::uint64_t now = qpcNow100ns();
                if (pkt.last) {
                    const double gap = (now - pkt.last) * 1e-4;
                    if (gap > 0.5) pkt.wakeups++;  // a new wake-up (not a back-to-back packet)
                    pkt.maxGapMs = std::max(pkt.maxGapMs.load(), gap);
                    pkt.windowGapMs = std::max(pkt.windowGapMs.load(), gap);
                }
                pkt.last = now;
                pkt.count++;
                pkt.frames += frames;
                pkt.maxFrames = std::max<std::uint64_t>(pkt.maxFrames.load(), frames);
            }
            if (silent || !data) {
                engine.pushSilence(frames);
                return;
            }
            engine.pushInput(data, frames);
            if (latencyTest) {
                const std::uint64_t now = qpcNow100ns();
                {
                    std::lock_guard<std::mutex> l(inDet.m_);
                    inDet.feed(data, frames, C, qpc);
                }
                // Arrival: the packet's last frame is "now".
                std::lock_guard<std::mutex> l(arriveDet.m_);
                arriveDet.feed(data, frames, C, now - static_cast<std::uint64_t>((frames - 1) * 1e7 / kRate));
            }
        });
    }

    SetConsoleCtrlHandler(onCtrl, TRUE);
    std::printf("running  : Ctrl+C to stop\n\n");
    const double limit = a.num("--seconds", 0.0);
    const auto t0 = Clock::now();
    int exitCode = 0;
    double lastPrint = 0;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double t = std::chrono::duration<double>(Clock::now() - t0).count();
        if (capture && capture->failed()) {
            std::fprintf(stderr, "\ninput stopped: %s\n", capture->error().c_str());
            exitCode = 1;
            break;
        }
        if (render && render->failed()) {
            std::fprintf(stderr, "\noutput stopped: %s\n", render->error().c_str());
            exitCode = 1;
            break;
        }
        if (t - lastPrint >= 1.0) {
            lastPrint = t;
            const EngineStats s = engine.takeStats();
            std::printf("[%5.0fs] %s buf %5.1f/%4.1f ms  drift %+5.0f ppm  cpu %4.1f%%  dyn %5.1f/%+4.1f dB  lim %5.1f dB  "
                        "underruns %llu  out-late %llu",
                        t, s.running ? "PLAY" : "WAIT", s.bufferMs, s.targetMs, s.driftPpm, s.cpuLoad * 100,
                        s.dynMinGainDb, s.dynMaxGainDb, s.limiterMinGainDb, static_cast<unsigned long long>(s.underruns),
                        static_cast<unsigned long long>(render ? render->lateWakeups() : 0));
            if (capture) std::printf("  in-gap %4.1f ms", pkt.windowGapMs.exchange(0.0));
            if (s.overruns || s.resyncs)
                std::printf("  overruns %llu resyncs %llu", static_cast<unsigned long long>(s.overruns),
                            static_cast<unsigned long long>(s.resyncs));
            if (nullSink) {
                const float pk = nullSink->takePeak();
                std::printf("  out peak %5.1f dBFS", pk > 0 ? 20.0 * std::log10(pk) : -120.0);
            }
            std::printf("\n");
            std::fflush(stdout);
        }
        if (limit > 0 && t >= limit) break;
    }

    // ---- stop: producer first, then consumer
    if (injector) injector->stop();
    if (test) test->stop();
    if (capture) capture->stop();
    if (render) render->stop();
    if (nullSink) nullSink->stop();
    if (probe) probe->stop();

    if (latencyTest) {
        const auto A = inDet.take(), B = arriveDet.take(), Cw = writeDet.take(), D = outDet.take();
        const bool excl = outFmt.exclusive;
        // Exclusive mode: no loopback point; C already includes the device buffer queue.
        const auto total = excl ? matchDelays(A, Cw) : matchDelays(A, D);
        if (total.size() < 3) {
            std::printf("\nlatency test: not enough clicks matched (%zu in, %zu out)\n", A.size(),
                        excl ? Cw.size() : D.size());
            return 1;
        }
        auto row = [](const char* name, const std::vector<double>& d) {
            if (d.empty()) {
                std::printf("  %-44s   (no match)\n", name);
                return;
            }
            std::printf("  %-44s %6.1f ms   (min %5.1f, max %5.1f)\n", name, d[d.size() / 2], d.front(), d.back());
        };
        if (pkt.count && pkt.wakeups) {
            const double secs = a.num("--seconds", 1.0);
            std::printf("\ninput packets: %.0f/s, avg %.0f frames (%.2f ms), max %llu frames; %.0f wake-ups/s, "
                        "%.1f ms of audio per wake-up, longest gap %.1f ms\n",
                        pkt.count / secs, static_cast<double>(pkt.frames) / pkt.count,
                        static_cast<double>(pkt.frames) / pkt.count * 1000.0 / kRate,
                        static_cast<unsigned long long>(pkt.maxFrames.load()), pkt.wakeups / secs,
                        static_cast<double>(pkt.frames) / pkt.wakeups * 1000.0 / kRate, pkt.maxGapMs.load());
        }
        std::printf("\nmeasured latency (%zu clicks, median):\n", total.size());
        row("A->B  cable capture delivered to TacAudio", matchDelays(A, B));
        row("B->C  inside TacAudio (buffer, block, limiter)", matchDelays(B, Cw));
        if (excl) {
            row("A->C  total, straight to the device (exclusive)", total);
            std::printf("(exclusive: no Windows mixer; C is when the device buffer starts playing the click)\n");
        } else {
            row("C->D  output queue + Windows mixer", matchDelays(Cw, D));
            row("A->D  total added by TacAudio", total);
            std::printf("(output device hardware latency not included; it is the same with DTS)\n");
        }
    }
    std::printf("stopped\n");
    return exitCode;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    try {
        const Args a = cli::parse(argc, argv, 2,
                                  {"--mute", "--no-drift", "--no-limiter", "--spherical", "--verbose", "--latency-test",
                                   "--no-low-latency", "--exclusive", "--raw-hrtf"});
        if (cmd == "devices") return cmdDevices();
        if (cmd == "cable-test") return cmdCableTest(a);
        if (cmd == "run") return cmdRun(a);
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

// tacrender — TacAudio offline renderer (milestone M1).
//
//   tacrender render <in.wav> <out.wav> [options]   multichannel (5.1/7.1/7.1.4) -> binaural
//   tacrender demo <out-dir> [options]               synthetic test scenes, 7.1 + binaural
//   tacrender make-hrir <out.thrir> [--rate 48000]   export the built-in spherical-head set
//   tacrender import-sofa <in.sofa> <out.thrir>      convert a SOFA HRTF to .thrir
//   tacrender hrir-info <set.thrir>
//   tacrender heads                                  list installed head (HRTF) sets
//   tacrender audition <out-dir>                     render a test scene through every head
//   tacrender steps-report <capture.wav>             what the footstep detector hears in a recording
//
// HRTF: --head <name> | --sofa <file.sofa> | --hrir <file.thrir> | --spherical
//       (default: SADIE II KU100 if installed, else bundled MIT KEMAR)
// Options: --block <n> --side <deg> --back <deg> --lfe-db <dB> --gain-db <dB>
//          --normalize <dBFS> --pcm24

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "cli.h"
#include "core/binaural_renderer.h"
#include "core/footstep_detector.h"
#include "core/hrtf.h"
#include "core/layout.h"
#include "core/scene.h"
#include "core/wav.h"

namespace fs = std::filesystem;
using namespace tac;
using cli::Args;

namespace {

int usage() {
    std::printf(
        "tacrender - TacAudio offline binaural renderer\n"
        "\n"
        "  tacrender render <in.wav> <out.wav> [options]\n"
        "  tacrender demo <out-dir> [options]\n"
        "  tacrender make-hrir <out.thrir> [--rate 48000]\n"
        "  tacrender import-sofa <in.sofa> <out.thrir> [--rate 48000] [--taps 512]\n"
        "  tacrender hrir-info <set.thrir>\n"
        "  tacrender heads                       list --head choices\n"
        "  tacrender audition <out-dir> [--scene compass|circle|battle]   one render per head\n"
        "  tacrender steps-report <capture.wav> [--max <lines>]   footstep detector, impact by impact\n"
        "\n"
        "%s\n"
        "options: --block <n> --side <deg> --back <deg> --lfe-db <dB> --gain-db <dB>\n"
        "         --normalize <dBFS> --pcm24\n",
        cli::kHrtfHelp);
    return 2;
}

struct Stats {
    float peak = 0;
    std::size_t clipped = 0;
};

Stats measure(const AudioBuffer& b) {
    Stats s;
    for (const auto& ch : b.channels)
        for (float v : ch) {
            s.peak = std::max(s.peak, std::abs(v));
            if (std::abs(v) > 1.0f) ++s.clipped;
        }
    return s;
}

void finishAndWrite(AudioBuffer& out, const Args& a, const fs::path& path) {
    if (a.has("--normalize")) {
        const float target = std::pow(10.0f, static_cast<float>(a.num("--normalize", -1.0)) / 20.0f);
        const Stats st = measure(out);
        if (st.peak > 0)
            for (auto& ch : out.channels)
                for (float& v : ch) v *= target / st.peak;
    }
    const Stats st = measure(out);
    writeWav(path, out, a.has("--pcm24") ? WavSampleFormat::Pcm24 : WavSampleFormat::Float32);
    std::printf("wrote    : %s  (peak %.1f dBFS%s)\n", path.string().c_str(),
                st.peak > 0 ? 20.0 * std::log10(st.peak) : -999.0,
                st.clipped ? ", WARNING: samples above 0 dBFS - use --gain-db or --normalize" : "");
}

void printBindings(const BinauralRenderer& r) {
    for (const auto& b : r.bindings())
        std::printf("  ch%-2zu %-4s want az %6.1f el %5.1f -> HRIR az %6.1f el %5.1f\n", b.channel,
                    r.layout().speakers[b.channel].name.c_str(), b.requestedAz, b.requestedEl, b.usedAz, b.usedEl);
}

AudioBuffer renderTimed(const AudioBuffer& in, const SpeakerLayout& layout, const HrirSet& h,
                        const BinauralConfig& cfg) {
    const auto t0 = std::chrono::steady_clock::now();
    AudioBuffer out = renderBinaural(in, layout, h, cfg);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double audioSecs = static_cast<double>(in.numFrames()) / in.sampleRate;
    std::printf("render   : %.2f s of audio in %.3f s (%.0fx real time)\n", audioSecs, secs,
                secs > 0 ? audioSecs / secs : 0.0);
    return out;
}

int cmdRender(const Args& a) {
    if (a.positional.size() != 2) return usage();
    AudioBuffer in = readWav(a.positional[0]);
    std::printf("input    : %s  %zu ch, %u Hz, %.2f s, mask 0x%X\n", a.positional[0].c_str(), in.numChannels(),
                in.sampleRate, static_cast<double>(in.numFrames()) / in.sampleRate, in.channelMask);
    if (in.channelMask == 0) {
        std::fprintf(stderr, "error: unknown channel layout for %zu channels\n", in.numChannels());
        return 1;
    }
    const SpeakerLayout layout = layoutFromChannelMask(in.channelMask, cli::anglesFrom(a));
    std::printf("layout   : %s\n", layout.describe().c_str());

    const HrirSet h = cli::loadHrirs(a, in.sampleRate);
    const BinauralConfig cfg = cli::binauralFrom(a);
    printBindings(BinauralRenderer(layout, h, in.sampleRate, cfg));

    AudioBuffer out = renderTimed(in, layout, h, cfg);
    finishAndWrite(out, a, a.positional[1]);
    return 0;
}

int cmdDemo(const Args& args) {
    if (args.positional.size() != 1) return usage();
    // Measured HRTFs boost the near ear by up to ~10 dB, so demos are peak-normalised
    // to -1 dBFS unless the caller chose otherwise.
    Args a = args;
    if (!a.has("--normalize") && !a.has("--gain-db")) a.opts["--normalize"] = "-1";

    const fs::path dir = a.positional[0];
    fs::create_directories(dir);
    const auto rate = static_cast<std::uint32_t>(a.num("--rate", 48000));
    const SpeakerLayout layout = layout71(cli::anglesFrom(a));
    std::printf("layout   : %s\n", layout.describe().c_str());
    const HrirSet h = cli::loadHrirs(a, rate);
    const BinauralConfig cfg = cli::binauralFrom(a);
    printBindings(BinauralRenderer(layout, h, rate, cfg));

    for (SceneKind k : {SceneKind::Compass, SceneKind::Circle, SceneKind::Battle}) {
        Scene scene = makeScene(k, layout, rate);
        const std::string base = std::string("demo_") + sceneName(k);
        std::printf("\n[%s]\n", sceneName(k));
        writeWav(dir / (base + "_71.wav"), scene.audio);
        std::printf("wrote    : %s\n", (dir / (base + "_71.wav")).string().c_str());
        AudioBuffer out = renderTimed(scene.audio, layout, h, cfg);
        finishAndWrite(out, a, dir / (base + "_binaural.wav"));
    }
    std::puts(
        "\nListen with headphones:\n"
        "  demo_compass  : footsteps front, front-right, right, back-right, back, back-left, left, front-left (2 s each)\n"
        "  demo_circle   : footsteps walking clockwise around you, starting in front (12 s)\n"
        "  demo_battle   : the same circle + loud gunshot bursts from front-right (input for M3 processing)");
    return 0;
}

int cmdMakeHrir(const Args& a) {
    if (a.positional.size() != 1) return usage();
    const HrirSet s = HrirSet::makeSphericalHead(a.num("--rate", 48000));
    s.save(a.positional[0]);
    std::printf("wrote %s (%zu measurements, %zu taps)\n", a.positional[0].c_str(), s.measurements.size(), s.irLength);
    return 0;
}

int cmdImportSofa(const Args& a) {
    if (a.positional.size() != 2) return usage();
#ifdef TAC_HAS_SOFA
    const HrirSet s = cli::loadSofaVerbose(a.positional[0], a.num("--rate", 48000),
                                           static_cast<std::size_t>(a.num("--taps", 512)));
    s.save(a.positional[1]);
    std::printf("wrote %s (%zu measurements, %zu taps)\n", a.positional[1].c_str(), s.measurements.size(), s.irLength);
    return 0;
#else
    std::fprintf(stderr, "error: built without SOFA support (TAC_WITH_SOFA=OFF)\n");
    return 1;
#endif
}

int cmdHrirInfo(const Args& a) {
    if (a.positional.size() != 1) return usage();
    const HrirSet s = HrirSet::load(a.positional[0]);
    float elMin = 90, elMax = -90;
    for (const auto& m : s.measurements) {
        elMin = std::min(elMin, m.elevationDeg);
        elMax = std::max(elMax, m.elevationDeg);
    }
    std::printf("%s: %zu measurements, %zu taps, %.0f Hz, elevation %.0f..%.0f deg\n", s.name.c_str(),
                s.measurements.size(), s.irLength, s.sampleRate, elMin, elMax);
    return 0;
}

int cmdHeads() {
    const auto heads = cli::listHeads();
    if (heads.empty()) {
        std::printf("no head sets found (SOFA support off, or data/hrtf/sadie2 missing)\n");
        return 1;
    }
    std::printf("Head (HRTF) sets - use with --head <name>:\n");
    for (const auto& h : heads) std::printf("  %-6s %s\n", h.name.c_str(), h.description.c_str());
    return 0;
}

// Renders the same scene through every installed head so the listener can pick the best fit.
int cmdAudition(const Args& args) {
    if (args.positional.size() != 1) return usage();
    const fs::path dir = args.positional[0];
    fs::create_directories(dir);
    const auto heads = cli::listHeads();
    if (heads.empty()) return cmdHeads();

    SceneKind kind = SceneKind::Compass;
    const std::string sc = args.get("--scene", "compass");
    if (sc == "circle") kind = SceneKind::Circle;
    else if (sc == "battle") kind = SceneKind::Battle;
    else if (sc != "compass") throw std::invalid_argument("unknown --scene '" + sc + "'");

    const SpeakerLayout layout = layout71(cli::anglesFrom(args));
    const Scene scene = makeScene(kind, layout, 48000);
    for (const auto& h : heads) {
        Args a = args;
        a.opts["--head"] = h.name;
        if (!a.has("--room")) a.opts["--room"] = "balanced";
        if (!a.has("--normalize") && !a.has("--gain-db")) a.opts["--normalize"] = "-1";
        std::printf("\n[%s] %s\n", h.name.c_str(), h.description.c_str());
        const HrirSet hr = cli::loadHrirs(a, 48000);
        AudioBuffer out = renderBinaural(scene.audio, layout, hr, cli::binauralFrom(a));
        finishAndWrite(out, a, dir / (h.name + "_" + sc + ".wav"));
    }
    std::printf("\nListen to each file with headphones and note which head gives the clearest front/back,\n"
                "most 'outside the head' sound and the most natural tone. Then use it with --head <name>.\n");
    return 0;
}

// Runs the footstep detector over a recording of game sound (Direct mode's "Record" button writes
// one) and prints every impact with what became of it, plus clear onsets that never became one.
int cmdStepsReport(const Args& a) {
    if (a.positional.size() != 1) return usage();
    const AudioBuffer in = readWav(a.positional[0]);
    const std::size_t nch = in.numChannels();
    const SpeakerLayout lay =
        layoutFromChannelMask(in.channelMask ? in.channelMask : defaultChannelMask(static_cast<std::uint32_t>(nch)));
    std::vector<float> az;
    std::vector<bool> lfe;
    for (std::size_t c = 0; c < nch; ++c) {
        az.push_back(c < lay.size() ? lay.speakers[c].azimuthDeg : 0.0f);
        lfe.push_back(c < lay.size() && lay.speakers[c].isLfe);
    }
    auto name = [&](std::size_t c) { return c < lay.size() ? lay.speakers[c].name.c_str() : "?"; };
    std::printf("file: %s  %zu ch, %u Hz, %.1f s\n", a.positional[0].c_str(), nch, in.sampleRate,
                static_cast<double>(in.numFrames()) / in.sampleRate);
    for (std::size_t c = 0; c < nch; ++c) {
        double e = 0;
        float pk = 0;
        for (float v : in.channels[c]) {
            e += static_cast<double>(v) * v;
            pk = std::max(pk, std::abs(v));
        }
        std::printf("  %-4s az %+5.0f  peak %6.1f dBFS  rms %6.1f dBFS\n", name(c), az[c], 20 * std::log10(pk + 1e-9),
                    10 * std::log10(e / std::max<std::size_t>(1, in.numFrames()) + 1e-12));
    }

    FootstepTracker t(in.sampleRate, az, lfe);
    std::vector<FootstepTracker::Event> events;
    t.setLog(&events);
    // Clear onsets per channel, judged 16 ms later like the detector does.
    struct Onset {
        double time;
        std::size_t ch;
        float jumpDb, peakDb, tiltDb, notLoud, crunchy;
    };
    std::vector<Onset> onsets;
    const double tick = static_cast<double>(FootstepDetector::kDecim) / in.sampleRate;
    std::vector<long long> pending(nch, -1), lastOnset(nch, -100000);
    std::vector<float> jumpMax(nch, 0.0f);
    std::vector<const float*> ptr(nch);
    const std::size_t step = FootstepDetector::kDecim;
    long long k = 0;
    for (std::size_t off = 0; off + step <= in.numFrames(); off += step, ++k) {
        for (std::size_t c = 0; c < nch; ++c) ptr[c] = in.channels[c].data() + off;
        t.process(ptr.data(), step);
        for (std::size_t c = 0; c < nch; ++c) {
            if (lfe[c]) continue;
            const auto& d = t.detector(c).debug();
            if (pending[c] >= 0) {
                jumpMax[c] = std::max(jumpMax[c], d.jumpDb);
                if (k - pending[c] >= 48) {
                    onsets.push_back(Onset{static_cast<double>(pending[c]) * tick, c, jumpMax[c], d.peakDb, d.tiltDb,
                                           d.notLoud, d.crunchy});
                    pending[c] = -1;
                }
            } else if (d.onset * d.audible >= 0.6f && k - lastOnset[c] > static_cast<long long>(0.15 / tick)) {
                pending[c] = lastOnset[c] = k;
                jumpMax[c] = d.jumpDb;
            }
        }
    }

    const int maxLines = static_cast<int>(a.num("--max", 400));
    static const char* kVerdict[] = {"own sound (straight ahead / everywhere)", "burst (rapid hits)",
                                     "first step (Sensitive only)", "walk (Sensitive + Balanced)", "steady walk (all levels)",
                                     "keeps going (rain / engine)"};
    int counts[6] = {};
    std::printf("\nimpacts (%zu):\n  time     dir    focus  level   verdict\n", events.size());
    int lines = 0;
    for (const auto& e : events) {
        ++counts[static_cast<int>(e.verdict)];
        if (lines++ < maxLines)
            std::printf("  %7.2f  %+5.0f  %5.2f  %6.1f  %s\n", e.timeSec, e.azimuthDeg, e.focus, e.levelDb,
                        kVerdict[static_cast<int>(e.verdict)]);
    }
    std::printf("\nclear onsets per speaker (%zu) - judged 16 ms later:\n  time     spk   jump  peak    tilt   result\n",
                onsets.size());
    int tooLoud = 0, bassy = 0, passed = 0;
    lines = 0;
    for (const auto& o : onsets) {
        const char* r = o.notLoud < 0.5f ? "dropped: too loud" : o.crunchy < 0.5f ? "dropped: bass-heavy" : "ok";
        (o.notLoud < 0.5f ? tooLoud : o.crunchy < 0.5f ? bassy : passed)++;
        if (lines++ < maxLines)
            std::printf("  %7.2f  %-4s %5.1f  %6.1f  %+6.1f  %s\n", o.time, name(o.ch), o.jumpDb, o.peakDb, o.tiltDb, r);
    }
    std::printf("\nsummary: %zu impacts - own %d, burst %d, keeps going %d, first step %d, walk %d, steady %d\n",
                events.size(), counts[0], counts[1], counts[5], counts[2], counts[3], counts[4]);
    std::printf("         %zu clear onsets - ok %d, too loud %d, bass-heavy %d\n", onsets.size(), passed, tooLoud, bassy);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    try {
        const Args a = cli::parse(argc, argv, 2, {"--pcm24", "--spherical", "--raw-hrtf"});
        if (cmd == "render") return cmdRender(a);
        if (cmd == "demo") return cmdDemo(a);
        if (cmd == "make-hrir") return cmdMakeHrir(a);
        if (cmd == "import-sofa") return cmdImportSofa(a);
        if (cmd == "hrir-info") return cmdHrirInfo(a);
        if (cmd == "heads") return cmdHeads();
        if (cmd == "audition") return cmdAudition(a);
        if (cmd == "steps-report") return cmdStepsReport(a);
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

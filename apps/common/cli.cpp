#include "cli.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

#ifdef TAC_HAS_SOFA
#include "sofa/sofa_import.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace tac::cli {

const char* const kHrtfHelp =
    "hrtf:    --head <name> (see `tacrender heads`) | --sofa <file.sofa> | --hrir <file.thrir> | --spherical\n"
    "         (default: SADIE II KU100 if installed, else bundled MIT KEMAR)\n"
    "         --raw-hrtf  skip diffuse-field equalisation (on by default)\n"
    "room:    --room neutral|balanced|spacious   --room-db <dB> (room level offset)\n"
    "dynamics: --dynamics off|light|medium|strong (tame loud sounds, lift quiet ones)\n"
    "          --punch 0-100 (how much of each shot's impact passes before taming; default 60)\n"
    "          --footsteps 0-12 (dB of footstep clarity around 3.5 kHz, quiet sounds only)";

std::string Args::get(const std::string& k, const std::string& def) const {
    auto it = opts.find(k);
    return it == opts.end() ? def : it->second;
}

double Args::num(const std::string& k, double def) const {
    if (!has(k)) return def;
    try {
        return std::stod(get(k));
    } catch (const std::exception&) {
        throw std::invalid_argument("option " + k + " expects a number, got '" + get(k) + "'");
    }
}

Args parse(int argc, char** argv, int first, std::initializer_list<const char*> flags) {
    Args a;
    for (int i = first; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            bool isFlag = false;
            for (const char* f : flags) isFlag |= s == f;
            if (isFlag || i + 1 >= argc) {
                a.opts[s] = "1";
            } else {
                a.opts[s] = argv[++i];
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

namespace {
fs::path g_exeDirOverride;
}

void setExeDirOverride(const fs::path& dir) { g_exeDirOverride = dir; }

fs::path exeDir() {
    if (!g_exeDirOverride.empty()) return g_exeDirOverride;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#endif
    return fs::current_path();
}

namespace {
void printSet(const HrirSet& s, const char* origin) {
    std::printf("HRIR set : %s [%s] (%zu measurements, %zu taps @ %.0f Hz)\n", s.name.c_str(), origin,
                s.measurements.size(), s.irLength, s.sampleRate);
}
}  // namespace

#ifdef TAC_HAS_SOFA
HrirSet loadSofaVerbose(const fs::path& p, double sampleRate, std::size_t taps) {
    SofaImportOptions o;
    o.sampleRate = sampleRate;
    o.maxTaps = taps;
    SofaImportReport r;
    HrirSet s = importSofa(p, o, &r);
    std::printf("SOFA     : %s  %.0f Hz -> %.0f Hz, %zu -> %zu taps (trimmed %zu leading), front gain %+.1f dB\n",
                p.filename().string().c_str(), r.sourceRate, sampleRate, r.sourceTaps, s.irLength, r.trimmedLeading,
                r.frontGainDb);
    return s;
}
#endif

namespace {
HrirSet loadHrirsRaw(const Args& a, double sampleRate);
}

HrirSet loadHrirs(const Args& a, double sampleRate) {
    HrirSet s = loadHrirsRaw(a, sampleRate);
    if (a.has("--raw-hrtf")) {
        std::printf("HRTF EQ  : off (--raw-hrtf): the dummy head's own ear resonance is audible\n");
        return s;
    }
    const DiffuseFieldEqReport r = s.diffuseFieldEqualize();
    std::printf("HRTF EQ  : diffuse-field equalised (max boost %.1f dB, max cut %.1f dB), %zu taps\n", r.maxBoostDb,
                r.maxCutDb, s.irLength);
    return s;
}

std::vector<HeadInfo> listHeads() {
    std::vector<HeadInfo> heads;
    const fs::path mit = exeDir() / "hrtf" / "MIT_KEMAR_normal_pinna.sofa";
    if (fs::exists(mit)) heads.push_back({"mit", "MIT KEMAR dummy head (1994, 710 directions)", mit});

    // SADIE II (University of York, Apache-2.0): data/hrtf/sadie2/<subject>.sofa
    std::vector<fs::path> dirs = {exeDir() / "hrtf" / "sadie2", exeDir() / ".." / ".." / "data" / "hrtf" / "sadie2",
                                  fs::current_path() / "data" / "hrtf" / "sadie2"};
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        if (fs::exists(d / "D1.sofa")) heads.push_back({"ku100", "SADIE II D1: Neumann KU100 dummy head", d / "D1.sofa"});
        if (fs::exists(d / "D2.sofa")) heads.push_back({"kemar", "SADIE II D2: KEMAR dummy head (2018)", d / "D2.sofa"});
        for (int i = 3; i <= 20; ++i) {
            const fs::path p = d / ("H" + std::to_string(i) + ".sofa");
            if (fs::exists(p)) heads.push_back({"h" + std::to_string(i), "SADIE II H" + std::to_string(i) + ": human subject", p});
        }
        break;  // first folder found wins
    }
    return heads;
}

namespace {
HrirSet loadHrirsRaw(const Args& a, double sampleRate) {
    if (a.has("--hrir")) {
        HrirSet s = HrirSet::load(a.get("--hrir"));
        printSet(s, "thrir");
        return s;
    }
#ifdef TAC_HAS_SOFA
    if (a.has("--sofa")) {
        HrirSet s = loadSofaVerbose(a.get("--sofa"), sampleRate);
        printSet(s, "sofa");
        return s;
    }
    if (!a.has("--spherical")) {
        const auto heads = listHeads();
        std::string want = a.get("--head", "");
        std::transform(want.begin(), want.end(), want.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!want.empty()) {
            for (const auto& h : heads)
                if (h.name == want) {
                    HrirSet s = loadSofaVerbose(h.path, sampleRate);
                    s.name = h.name;
                    printSet(s, h.description.c_str());
                    return s;
                }
            throw std::runtime_error("unknown --head '" + want + "' (see `tacrender heads`)");
        }
        // Default: KU100 if SADIE II is installed, else the bundled MIT KEMAR.
        for (const char* pref : {"ku100", "mit"})
            for (const auto& h : heads)
                if (h.name == pref) {
                    HrirSet s = loadSofaVerbose(h.path, sampleRate);
                    s.name = h.name;
                    printSet(s, ("default: " + h.description).c_str());
                    return s;
                }
    }
#else
    if (a.has("--sofa")) throw std::runtime_error("built without SOFA support (TAC_WITH_SOFA=OFF); use --hrir");
#endif
    HrirSet s = HrirSet::makeSphericalHead(sampleRate);
    printSet(s, "built-in model");
    return s;
}
}  // namespace

LayoutAngles anglesFrom(const Args& a) {
    LayoutAngles ang;
    ang.sideDeg = static_cast<float>(a.num("--side", ang.sideDeg));
    ang.backDeg = static_cast<float>(a.num("--back", ang.backDeg));
    return ang;
}

BinauralConfig binauralFrom(const Args& a, float defaultGainDb, std::size_t defaultBlock,
                            const std::string& defaultRoom, const std::string& defaultDynamics) {
    BinauralConfig c;
    c.blockSize = static_cast<std::size_t>(a.num("--block", static_cast<double>(defaultBlock)));
    c.lfeGainDb = static_cast<float>(a.num("--lfe-db", c.lfeGainDb));
    c.masterGainDb = static_cast<float>(a.num("--gain-db", defaultGainDb));
    c.room = roomPreset(a.get("--room", defaultRoom));
    const float wet = static_cast<float>(a.num("--room-db", 0.0));
    c.room.earlyDb += wet;
    c.room.lateDb += wet;
    if (c.room.enabled)
        std::printf("room     : %s (early %.0f dB, late %.0f dB, decay %.2f s)\n", c.room.name.c_str(), c.room.earlyDb,
                    c.room.lateDb, c.room.rt60);
    else
        std::printf("room     : neutral (dry)\n");

    c.dynamics = dynamicsPreset(a.get("--dynamics", defaultDynamics));
    c.dynamics.punch = static_cast<float>(a.num("--punch", c.dynamics.punch));
    c.dynamics.footstepDb = static_cast<float>(std::clamp(a.num("--footsteps", 0.0), 0.0, 12.0));
    if (c.dynamics.footstepDb > 0)
        std::printf("footsteps: +%.1f dB clarity around %.0f Hz while quiet\n", c.dynamics.footstepDb,
                    c.dynamics.footstepHz);
    if (c.dynamics.enabled)
        std::printf("dynamics : %s (tame above %.0f dBFS %.1f:1, lift below %.0f dBFS up to +%.0f dB, makeup %+.0f dB, "
                    "punch %.0f)\n",
                    c.dynamics.name.c_str(), c.dynamics.thresholdDb, c.dynamics.ratio, c.dynamics.boostBelowDb,
                    c.dynamics.maxBoostDb, c.dynamics.makeupDb, c.dynamics.punch);
    else
        std::printf("dynamics : off\n");
    return c;
}

}  // namespace tac::cli

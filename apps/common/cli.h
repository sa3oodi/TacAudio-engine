#pragma once
// Shared command-line helpers for the TacAudio tools.

#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "core/binaural_renderer.h"
#include "core/hrtf.h"
#include "core/layout.h"

namespace tac::cli {

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;

    bool has(const std::string& k) const { return opts.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = "") const;
    double num(const std::string& k, double def) const;
};

// Parses argv[first..]. Options in `flags` take no value; every other "--x" takes one.
Args parse(int argc, char** argv, int first, std::initializer_list<const char*> flags);

std::filesystem::path exeDir();

// Installed head (HRTF) sets selectable with --head <name>: "mit" (bundled), and if SADIE II is
// present in data/hrtf/sadie2: "ku100", "kemar", "h3".."h20".
struct HeadInfo {
    std::string name;
    std::string description;
    std::filesystem::path path;
};
std::vector<HeadInfo> listHeads();

// HRTF selection shared by all tools (first match wins):
//   --hrir <file.thrir> | --sofa <file.sofa> | --spherical | bundled MIT KEMAR | spherical head
HrirSet loadHrirs(const Args& a, double sampleRate);

#ifdef TAC_HAS_SOFA
HrirSet loadSofaVerbose(const std::filesystem::path& p, double sampleRate, std::size_t taps = 512);
#endif

LayoutAngles anglesFrom(const Args& a);
// --block, --lfe-db, --gain-db, --room neutral|balanced|spacious, --room-db (wet level offset),
// --dynamics off|light|medium|strong.
BinauralConfig binauralFrom(const Args& a, float defaultGainDb = 0.0f, std::size_t defaultBlock = 256,
                            const std::string& defaultRoom = "neutral", const std::string& defaultDynamics = "off");

extern const char* const kHrtfHelp;

}  // namespace tac::cli

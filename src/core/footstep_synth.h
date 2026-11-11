#pragma once
// Synthetic game-style footsteps (boots on gravel / concrete): heel thump + sharp heel click +
// a spray of gravel grains, then the toe roll. Made from noise and filters only (no samples), so
// every step is slightly different, like Foley in games. Used by the "Find your head" test and
// to test the footstep detector.

#include <cstdint>
#include <vector>

namespace tac {

// `count` steps, `intervalSec` apart (a walking pace is ~0.4 s), peak normalised to `peak`.
std::vector<float> synthFootsteps(double sampleRate, int count, double intervalSec, std::uint32_t seed,
                                  float peak = 0.5f);

}  // namespace tac

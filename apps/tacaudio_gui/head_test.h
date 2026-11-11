#pragma once
// "Find your head": a short listening test that plays footsteps from 8 directions through
// each installed head (HRTF) and records where the listener heard them. The head with the
// smallest direction error (and fewest front/back mix-ups) suits this listener best.

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "cli.h"

namespace tac {

namespace wasapi {
class RenderStream;
}

class HeadTest {
public:
    static constexpr int kDirs = 8;
    static constexpr float kAzimuth[kDirs] = {0, 45, 90, 135, 180, -135, -90, -45};  // + = right

    struct Trial {
        int head = 0;
        int dir = 0;      // index into kAzimuth
        int answer = -1;  // index into kAzimuth, -1 = not answered
    };
    struct Result {
        std::string name, description;
        double meanErrorDeg = 0;  // average angle between played and heard direction
        int frontBack = 0;        // front heard as back or back as front
        int answered = 0;
    };

    HeadTest();
    ~HeadTest();
    HeadTest(const HeadTest&) = delete;
    HeadTest& operator=(const HeadTest&) = delete;

    // Loads the heads, renders every trial's sound and opens `outputDevice` (shared mode).
    // Slow (a few seconds): call from a worker thread with COM initialised. Throws on failure.
    void prepare(const std::vector<cli::HeadInfo>& heads, const std::string& outputDevice, unsigned seed);
    void close();  // stops playback

    void play(std::size_t trial);  // (re)starts the trial's sound
    bool playing() const;
    std::size_t size() const { return trials_.size(); }
    const Trial& trial(std::size_t i) const { return trials_[i]; }
    void answer(std::size_t i, int dir) { trials_[i].answer = dir; }

    // Per head, best first.
    std::vector<Result> results() const;

private:
    std::vector<cli::HeadInfo> heads_;
    std::vector<Trial> trials_;
    std::vector<std::vector<float>> sounds_;  // per trial, interleaved stereo
    std::unique_ptr<wasapi::RenderStream> out_;
    std::atomic<const float*> data_{nullptr};
    std::atomic<std::size_t> len_{0}, pos_{0};
};

}  // namespace tac

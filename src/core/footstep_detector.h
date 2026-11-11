#pragma once
// Footstep detection for the direction ring: which channels carry enemy footsteps right now (0..1).
//
// Two stages:
//
// 1. FootstepDetector (per channel) finds footstep-like *impacts*: a short sound whose energy sits
//    in the crunch band (~1.5-6 kHz), at a moderate level. It requires all of
//      * an onset: the band's level (5 ms) jumps above its background (the quiet level between
//        sounds, so a run of steps does not hide the next one)
//      * not near full scale: your own gunfire and explosions on top of you
//      * not bass-heavy: judged on what the sound added in the crunch band vs in the bass, so a
//        steady rumble on the same speaker does not count against a step (explosions are rumble)
//      * audible: above the noise floor
//    Onsets are judged 16 ms later, once the whole sound is there.
//
// 2. FootstepTracker (all channels together) decides which impacts are enemy footsteps. Every
//    impact is placed (direction from how much it added on each speaker), then:
//      * your own sounds are dropped: your weapon, reload and footsteps are mixed straight ahead
//        or evenly to every speaker (not placed in the world), so impacts centred within 12
//        degrees of the front, or with no clear direction, are ignored. An enemy right in front
//        of you is on your screen anyway.
//      * the rest is reported at three levels (the app picks one, "Detection"):
//          Sensitive - every placed impact
//          Balanced  - an impact that follows another from the same direction (within 60
//                      degrees) 0.1-1.2 s earlier: the second step of a walk on. Single clicks,
//                      single shots and bullet hits are dropped.
//          Strict    - three steps or more at a steady pace (each gap 0.7-1.43x the previous one,
//                      once walking, one missed step allowed). Reloads and most gunfire are
//                      dropped.
//      * Balanced and Strict also skip bursts: onsets every < 0.15 s on a speaker at an even rate
//        (gaps within 12 %), three gaps in a row - automatic fire. Sprinting steps are just as fast
//        but uneven. A burst blocks that speaker for 0.5 s. They decide 0.25 s after an
//        impact, so the first shots of a burst are caught too; Sensitive shows impacts at once.
//    Everything is judged per speaker: in a match some speaker always has something going on, so
//    an impact's direction comes only from the speakers that carry it.
//      * a step fades: a sound whose speaker stays 10 dB above the earlier background for the
//        next 0.1-0.25 s (rain or wind starting, an engine) is not a step (Balanced / Strict).
//    Counters tell the app what was heard and why it was dropped.
// Real-time safe (no allocation after construction, no locks).

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/biquad.h"

namespace tac {

class FootstepDetector {
public:
    explicit FootstepDetector(double sampleRate = 48000.0);

    static constexpr int kDecim = 16;  // one decision every 16 samples (0.33 ms at 48 kHz)

    // Returns true when a new decision was made (every kDecim samples) during this call.
    bool process(const float* x, std::size_t n) noexcept;
    float candidate() const noexcept { return cand_; }  // 0..1: an impact that looks like a step
    // Band energy above the channel's own background (what the impact added), 20 ms.
    float excessEnergy() const noexcept { return bandE_ > floor_ ? bandE_ - floor_ : 0.0f; }
    float bandEnergy() const noexcept { return bandE_; }  // crunch band, 20 ms
    // Crunch band background when the latest candidate's onset happened (16 ms ago).
    float onsetFloor() const noexcept { return floorHist_[histPos_]; }
    void reset() noexcept;

    // The latest decision's measurements (for the steps-report tool).
    struct Debug {
        float jumpDb = 0, peakDb = -180, tiltDb = 0;  // onset size, channel peak, crunch vs rumble
        float onset = 0, notLoud = 0, crunchy = 0, audible = 0;
    };
    const Debug& debug() const noexcept { return dbg_; }

private:
    Biquad hp_, lp_, low_;          // crunch band = hp(1.5 kHz) + lp(6 kHz); rumble = lp(250 Hz)
    float fast_ = 0, slow_ = 0;     // band energy, 5 ms / 300 ms
    float bandE_ = 0, lowE_ = 0;    // band / rumble energy, 20 ms
    float peak_ = 0;                // full-band peak, 50 ms release
    float cand_ = 0;
    float floor_ = 0, lowFloor_ = 0;  // background (quiet level between sounds): band, rumble
    float cFloorUp_ = 0, cFloorDown_ = 0;
    Debug dbg_;
    float cFast_, cSlow_, c20_, cPeak_;
    int tick_ = 0;
    // An explosion's first milliseconds can look crunchy before its rumble builds up.
    static constexpr int kDelay = 48;  // decisions (16 ms at 3 kHz)
    float onsetHist_[kDelay] = {};
    float floorHist_[kDelay] = {};
    int histPos_ = 0;
};

class FootstepTracker {
public:
    static constexpr std::size_t kMaxChannels = 8;
    static constexpr int kLevels = 3;  // 0 sensitive, 1 balanced, 2 strict

    struct Stats {
        int impacts = 0;   // footstep-like impacts heard
        int own = 0;       // dropped: straight ahead / no clear direction (your own sounds)
        int single = 0;    // dropped by Balanced: not followed up like a walk
        int balanced = 0;  // reported at Balanced
        int strict = 0;    // reported at Strict
    };

    FootstepTracker() = default;
    // One entry per channel (speaker): its direction (degrees, 0 = front, + = right) and LFE flag.
    FootstepTracker(double sampleRate, const std::vector<float>& azimuthDeg, const std::vector<bool>& lfe);

    std::size_t channels() const noexcept { return n_; }
    // ch: planar input, one pointer per channel (channels() of them).
    void process(const float* const* ch, std::size_t frames) noexcept;
    // Strongest footstep activity on channel c at `level` since the last call.
    float takeMax(std::size_t c, int level) noexcept;
    void reset() noexcept;

    const Stats& totals() const noexcept { return total_; }

    // Offline analysis (steps-report): every impact and what became of it. Not real-time safe.
    enum class Verdict { Own, Burst, First, Walk, Steady, Sustained };
    struct Event {
        double timeSec = 0;
        float azimuthDeg = 0, focus = 0, levelDb = 0;
        Verdict verdict = Verdict::First;
    };
    void setLog(std::vector<Event>* log) noexcept { log_ = log; }
    const FootstepDetector& detector(std::size_t c) const { return det_[c]; }
    Stats takeStats() noexcept {  // since the last call
        const Stats s = fresh_;
        fresh_ = {};
        return s;
    }

private:
    using Levels = std::array<float, kMaxChannels>;
    // An impact waiting for its Balanced / Strict verdict (0.25 s, so a burst can show itself).
    struct Pending {
        std::int64_t start = 0;
        float az = 0, focus = 0, levelDb = 0;
        std::size_t dom = 0;  // loudest speaker
        Levels e{};
        float floor0 = 0;     // its speaker's background when it started
        float minAfter = 0;   // its speaker's quietest level 0.1-0.25 s later (steps fade, rain does not)
    };
    void decide() noexcept;
    void classify() noexcept;
    void confirm(const Pending& p) noexcept;
    void light(int level, const Levels& e) noexcept;
    void count(int Stats::*field) noexcept {
        ++(total_.*field);
        ++(fresh_.*field);
    }

    struct Walk {
        std::int64_t last = -1;   // tick of the last step, -1 = unused
        std::int64_t gap = 0;     // pace (ticks between steps), 0 = unknown
        float az = 0;             // direction of the last step (degrees)
        int count = 0;            // steps
        int steady = 0;           // steps at a steady pace
    };

    std::size_t n_ = 0;
    double tickRate_ = 3000.0;
    std::vector<FootstepDetector> det_;
    std::array<float, kMaxChannels> cosAz_{}, sinAz_{};
    std::array<bool, kMaxChannels> lfe_{};
    int phase_ = 0;                 // samples into the current decision
    std::int64_t now_ = 0;          // decisions so far
    // Per speaker: a game mix always has something going on somewhere, so onsets, bursts and an
    // impact's direction are judged on each speaker by itself.
    // An impact starts at an onset and is measured for 40 ms, on the speakers that carry it; a
    // speaker's onsets in the next 150 ms belong to the same impact (heel, gravel, toe).
    std::array<bool, kMaxChannels> high_{};        // candidate above the rise threshold
    std::array<bool, kMaxChannels> part_{};        // speaker carries the impact being measured
    std::array<std::int64_t, kMaxChannels> lastImpact_{}, lastRise_{}, vetoUntil_{};
    // Bursts: onsets every < 0.15 s at an even rate, three gaps in a row (automatic fire).
    std::array<int, kMaxChannels> shortRun_{};
    std::array<double, kMaxChannels> prevGap_{};  // seconds
    std::array<std::int64_t, kMaxChannels> burstAt_{};  // last burst seen on the speaker
    std::array<Pending, 16> pending_{};
    std::size_t pendHead_ = 0, pendCount_ = 0;
    bool measuring_ = false;
    std::int64_t impactStart_ = 0;
    std::array<float, kMaxChannels> evE_{};
    std::array<float, kMaxChannels> startFloor_{};  // each speaker's background before the impact
    std::array<Walk, 4> walks_{};
    std::array<std::array<float, kMaxChannels>, kLevels> act_{}, max_{};
    float cRel_ = 0;
    Stats total_{}, fresh_{};
    std::vector<Event>* log_ = nullptr;
};

}  // namespace tac

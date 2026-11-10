#pragma once
// AudioSession: one running TacAudio pipeline (loopback capture -> engine -> render), with live
// sound controls. Used by the GUI. Control methods are thread-safe with respect to each other;
// the audio threads are never blocked by them.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/hrtf.h"
#include "engine/engine.h"

namespace tac {

namespace wasapi {
class CaptureStream;
class RenderStream;
}  // namespace wasapi

struct DeviceSettings {
    std::string source = "CABLE Input";  // render device whose output is captured (loopback)
    std::string output = "Game";         // where TacAudio plays
    bool exclusive = true;
};

struct SoundSettings {
    std::string head = "kemar";
    std::string room = "balanced";
    std::string dynamics = "medium";
    float punch = 60.0f;
    float footstepsDb = 3.0f;
    float bassDb = 0.0f;    // LFE (subwoofer channel) level into both ears; 0 dB = no boost
    float volumeDb = -6.0f;
    bool enabled3d = true;  // false = plain stereo downmix (A/B)

    // True if switching between a and b needs a renderer rebuild (not just volume / 3D toggle).
    static bool needsRebuild(const SoundSettings& a, const SoundSettings& b) {
        return a.head != b.head || a.room != b.room || a.dynamics != b.dynamics || a.punch != b.punch ||
               a.footstepsDb != b.footstepsDb || a.bassDb != b.bassDb;
    }
};

struct SessionStatus {
    bool running = false;
    EngineStats engine;
    double latencyMs = 0;          // estimated added latency (input packet + engine + output queue)
    std::uint64_t outLate = 0;
    double inGapMs = 0;            // longest input gap since the last status()
    std::string source, output;    // descriptions
    std::string error;             // last error (empty = fine)
};

class AudioSession {
public:
    AudioSession();
    ~AudioSession();

    // Opens the devices and starts audio. Loads the head (can take ~1 s the first time):
    // call from a worker thread, not the UI thread. Throws std::exception on failure.
    void start(const DeviceSettings& devices, const SoundSettings& sound);
    void stop();
    bool running() const;

    // Applies sound settings while running. Head / room / dynamics / punch / footsteps rebuild
    // the renderer (click-free swap; may load a head - worker thread); volume and 3D are instant.
    void apply(const SoundSettings& sound);

    // Instant controls (no rebuild; safe to call from the UI thread every frame).
    void setVolumeDb(float db);
    void set3d(bool enabled);

    // Snapshot for the UI (~1 per second): resets the "since last call" peaks.
    SessionStatus status();
    // Output peaks per ear since the last call (linear); 0 when stopped. Cheap: call per frame.
    void meters(float& left, float& right);

private:
    std::shared_ptr<const HrirSet> loadHead(const std::string& name);  // cached
    BinauralConfig makeConfig(const SoundSettings& s) const;

    mutable std::mutex m_;
    std::map<std::string, std::shared_ptr<const HrirSet>> heads_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<wasapi::CaptureStream> capture_;
    std::unique_ptr<wasapi::RenderStream> render_;
    SoundSettings sound_;
    double extraLatencyMs_ = 0;    // input packet + output queue
    std::string sourceDesc_, outputDesc_, error_;
    struct Gap;
    std::shared_ptr<Gap> gap_;
};

}  // namespace tac

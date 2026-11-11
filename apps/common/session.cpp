#include "session.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "cli.h"
#include "core/wav.h"
#include "platform/wasapi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace tac {

namespace {
constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kBlock = 128;

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
}  // namespace

struct AudioSession::Gap {
    std::atomic<double> maxMs{0.0};
    std::uint64_t last = 0;  // capture thread only
};

AudioSession::AudioSession() = default;
AudioSession::~AudioSession() { stop(); }

bool AudioSession::running() const {
    std::lock_guard<std::mutex> l(m_);
    return engine_ != nullptr;
}

std::shared_ptr<const HrirSet> AudioSession::loadHead(const std::string& name) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = heads_.find(name);
        if (it != heads_.end()) return it->second;
    }
    cli::Args a;
    a.opts["--head"] = name;
    auto h = std::make_shared<const HrirSet>(cli::loadHrirs(a, kRate));  // slow part, outside the lock
    std::lock_guard<std::mutex> l(m_);
    heads_[name] = h;
    return h;
}

BinauralConfig AudioSession::makeConfig(const SoundSettings& s) const {
    cli::Args a;
    a.opts["--room"] = s.room;
    a.opts["--dynamics"] = s.dynamics;
    a.opts["--punch"] = std::to_string(s.punch);
    a.opts["--footsteps"] = std::to_string(s.footstepsDb);
    a.opts["--footsteps-smart"] = s.footstepsSmart ? "1" : "0";
    a.opts["--eq"] = s.eq;
    a.opts["--lfe-db"] = std::to_string(s.bassDb);
    a.opts["--gain-db"] = "0";  // volume is applied by the engine (instant, no rebuild)
    return cli::binauralFrom(a, 0.0f, kBlock, s.room, s.dynamics);
}

void AudioSession::start(const DeviceSettings& dev, const SoundSettings& sound) {
    stop();
    wasapi::ComScope com;

    const auto src = wasapi::findDevice(dev.source, /*render=*/true);
    if (!src) throw std::runtime_error("source device '" + dev.source + "' not found");
    const auto out = wasapi::findDevice(dev.output, /*render=*/true);
    if (!out) throw std::runtime_error("output device '" + dev.output + "' not found");
    if (src->id == out->id) throw std::runtime_error("source and output must be different devices");

    auto head = loadHead(sound.head);

    auto capture = std::make_unique<wasapi::CaptureStream>();
    const auto cf = capture->open(*src, /*loopback=*/true, kRate);
    const std::uint32_t mask = cf.channelMask ? cf.channelMask : defaultChannelMask(cf.channels);
    if (!mask) throw std::runtime_error("unsupported source channel count " + std::to_string(cf.channels));
    const SpeakerLayout layout = layoutFromChannelMask(mask);

    auto render = std::make_unique<wasapi::RenderStream>();
    wasapi::RenderOptions ro;
    ro.exclusive = dev.exclusive;
    const auto rf = render->open(*out, kRate, 2, ro);

    const std::uint32_t capPeriod = src->defaultPeriodFrames ? src->defaultPeriodFrames : 480;
    const std::uint32_t outPeriod = rf.periodFrames ? rf.periodFrames : 480;

    EngineConfig cfg;
    cfg.sampleRate = kRate;
    cfg.binaural = makeConfig(sound);
    cfg.targetLatencyMs = ((capPeriod + outPeriod) / 2.0 + kBlock) * 1000.0 / kRate + 1.5;
    auto engine = std::make_unique<Engine>(layout, *head, cfg);
    engine->setOutputGainDb(sound.volumeDb);
    engine->setBypass(!sound.enabled3d);

    auto gap = std::make_shared<Gap>();
    Engine* e = engine.get();
    render->start([e](float* d, std::size_t n) { e->pullOutput(d, n); });
    capture->start([e, gap](const float* d, std::size_t n, bool silent, std::uint64_t) {
        const std::uint64_t now = qpcNow100ns();
        if (gap->last) gap->maxMs = std::max(gap->maxMs.load(), (now - gap->last) * 1e-4);
        gap->last = now;
        if (silent || !d) e->pushSilence(n);
        else e->pushInput(d, n);
    });

    std::lock_guard<std::mutex> l(m_);
    extraLatencyMs_ = capPeriod * 1000.0 / kRate + rf.targetPadding * 1000.0 / kRate;
    sourceDesc_ = src->name + " (" + std::to_string(cf.channels) + " ch)";
    outputDesc_ = out->name + (rf.exclusive ? " (exclusive " : rf.lowLatency ? " (low-latency " : " (shared ") +
                  std::to_string(static_cast<int>(outPeriod * 1000.0 / kRate + 0.5)) + " ms)";
    error_.clear();
    sound_ = sound;
    gap_ = gap;
    capture_ = std::move(capture);
    render_ = std::move(render);
    engine_ = std::move(engine);
}

void AudioSession::stop() {
    std::unique_ptr<Engine> engine;
    std::unique_ptr<wasapi::CaptureStream> capture;
    std::unique_ptr<wasapi::RenderStream> render;
    {
        std::lock_guard<std::mutex> l(m_);
        engine = std::move(engine_);
        capture = std::move(capture_);
        render = std::move(render_);
    }
    if (capture) capture->stop();  // producer first
    if (render) render->stop();
    // engine destroyed last, after both audio threads have stopped
}

void AudioSession::apply(const SoundSettings& s) {
    SoundSettings old;
    Engine* e = nullptr;
    {
        std::lock_guard<std::mutex> l(m_);
        if (!engine_) {
            sound_ = s;
            return;
        }
        old = sound_;
        e = engine_.get();
    }
    if (SoundSettings::needsRebuild(old, s)) {
        auto head = loadHead(s.head);
        const BinauralConfig cfg = makeConfig(s);
        std::lock_guard<std::mutex> l(m_);
        if (engine_.get() != e) return;  // stopped/restarted meanwhile
        // A previous swap takes ~30 ms; wait for it rather than dropping this change.
        for (int i = 0; i < 100 && !e->reconfigure(cfg, *head); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> l(m_);
    // Keep the instant controls as they are now (the UI may have moved them meanwhile).
    const float vol = sound_.volumeDb;
    const bool on3d = sound_.enabled3d;
    sound_ = s;
    sound_.volumeDb = vol;
    sound_.enabled3d = on3d;
}

void AudioSession::setVolumeDb(float db) {
    std::lock_guard<std::mutex> l(m_);
    sound_.volumeDb = db;
    if (engine_) engine_->setOutputGainDb(db);
}

void AudioSession::set3d(bool enabled) {
    std::lock_guard<std::mutex> l(m_);
    sound_.enabled3d = enabled;
    if (engine_) engine_->setBypass(!enabled);
}

void AudioSession::meters(float& left, float& right) {
    std::lock_guard<std::mutex> l(m_);
    left = right = 0.0f;
    if (engine_) engine_->takeMeters(left, right);
}

std::size_t AudioSession::inputMeters(float* peaks, float* azimuth, bool* lfe) {
    std::lock_guard<std::mutex> l(m_);
    if (!engine_) return 0;
    const SpeakerLayout& lay = engine_->inputLayout();
    const std::size_t n = std::min<std::size_t>(lay.size(), Engine::kMaxMeterChannels);
    engine_->takeInputPeaks(peaks, n);
    for (std::size_t c = 0; c < n; ++c) {
        azimuth[c] = lay.speakers[c].azimuthDeg;
        lfe[c] = lay.speakers[c].isLfe;
    }
    return n;
}

SessionStatus AudioSession::status() {
    std::lock_guard<std::mutex> l(m_);
    SessionStatus st;
    st.error = error_;
    if (!engine_) return st;
    st.running = true;
    st.engine = engine_->takeStats();
    st.latencyMs = extraLatencyMs_ + engine_->nominalLatencyMs();
    st.outLate = render_ ? render_->lateWakeups() : 0;
    st.inGapMs = gap_ ? gap_->maxMs.exchange(0.0) : 0.0;
    st.source = sourceDesc_;
    st.output = outputDesc_;
    if (capture_ && capture_->failed()) st.error = "input stopped: " + capture_->error();
    if (render_ && render_->failed()) st.error = "output stopped: " + render_->error();
    return st;
}

}  // namespace tac

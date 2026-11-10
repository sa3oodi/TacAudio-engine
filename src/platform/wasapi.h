#pragma once
// WASAPI (Windows Audio Session API) shared-mode streams.
//
// Both streams ask Windows for 32-bit float at the engine sample rate
// (AUTOCONVERTPCM), so the engine never deals with device formats.
// Callbacks run on a dedicated thread registered with MMCSS "Pro Audio".

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tac::wasapi {

// RAII COM initialisation (multithreaded apartment) for the calling thread.
class ComScope {
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool owns_ = false;
};

struct DeviceInfo {
    std::wstring id;
    std::string name;
    bool isRender = true;     // render (output) vs capture (input) endpoint
    bool isDefault = false;
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelMask = 0;
    // Shared-mode engine periods in frames (IAudioClient3); 0 = unknown.
    std::uint32_t defaultPeriodFrames = 0;
    std::uint32_t minPeriodFrames = 0;
    double minExclusivePeriodMs = 0;  // IAudioClient::GetDevicePeriod minimum (exclusive mode)
};

std::vector<DeviceInfo> listDevices(bool render);

// query: "default", a 1-based index from listDevices(), or a case-insensitive name substring.
std::optional<DeviceInfo> findDevice(const std::string& query, bool render);

struct StreamFormat {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t channelMask = 0;
    std::uint32_t bufferFrames = 0;
    std::uint32_t periodFrames = 0;    // engine period this stream runs at
    std::uint32_t targetPadding = 0;   // render: frames kept queued in the device buffer
    bool lowLatency = false;           // render: IAudioClient3 low-latency shared stream
    bool exclusive = false;            // render: exclusive mode (bypasses the Windows mixer)
    std::string sampleFormat = "float32";
};

struct RenderOptions {
    bool lowLatency = true;        // try IAudioClient3 at the device's minimum period
    double targetPaddingMs = 0;    // 0 = auto: one period + a small margin (not the whole buffer)
    // Exclusive mode: TacAudio owns the device (other apps cannot use it meanwhile) and
    // bypasses the Windows mixer. 0 = device minimum period.
    bool exclusive = false;
    double exclusivePeriodMs = 0;
};

class CaptureStream {
public:
    // frames of interleaved float; silent == true means the device flagged silence (data may be null).
    // qpc100ns: performance-counter time of the first frame, in 100 ns units (0 if unavailable).
    using Callback = std::function<void(const float* data, std::size_t frames, bool silent, std::uint64_t qpc100ns)>;

    CaptureStream();
    ~CaptureStream();

    // loopback: capture what a *render* device is playing. Otherwise `device` is a capture endpoint.
    StreamFormat open(const DeviceInfo& device, bool loopback, std::uint32_t sampleRate);
    void start(Callback cb);
    void stop();
    bool failed() const noexcept { return failed_.load(); }
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> failed_{false};
};

class RenderStream {
public:
    using Callback = std::function<void(float* data, std::size_t frames)>;  // fill interleaved float

    RenderStream();
    ~RenderStream();

    StreamFormat open(const DeviceInfo& device, std::uint32_t sampleRate, std::uint32_t channels = 2,
                      const RenderOptions& options = {});
    void start(Callback cb);
    void stop();
    bool failed() const noexcept { return failed_.load(); }
    std::string error() const;
    // Device wake-ups the render thread answered more than one period late (each one is a
    // likely audible glitch). Counted since start.
    std::uint64_t lateWakeups() const noexcept { return late_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> late_{0};
};

}  // namespace tac::wasapi

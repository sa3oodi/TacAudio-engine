#pragma once
// Virtual-cable checks shared by the app's setup assistant and `tacaudio-cli cable-test`.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "platform/wasapi.h"

namespace tac {

// The VB-CABLE playback endpoint ("CABLE Input ..."), if installed.
std::optional<wasapi::DeviceInfo> findCable(const std::vector<wasapi::DeviceInfo>& renderDevices);

// True if the device is configured as 7.1 (8 channels).
inline bool isSevenOne(const wasapi::DeviceInfo& d) { return d.channels == 8; }

struct CableChannelResult {
    std::string sent, received;  // speaker names, e.g. "FL"
    double levelDb = 0;          // received level relative to sent
    double crosstalkDb = -200;   // strongest leak into another channel, relative to the wanted one
    bool pass = false;
};

struct CableTestResult {
    bool pass = false;
    std::string message;                       // one-line verdict
    std::vector<CableChannelResult> channels;  // empty if nothing came back
};

// Plays one tone burst per channel into `device` while loopback-capturing it and checks that
// every channel arrives on the right channel, at the right level, without cross-talk.
// Inaudible on a virtual cable. Takes about (0.4 s x channels + 1.5 s). Needs COM on the
// calling thread. Throws std::exception if the device cannot be opened.
CableTestResult runCableTest(const wasapi::DeviceInfo& device);

}  // namespace tac

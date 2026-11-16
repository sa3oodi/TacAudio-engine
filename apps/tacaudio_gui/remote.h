#pragma once
// Phone remote: a small web server inside the app, for the home network only. A phone opens the
// page from the QR code (http://<pc>:<port>/#k=<key>) and controls TacAudio with it.
//
//   * Off by default; listens only while Settings > Phone remote is on.
//   * Every API call needs the random key from the QR code; requests from outside private
//     network addresses (10.x, 172.16-31.x, 192.168.x, link-local, loopback) are refused.
//   * Threads: the server thread never touches the app. It serves the latest state snapshot (set
//     by the UI thread) and queues commands that the UI thread applies like its own controls.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tac {

struct RemoteCommand {
    std::string name, value;
};

class RemoteServer {
public:
    RemoteServer();
    ~RemoteServer();
    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    // Listens on the first free port from `port` (tries 10). False + error() if none works.
    // bindAddress: "0.0.0.0" = the home network; "127.0.0.1" = this PC only (tests).
    bool start(const std::string& key, int port = kDefaultPort, const std::string& bindAddress = "0.0.0.0");
    void stop();
    bool running() const { return running_; }
    const std::string& error() const { return error_; }
    std::string url() const;  // what the QR code holds; "" if no network address was found

    void setState(std::string json);          // UI thread: the snapshot phones read
    std::vector<RemoteCommand> takeCommands();  // UI thread: commands since the last call
    int phonesConnected() const;              // phones heard from in the last 10 s

    static constexpr int kDefaultPort = 47800;
    static std::string newKey();     // 128 random bits as hex
    static std::string localIpv4();  // this PC on the home network ("" if none)
    static bool isPrivateIpv4(const std::string& addr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    bool running_ = false;
    int port_ = 0;
    std::string key_, error_, ip_;

    mutable std::mutex mutex_;  // guards the members below (server thread <-> UI thread)
    std::string state_ = "{}";
    std::vector<RemoteCommand> commands_;
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> phones_;
};

}  // namespace tac

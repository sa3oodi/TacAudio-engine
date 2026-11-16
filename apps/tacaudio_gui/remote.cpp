// httplib first: it includes winsock2.h, which must come before windows.h.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <httplib.h>

#include "remote.h"

#include <iphlpapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "remote_page.h"

namespace tac {

struct RemoteServer::Impl {
    httplib::Server server;
};

RemoteServer::RemoteServer() = default;

RemoteServer::~RemoteServer() { stop(); }

namespace {

bool sameKey(const std::string& a, const std::string& b) {  // constant time
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

bool parseIpv4(const std::string& s, unsigned char out[4]) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    out[0] = static_cast<unsigned char>(a);
    out[1] = static_cast<unsigned char>(b);
    out[2] = static_cast<unsigned char>(c);
    out[3] = static_cast<unsigned char>(d);
    return true;
}

}  // namespace

bool RemoteServer::isPrivateIpv4(const std::string& addr) {
    unsigned char ip[4];
    if (!parseIpv4(addr, ip)) return false;
    return ip[0] == 10 || ip[0] == 127 || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) || (ip[0] == 192 && ip[1] == 168) ||
           (ip[0] == 169 && ip[1] == 254);
}

std::string RemoteServer::newKey() {
    unsigned char b[16] = {};
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", b[i]);
    return std::string(hex, 32);
}

std::string RemoteServer::localIpv4() {
    // The address of an active Ethernet / Wi-Fi adapter with a gateway (the home network), private
    // ranges first. VPN and virtual adapters usually have no gateway or another type.
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    IP_ADAPTER_ADDRESSES* list = nullptr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        const ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                                          GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                             nullptr, list, &size);
        if (r == ERROR_SUCCESS) break;
        if (r != ERROR_BUFFER_OVERFLOW) return {};
        list = nullptr;
    }
    std::string fallback;
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* sa = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            if (sa->sin_family != AF_INET) continue;
            char s[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sa->sin_addr, s, sizeof(s));
            const std::string ip = s;
            if (ip.rfind("169.254.", 0) == 0) continue;
            if (isPrivateIpv4(ip)) return ip;
            if (fallback.empty()) fallback = ip;
        }
    }
    return fallback;
}

bool RemoteServer::start(const std::string& key, int port, const std::string& bindAddress) {
    stop();
    error_.clear();
    key_ = key;
    ip_ = bindAddress == "127.0.0.1" ? bindAddress : localIpv4();
    impl_ = std::make_unique<Impl>();
    auto& srv = impl_->server;

    // Home network only, and every API call needs the key.
    srv.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (isPrivateIpv4(req.remote_addr)) return httplib::Server::HandlerResponse::Unhandled;
        res.status = 403;
        return httplib::Server::HandlerResponse::Handled;
    });
    auto authorised = [this](const httplib::Request& req) {
        const std::string k = req.has_header("X-Key") ? req.get_header_value("X-Key") : req.get_param_value("k");
        if (!sameKey(k, key_)) return false;
        std::lock_guard<std::mutex> l(mutex_);
        const auto now = std::chrono::steady_clock::now();
        auto it = std::find_if(phones_.begin(), phones_.end(), [&](const auto& p) { return p.first == req.remote_addr; });
        if (it != phones_.end()) it->second = now;
        else if (phones_.size() < 16) phones_.emplace_back(req.remote_addr, now);
        return true;
    };

    srv.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(remotePageHtml(), "text/html; charset=utf-8");
    });
    srv.Get("/manifest.json", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"name":"TacAudio Remote","short_name":"TacAudio","start_url":"/","display":"standalone",)"
                        R"("background_color":"#0e1116","theme_color":"#0e1116"})",
                        "application/manifest+json");
    });
    srv.Get("/api/state", [this, authorised](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        if (!authorised(req)) {
            res.status = 403;
            return;
        }
        std::lock_guard<std::mutex> l(mutex_);
        res.set_content(state_, "application/json");
    });
    srv.Post("/api/set", [this, authorised](const httplib::Request& req, httplib::Response& res) {
        if (!authorised(req)) {
            res.status = 403;
            return;
        }
        const std::string name = req.get_param_value("name"), value = req.get_param_value("value");
        if (name.empty() || name.size() > 32 || value.size() > 64) {
            res.status = 400;
            return;
        }
        std::lock_guard<std::mutex> l(mutex_);
        if (commands_.size() < 256) commands_.push_back({name, value});
        res.set_content("ok", "text/plain");
    });
    srv.set_payload_max_length(4096);
    srv.set_read_timeout(5, 0);
    srv.set_write_timeout(5, 0);

    for (int p = port; p < port + 10; ++p)
        if (srv.bind_to_port(bindAddress, p)) {
            port_ = p;
            running_ = true;
            break;
        }
    if (!running_) {
        error_ = "Could not open a network port (" + std::to_string(port) + "-" + std::to_string(port + 9) +
                 " are all in use).";
        impl_.reset();
        return false;
    }
    thread_ = std::thread([this] { impl_->server.listen_after_bind(); });
    return true;
}

void RemoteServer::stop() {
    if (impl_) impl_->server.stop();
    if (thread_.joinable()) thread_.join();
    impl_.reset();
    running_ = false;
    std::lock_guard<std::mutex> l(mutex_);
    phones_.clear();
    commands_.clear();
}

std::string RemoteServer::url() const {
    if (!running_ || ip_.empty()) return {};
    return "http://" + ip_ + ":" + std::to_string(port_) + "/#k=" + key_;
}

void RemoteServer::setState(std::string json) {
    std::lock_guard<std::mutex> l(mutex_);
    state_ = std::move(json);
}

std::vector<RemoteCommand> RemoteServer::takeCommands() {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<RemoteCommand> out;
    out.swap(commands_);
    return out;
}

int RemoteServer::phonesConnected() const {
    std::lock_guard<std::mutex> l(mutex_);
    const auto now = std::chrono::steady_clock::now();
    return static_cast<int>(std::count_if(phones_.begin(), phones_.end(),
                                          [&](const auto& p) { return now - p.second < std::chrono::seconds(10); }));
}

}  // namespace tac

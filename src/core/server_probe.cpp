#include "server_probe.h"

#include <charconv>
#include <utility>

namespace {

constexpr unsigned short default_port = 25565;

bool parse_ipv4(const std::string& text) {
    int parts = 0;
    std::size_t position = 0;
    while (parts < 4) {
        const std::size_t dot = text.find('.', position);
        const std::string_view part(text.data() + position,
                                    (dot == std::string::npos ? text.size() : dot) - position);
        if (part.empty() || part.size() > 3) {
            return false;
        }
        unsigned int value = 0;
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (error != std::errc{} || end != part.data() + part.size() || value > 255) {
            return false;
        }
        ++parts;
        if (dot == std::string::npos) {
            return parts == 4 && position + part.size() == text.size();
        }
        position = dot + 1;
    }
    return false;
}

} // namespace

bool parse_server_endpoint(const std::string& text, std::string& ip, unsigned short& port) {
    const std::size_t colon = text.find(':');
    std::string address = colon == std::string::npos ? text : text.substr(0, colon);
    unsigned short parsed_port = default_port;

    if (colon != std::string::npos) {
        const std::string port_text = text.substr(colon + 1);
        unsigned int value = 0;
        const auto [end, error] = std::from_chars(
            port_text.data(), port_text.data() + port_text.size(), value);
        if (error != std::errc{} || end != port_text.data() + port_text.size() ||
            value == 0 || value > 65535) {
            return false;
        }
        parsed_port = static_cast<unsigned short>(value);
    }

    if (!parse_ipv4(address)) {
        return false;
    }
    ip = std::move(address);
    port = parsed_port;
    return true;
}

ServerProbe::~ServerProbe() {
    request_stop();
    join_worker();
}

void ServerProbe::start(const std::string& ip, unsigned short port) {
    request_stop();
    join_worker();

    {
        std::lock_guard lock(mutex_);
        ip_ = ip;
        port_ = port;
        has_result_ = false;
        status_ = ServerStatus{};
    }
    cancelled_ = false;
    running_ = true;
    worker_ = std::thread([this, ip, port] {
        ServerStatus status = query_server_status(
            ip, port, [this] { return cancelled_.load(); }, true);
        {
            std::lock_guard lock(mutex_);
            status_ = std::move(status);
            has_result_ = true;
        }
        running_ = false;
    });
}

void ServerProbe::request_stop() {
    cancelled_ = true;
}

void ServerProbe::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
}

ServerProbe::Snapshot ServerProbe::snapshot() const {
    Snapshot snapshot;
    snapshot.running = running_.load();

    std::lock_guard lock(mutex_);
    snapshot.ip = ip_;
    snapshot.port = port_;
    snapshot.has_result = has_result_;
    snapshot.status = status_;
    return snapshot;
}

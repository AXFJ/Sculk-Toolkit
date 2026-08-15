#pragma once

#include "status_query.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Runs one server-list-ping query off the caller's thread so a UI can stay
// responsive. Independent of scanning, so it also works in UDP-only mode.
class ServerProbe {
public:
    struct Snapshot {
        std::string ip;
        unsigned short port = 0;
        bool running = false;
        bool has_result = false;
        ServerStatus status;
    };

    ~ServerProbe();

    void start(const std::string& ip, unsigned short port);
    void request_stop();
    Snapshot snapshot() const;

private:
    void join_worker();

    mutable std::mutex mutex_;
    std::string ip_;
    unsigned short port_ = 0;
    bool has_result_ = false;
    ServerStatus status_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::thread worker_;
};

// Accepts "1.2.3.4" or "1.2.3.4:25566"; returns false when the address is not a
// numeric IPv4 address with an optional valid port.
bool parse_server_endpoint(const std::string& text, std::string& ip, unsigned short& port);

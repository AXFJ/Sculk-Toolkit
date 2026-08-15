#pragma once

#include "scan.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A discovered world plus the liveness bookkeeping needed by continuous scans.
struct LanWorldRecord {
    LanWorld world;
    std::chrono::system_clock::time_point first_seen{};
    std::chrono::system_clock::time_point last_seen{};
    std::uint64_t advertisements = 0;
    bool alive = true;
};

// Counts every advertisement received, so the total is not deduplicated.
struct LanScanStats {
    std::uint64_t received_advertisements = 0;
    std::size_t listed_worlds = 0;
    std::size_t dead_worlds = 0;
    // Status-query progress: worlds that already carry a result, and the queue
    // still waiting for one.
    std::size_t queried_worlds = 0;
    std::size_t pending_queries = 0;
};

enum class LanEventType {
    Started,
    WorldFound,
    WorldLost,
    WorldReturned,
    Stopped,
    Failed
};

struct LanScanEvent {
    LanEventType type = LanEventType::Started;
    std::chrono::system_clock::time_point at{};
    std::string ip;
    unsigned short port = 0;
    std::string detail;
};

enum class LanScanMode {
    UdpOnly,
    UdpAndStatusQuery
};

struct LanMonitorOptions {
    // std::nullopt keeps scanning until stopped.
    std::optional<std::chrono::seconds> duration;
    LanScanMode mode = LanScanMode::UdpAndStatusQuery;
    // A world that stops advertising for this long is reported as dead; it stays
    // in the list so continuous scans can show it returning later.
    std::chrono::seconds liveness_timeout{12};
};

// Runs the LAN listener on its own thread and maintains deduplicated world
// records, liveness state, counters, and an in-memory event log. The log is
// never written to disk.
class LanMonitor {
public:
    struct Snapshot {
        std::vector<LanWorldRecord> worlds;
        LanScanStats stats;
        std::string error;
        bool running = false;
        bool continuous = false;
        int remaining_seconds = 0;
        int elapsed_seconds = 0;
        // Seconds until the next automatic re-query, or 0 when disabled.
        int next_auto_query_seconds = 0;
    };

    ~LanMonitor();

    void start(const LanMonitorOptions& options);
    void request_stop();

    // Queues a server-list-ping for every listed world. Only has an effect while
    // a scan with status queries is running.
    void refresh_status_queries();
    // 0 disables the periodic refresh.
    void set_auto_query_interval(int seconds);

    bool running() const;
    Snapshot snapshot() const;
    std::vector<LanScanEvent> log() const;
    void clear_log();

private:
    void join_worker();
    void run(LanMonitorOptions options);
    void run_status_queries();
    void record_event(LanScanEvent event);
    void expire_worlds(std::chrono::steady_clock::time_point now,
                       std::chrono::seconds timeout);

    struct Entry {
        LanWorldRecord record;
        std::chrono::steady_clock::time_point last_seen_steady{};
    };

    static constexpr std::size_t log_capacity = 4096;

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::deque<LanScanEvent> log_;
    // IP:port pairs waiting for a server-list-ping query.
    std::deque<std::pair<std::string, unsigned short>> pending_queries_;
    LanScanStats stats_;
    std::string error_;
    bool continuous_ = false;
    int duration_seconds_ = 0;
    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point last_auto_query_{};
    bool status_queries_enabled_ = false;
    std::atomic<int> auto_query_seconds_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::thread worker_;
    std::thread query_worker_;
};

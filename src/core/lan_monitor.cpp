#include "lan_monitor.h"

#include "lan_listener.h"

#include <algorithm>
#include <utility>

namespace {

std::string world_key(const std::string& ip, unsigned short port) {
    return ip + ':' + std::to_string(port);
}

} // namespace

LanMonitor::~LanMonitor() {
    request_stop();
    join_worker();
}

void LanMonitor::start(const LanMonitorOptions& options) {
    request_stop();
    join_worker();

    {
        std::lock_guard lock(mutex_);
        entries_.clear();
        log_.clear();
        pending_queries_.clear();
        stats_ = LanScanStats{};
        error_.clear();
        last_auto_query_ = std::chrono::steady_clock::now();
        continuous_ = !options.duration.has_value();
        status_queries_enabled_ = options.mode == LanScanMode::UdpAndStatusQuery;
        duration_seconds_ = options.duration
            ? static_cast<int>(options.duration->count()) : 0;
        started_at_ = std::chrono::steady_clock::now();
    }
    cancelled_ = false;
    running_ = true;
    worker_ = std::thread([this, options] { run(options); });
    if (options.mode == LanScanMode::UdpAndStatusQuery) {
        query_worker_ = std::thread([this] { run_status_queries(); });
    }
}

void LanMonitor::request_stop() {
    cancelled_ = true;
}

void LanMonitor::refresh_status_queries() {
    std::lock_guard lock(mutex_);
    if (!status_queries_enabled_) {
        return;
    }
    for (const Entry& entry : entries_) {
        pending_queries_.emplace_back(entry.record.world.ip, entry.record.world.port);
    }
}

void LanMonitor::set_auto_query_interval(int seconds) {
    auto_query_seconds_ = seconds < 0 ? 0 : seconds;
}

void LanMonitor::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
    if (query_worker_.joinable()) {
        query_worker_.join();
    }
    running_ = false;
}

// Queries run on their own thread so a slow or unreachable server never delays
// the multicast listener.
void LanMonitor::run_status_queries() {
    {
        std::lock_guard lock(mutex_);
        last_auto_query_ = std::chrono::steady_clock::now();
    }
    while (true) {
        // Periodic re-query of the whole list, when enabled.
        const int auto_seconds = auto_query_seconds_.load();
        bool due = false;
        {
            std::lock_guard lock(mutex_);
            due = auto_seconds > 0 &&
                  std::chrono::steady_clock::now() - last_auto_query_ >=
                      std::chrono::seconds(auto_seconds);
            if (due) {
                last_auto_query_ = std::chrono::steady_clock::now();
            }
        }
        if (due) {
            refresh_status_queries();
        }

        std::pair<std::string, unsigned short> target;
        {
            std::lock_guard lock(mutex_);
            if (!pending_queries_.empty()) {
                target = pending_queries_.front();
                pending_queries_.pop_front();
            }
        }

        if (target.first.empty()) {
            if (cancelled_.load() || !running_.load()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        ServerStatus status = query_server_status(
            target.first, target.second, [this] { return cancelled_.load(); }, false);

        std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            entries_.begin(), entries_.end(), [&target](const Entry& entry) {
                return entry.record.world.ip == target.first &&
                       entry.record.world.port == target.second;
            });
        if (found != entries_.end()) {
            found->record.world.status = std::move(status);
        }
    }
}

bool LanMonitor::running() const {
    return running_.load();
}

void LanMonitor::record_event(LanScanEvent event) {
    event.at = std::chrono::system_clock::now();
    log_.push_back(std::move(event));
    if (log_.size() > log_capacity) {
        log_.pop_front();
    }
}

void LanMonitor::expire_worlds(std::chrono::steady_clock::time_point now,
                               std::chrono::seconds timeout) {
    for (Entry& entry : entries_) {
        if (!entry.record.alive || now - entry.last_seen_steady < timeout) {
            continue;
        }
        entry.record.alive = false;
        ++stats_.dead_worlds;
        record_event(LanScanEvent{LanEventType::WorldLost, {},
                                  entry.record.world.ip, entry.record.world.port,
                                  entry.record.world.lan_motd});
    }
}

void LanMonitor::run(LanMonitorOptions options) {
    const auto deadline = options.duration
        ? std::optional{std::chrono::steady_clock::now() + *options.duration}
        : std::nullopt;

    {
        std::lock_guard lock(mutex_);
        record_event(LanScanEvent{LanEventType::Started, {}, {}, 0,
                                  continuous_ ? std::string{} :
                                      std::to_string(duration_seconds_)});
    }

    const ListenResult result = listen_lan_advertisements(
        deadline,
        [this](const LanAdvertisement& advertisement) {
            std::lock_guard lock(mutex_);
            ++stats_.received_advertisements;

            const std::string key = world_key(advertisement.ip, advertisement.port);
            const auto now_steady = std::chrono::steady_clock::now();
            const auto now_system = std::chrono::system_clock::now();
            const auto found = std::find_if(
                entries_.begin(), entries_.end(), [&key](const Entry& entry) {
                    return world_key(entry.record.world.ip, entry.record.world.port) == key;
                });

            if (found == entries_.end()) {
                Entry entry;
                entry.record.world = LanWorld{advertisement.ip, advertisement.port,
                                              advertisement.lan_motd, std::nullopt};
                entry.record.first_seen = now_system;
                entry.record.last_seen = now_system;
                entry.record.advertisements = 1;
                entry.last_seen_steady = now_steady;
                entries_.push_back(std::move(entry));
                stats_.listed_worlds = entries_.size();
                if (status_queries_enabled_) {
                    pending_queries_.emplace_back(advertisement.ip, advertisement.port);
                }
                record_event(LanScanEvent{LanEventType::WorldFound, {}, advertisement.ip,
                                          advertisement.port, advertisement.lan_motd});
                return;
            }

            ++found->record.advertisements;
            found->record.last_seen = now_system;
            found->last_seen_steady = now_steady;

            if (!found->record.alive) {
                found->record.alive = true;
                stats_.dead_worlds = stats_.dead_worlds > 0 ? stats_.dead_worlds - 1 : 0;
                record_event(LanScanEvent{LanEventType::WorldReturned, {}, advertisement.ip,
                                          advertisement.port, advertisement.lan_motd});
            }
            // MOTD rotations are tracked but deliberately not logged.
            found->record.world.lan_motd = advertisement.lan_motd;
        },
        [this, timeout = options.liveness_timeout] {
            std::lock_guard lock(mutex_);
            expire_worlds(std::chrono::steady_clock::now(), timeout);
        },
        [this] { return cancelled_.load(); });

    {
        std::lock_guard lock(mutex_);
        error_ = result.error;
        record_event(result.error.empty()
            ? LanScanEvent{LanEventType::Stopped, {}, {}, 0,
                           std::to_string(entries_.size())}
            : LanScanEvent{LanEventType::Failed, {}, {}, 0, result.error});
    }
    running_ = false;
}

LanMonitor::Snapshot LanMonitor::snapshot() const {
    Snapshot snapshot;
    snapshot.running = running_.load();

    std::lock_guard lock(mutex_);
    snapshot.worlds.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        snapshot.worlds.push_back(entry.record);
    }
    snapshot.stats = stats_;
    snapshot.stats.listed_worlds = entries_.size();
    snapshot.stats.dead_worlds = static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(),
        [](const Entry& entry) { return !entry.record.alive; }));
    snapshot.stats.queried_worlds = static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(),
        [](const Entry& entry) { return entry.record.world.status.has_value(); }));
    snapshot.stats.pending_queries = pending_queries_.size();
    snapshot.error = error_;
    snapshot.continuous = continuous_;

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_at_).count();
    snapshot.elapsed_seconds = static_cast<int>(elapsed);
    if (snapshot.running && !continuous_) {
        snapshot.remaining_seconds = std::max(
            0, duration_seconds_ - static_cast<int>(elapsed));
    }
    const int auto_seconds = auto_query_seconds_.load();
    if (snapshot.running && status_queries_enabled_ && auto_seconds > 0) {
        const auto since_pass = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - last_auto_query_).count();
        snapshot.next_auto_query_seconds = std::max(
            0, auto_seconds - static_cast<int>(since_pass));
    }
    return snapshot;
}

std::vector<LanScanEvent> LanMonitor::log() const {
    std::lock_guard lock(mutex_);
    return {log_.begin(), log_.end()};
}

void LanMonitor::clear_log() {
    std::lock_guard lock(mutex_);
    log_.clear();
}

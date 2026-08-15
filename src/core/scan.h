#pragma once

#include "status_query.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct LanWorld {
    std::string ip;
    unsigned short port = 0;
    std::string lan_motd;
    std::optional<ServerStatus> status;
};

struct ScanReport {
    std::vector<LanWorld> worlds;
    std::string error;
    bool cancelled = false;
};

using WorldFoundHandler = std::function<void(const LanWorld&)>;
using ScanCancelHandler = std::function<bool()>;
using ScanProgressHandler = std::function<void(double)>;

ScanReport scan_lan_worlds(std::chrono::seconds duration,
                           const WorldFoundHandler& on_world_found,
                           const ScanCancelHandler& cancelled,
                           const ScanProgressHandler& on_progress);

std::filesystem::path export_scan_results(const std::vector<LanWorld>& worlds,
                                          std::string& error);

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

// Raw Minecraft LAN advertisement, exactly as received. No deduplication is
// applied here so callers can count total traffic and track liveness.
struct LanAdvertisement {
    std::string ip;
    unsigned short port = 0;
    std::string lan_motd;
};

using AdvertisementHandler = std::function<void(const LanAdvertisement&)>;
using ListenTickHandler = std::function<void()>;
using ListenCancelHandler = std::function<bool()>;

struct ListenResult {
    std::string error;
    bool cancelled = false;
};

// Joins the Minecraft LAN multicast group and reports every advertisement until
// the deadline passes (std::nullopt listens indefinitely) or cancelled() returns
// true. on_tick fires roughly five times per second so callers can update
// progress or expire worlds that stopped broadcasting.
ListenResult listen_lan_advertisements(
    std::optional<std::chrono::steady_clock::time_point> deadline,
    const AdvertisementHandler& on_advertisement,
    const ListenTickHandler& on_tick,
    const ListenCancelHandler& cancelled);

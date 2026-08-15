#pragma once

#include <functional>
#include <string>
#include <vector>

struct ServerStatus {
    bool available = false;
    std::string motd;
    std::string version_name;
    int protocol_version = 0;
    int online_players = 0;
    int max_players = 0;
    std::vector<std::string> player_names;
    // Same order as player_names; empty when the server omits the player id.
    std::vector<std::string> player_ids;
    std::string favicon;
    // enforcesSecureChat is optional in the status response.
    bool secure_chat_known = false;
    bool enforces_secure_chat = false;
    // Round-trip time of the status request, or -1 when it was not measured.
    int latency_ms = -1;
    std::string error;
};

ServerStatus query_server_status(const std::string& ip,
                                 unsigned short port,
                                 const std::function<bool()>& cancelled,
                                 bool include_favicon = false);

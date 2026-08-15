#include "scan.h"

#include "fake_server.h"
#include "lan_listener.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace {

std::string json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': escaped << "\\\""; break;
            case '\\': escaped << "\\\\"; break;
            case '\b': escaped << "\\b"; break;
            case '\f': escaped << "\\f"; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default:
                if (character < 0x20) {
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(character) << std::dec;
                } else {
                    escaped << static_cast<char>(character);
                }
        }
    }
    return escaped.str();
}

} // namespace

ScanReport scan_lan_worlds(std::chrono::seconds duration,
                           const WorldFoundHandler& on_world_found,
                           const ScanCancelHandler& cancelled,
                           const ScanProgressHandler& on_progress) {
    ScanReport report;

    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = started_at + duration;
    const double total = std::max(0.001, std::chrono::duration<double>(duration).count());
    std::set<std::string> discovered;
    on_progress(0.0);

    const ListenResult result = listen_lan_advertisements(
        deadline,
        [&](const LanAdvertisement& advertisement) {
            LanWorld world{advertisement.ip, advertisement.port, advertisement.lan_motd,
                           std::nullopt};
            const std::string key = world.ip + ':' + std::to_string(world.port);
            if (discovered.insert(key).second) {
                report.worlds.push_back(world);
                on_world_found(world);
            }
        },
        [&] {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started_at).count();
            on_progress(std::clamp(elapsed / total, 0.0, 1.0));
        },
        cancelled);

    report.error = result.error;
    report.cancelled = result.cancelled;
    if (report.error.empty() && !report.cancelled) {
        on_progress(1.0);
    }
    return report;
}

std::filesystem::path export_scan_results(const std::vector<LanWorld>& worlds,
                                          std::string& error) {
    static std::mutex export_mutex;
    std::lock_guard lock(export_mutex);

    std::error_code filesystem_error;
    const std::filesystem::path export_directory = "export";
    std::filesystem::create_directories(export_directory, filesystem_error);
    if (filesystem_error) {
        error = "无法创建 export 目录: " + filesystem_error.message();
        return {};
    }

    std::filesystem::path output_path = export_directory / "scan.json";
    for (int suffix = 2; std::filesystem::exists(output_path); ++suffix) {
        output_path = export_directory / ("scan" + std::to_string(suffix) + ".json");
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::out);
    if (!output) {
        error = "无法创建 " + output_path.generic_string();
        return {};
    }

    output << "{\n  \"servers\": [";
    for (std::size_t index = 0; index < worlds.size(); ++index) {
        const auto& world = worlds[index];
        FakeServerConfig config;
        config.mode = world.status && world.status->available
            ? FakeServerMode::Both : FakeServerMode::Udp;
        config.lan_port = world.port;
        config.lan_motd = world.lan_motd;
        config.port = world.port;
        if (world.status && world.status->available) {
            config.motd = world.status->motd;
            config.version = world.status->version_name;
            config.protocol = world.status->protocol_version;
            config.online_players = world.status->online_players;
            config.max_players = world.status->max_players;
            config.players = world.status->player_names;
        }
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"mode\": \"" << fake_server_mode_text(config.mode)
               << "\", \"ip\": \"" << json_escape(world.ip)
               << "\", \"lanPort\": " << config.lan_port
               << ", \"lanMotd\": \"" << json_escape(config.lan_motd)
               << "\", \"port\": " << config.port
               << ", \"motd\": \"" << json_escape(config.motd)
               << "\", \"version\": \"" << json_escape(config.version)
               << "\", \"max\": " << config.max_players
               << ", \"online\": " << config.online_players
               << ", \"players\": [";
        for (std::size_t player_index = 0; player_index < config.players.size(); ++player_index) {
            if (player_index != 0) output << ", ";
            const std::string uuid =
                world.status && player_index < world.status->player_ids.size()
                    ? world.status->player_ids[player_index] : std::string{};
            output << "{\"name\":\"" << json_escape(config.players[player_index])
                   << "\", \"uuid\":\"" << json_escape(uuid) << "\"}";
        }
        output << "], \"favicon\": \"" << json_escape(config.favicon)
               << "\", \"protocol\": " << config.protocol
               << ", \"kickmsg\": \"" << json_escape(config.kick_message) << "\"}";
    }
    if (!worlds.empty()) {
        output << '\n';
    }
    output << "  ]\n}\n";

    if (!output) {
        error = "写入 " + output_path.generic_string() + " 失败";
        return {};
    }
    return output_path;
}

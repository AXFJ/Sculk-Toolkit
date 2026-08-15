#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class FakeServerMode {
    Udp,
    Tcp,
    Both
};

// enforcesSecureChat is optional in the status response, so it can also be
// omitted entirely.
enum class SecureChatSetting {
    Absent,
    Yes,
    No
};

struct FakeServerConfig {
    FakeServerMode mode = FakeServerMode::Both;
    std::uint16_t lan_port = 25565;
    std::string lan_motd = "A Fake LAN World";
    std::uint16_t port = 25565;
    std::string motd = "A Minecraft Server";
    std::string version = "Fake Server";
    int max_players = 20;
    int online_players = 0;
    std::vector<std::string> players;
    // Same order as players; an empty entry means "let the server invent one".
    std::vector<std::string> player_uuids;
    std::string favicon;
    int protocol = 0;
    std::string kick_message = "This is a fake server.";
    SecureChatSetting secure_chat = SecureChatSetting::Absent;
    // Broadcasts that share a group are collapsed into one list row and share a
    // connection log. Empty means "no group".
    std::string group;
    bool random_player_list_order = false;
};

struct FakeServerSnapshot {
    int id = 0;
    bool running = false;
    FakeServerConfig config;
};

// One connection event of a fake server: a status ping or a login attempt.
struct FakeServerLogEntry {
    std::chrono::system_clock::time_point at{};
    int server_id = 0;
    std::string event;   // "slp" or "login"
    std::string ip;
    std::string player;  // empty for slp events
};

class FakeServerManager {
public:
    using LogHandler = std::function<void(const std::string& event,
                       const std::string& ip,
                       const std::string& player)>;

    FakeServerManager();
    ~FakeServerManager();

    int create(FakeServerConfig config);
    bool start(int id, const LogHandler& log_handler, std::string& error);
    bool stop(int id, std::string& error);
    bool remove(int id, std::string& error);
    bool modify(int id, const std::string& attribute,
                const std::string& value, std::string& error);

    // Replaces a server's configuration. A running server picks the new values
    // up without a restart unless the mode or a port changed, in which case it
    // is restarted with the sockets it needs.
    bool update(int id, FakeServerConfig config, std::string& error);

    std::vector<FakeServerSnapshot> list() const;
    std::vector<int> ids() const;
    bool is_running(int id) const;

    // Connection log of one server, kept in memory only. TCP-less servers never
    // produce entries.
    std::vector<FakeServerLogEntry> log(int id) const;
    void clear_log(int id);

    // Writes a preset file under export/. Empty ids exports every server, and a
    // non-empty group overrides the group of every exported entry. Ids are never
    // written, so imports always allocate fresh ones.
    // An empty path writes an auto-numbered file under export/.
    std::filesystem::path export_servers(const std::vector<int>& ids,
                                         const std::string& group,
                                         const std::filesystem::path& path,
                                         std::string& error) const;
    std::filesystem::path export_servers(const std::vector<int>& ids,
                                         const std::string& group,
                                         std::string& error) const;
    std::filesystem::path export_all(std::string& error) const;

    // A non-empty group overrides the group of every imported entry.
    std::vector<int> import_file(const std::filesystem::path& path,
                                 const std::string& group,
                                 std::string& error);
    std::vector<int> import_file(const std::filesystem::path& path, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool parse_fake_server_mode(const std::string& text, FakeServerMode& mode);
std::string fake_server_mode_text(FakeServerMode mode);
bool apply_fake_server_attribute(FakeServerConfig& config,
                                 const std::string& attribute,
                                 const std::string& value,
                                 std::string& error);
bool apply_fake_server_attributes_json(FakeServerConfig& config,
                                       const std::string& json,
                                       std::string& error);

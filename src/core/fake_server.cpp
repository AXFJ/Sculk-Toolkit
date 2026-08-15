#include "fake_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle invalid_socket = -1;
#endif

namespace {

constexpr const char* multicast_address = "224.0.2.60";
constexpr std::uint16_t multicast_port = 4445;

void close_socket(SocketHandle socket) {
    if (socket == invalid_socket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

int socket_error_code() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string socket_error_text(const std::string& prefix) {
    return prefix + " (错误 " + std::to_string(socket_error_code()) + ')';
}

bool set_nonblocking(SocketHandle socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool set_blocking(SocketHandle socket) {
#ifdef _WIN32
    u_long enabled = 0;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

bool uses_udp(FakeServerMode mode) {
    return mode == FakeServerMode::Udp || mode == FakeServerMode::Both;
}

bool uses_tcp(FakeServerMode mode) {
    return mode == FakeServerMode::Tcp || mode == FakeServerMode::Both;
}

std::string trim(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

bool parse_integer(const std::string& text, int minimum, int maximum,
                   int& value, std::string& error) {
    const auto [position, parse_error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (parse_error != std::errc{} || position != text.data() + text.size() ||
        value < minimum || value > maximum) {
        error = "数值必须位于 " + std::to_string(minimum) + " 到 " +
                std::to_string(maximum) + " 之间";
        return false;
    }
    return true;
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    output << "\\u00";
                    constexpr char hex[] = "0123456789abcdef";
                    output << hex[(character >> 4) & 0x0F] << hex[character & 0x0F];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < data.size(); index += 3) {
        const std::uint32_t first = data[index];
        const std::uint32_t second = index + 1 < data.size() ? data[index + 1] : 0;
        const std::uint32_t third = index + 2 < data.size() ? data[index + 2] : 0;
        const std::uint32_t value = (first << 16) | (second << 8) | third;
        output.push_back(alphabet[(value >> 18) & 0x3F]);
        output.push_back(alphabet[(value >> 12) & 0x3F]);
        output.push_back(index + 1 < data.size() ? alphabet[(value >> 6) & 0x3F] : '=');
        output.push_back(index + 2 < data.size() ? alphabet[value & 0x3F] : '=');
    }
    return output;
}

bool load_favicon_data_uri(const std::string& value,
                           std::string& data_uri,
                           std::string& error) {
    data_uri.clear();
    if (value.empty()) return true;
    if (value.starts_with("data:image/png;base64,")) {
        data_uri = value;
        return true;
    }

    std::ifstream input(value, std::ios::binary);
    if (!input) {
        error = "\u65e0\u6cd5\u8bfb\u53d6 favicon \u56fe\u7247: " + value;
        return false;
    }
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    static constexpr std::array<std::uint8_t, 8> png_signature{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < png_signature.size() ||
        !std::equal(png_signature.begin(), png_signature.end(), bytes.begin())) {
        error = "favicon \u5fc5\u987b\u662f PNG \u56fe\u7247: " + value;
        return false;
    }
    const auto read_u32 = [&bytes](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
               static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    if (bytes.size() < 24 || read_u32(16) != 64 || read_u32(20) != 64) {
        error = "favicon PNG \u5c3a\u5bf8\u5fc5\u987b\u662f 64x64: " + value;
        return false;
    }
    data_uri = "data:image/png;base64," + base64_encode(bytes);
    return true;
}

void append_varint(std::vector<std::uint8_t>& output, std::int32_t value) {
    std::uint32_t remaining = static_cast<std::uint32_t>(value);
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(remaining & 0x7F);
        remaining >>= 7;
        if (remaining != 0) byte |= 0x80;
        output.push_back(byte);
    } while (remaining != 0);
}

bool receive_exact(SocketHandle socket, std::uint8_t* output, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const int result = recv(socket, reinterpret_cast<char*>(output + received),
                                static_cast<int>(size - received), 0);
        if (result <= 0) return false;
        received += static_cast<std::size_t>(result);
    }
    return true;
}

bool receive_varint(SocketHandle socket, std::int32_t& value) {
    value = 0;
    for (int index = 0; index < 5; ++index) {
        std::uint8_t byte = 0;
        if (!receive_exact(socket, &byte, 1)) return false;
        value |= static_cast<std::int32_t>(byte & 0x7F) << (7 * index);
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

bool receive_packet(SocketHandle socket, std::vector<std::uint8_t>& packet) {
    std::int32_t length = 0;
    if (!receive_varint(socket, length) || length <= 0 || length > 1024 * 1024) return false;
    packet.resize(static_cast<std::size_t>(length));
    return receive_exact(socket, packet.data(), packet.size());
}

bool read_varint(const std::vector<std::uint8_t>& packet,
                 std::size_t& offset,
                 std::int32_t& value) {
    value = 0;
    for (int index = 0; index < 5 && offset < packet.size(); ++index) {
        const std::uint8_t byte = packet[offset++];
        value |= static_cast<std::int32_t>(byte & 0x7F) << (7 * index);
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

bool read_string(const std::vector<std::uint8_t>& packet,
                 std::size_t& offset,
                 std::string& value) {
    std::int32_t length = 0;
    if (!read_varint(packet, offset, length) || length < 0 ||
        static_cast<std::size_t>(length) > packet.size() - offset) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(packet.data() + offset),
                 static_cast<std::size_t>(length));
    offset += static_cast<std::size_t>(length);
    return true;
}

bool send_all(SocketHandle socket, const std::vector<std::uint8_t>& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int result = send(socket,
                                reinterpret_cast<const char*>(data.data() + sent),
                                static_cast<int>(data.size() - sent), 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

std::string build_status_json(const FakeServerConfig& config) {
    std::ostringstream output;
    output << "{\"version\":{\"name\":\"" << json_escape(config.version)
           << "\",\"protocol\":" << config.protocol << "},"
           << "\"players\":{\"max\":" << config.max_players
           << ",\"online\":" << config.online_players << ",\"sample\":[";

    // Build an index vector; shuffle it when the config asks for random order.
    std::vector<std::size_t> order(config.players.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (config.random_player_list_order && order.size() > 1) {
        static std::mt19937 rng(std::random_device{}());
        std::shuffle(order.begin(), order.end(), rng);
    }

    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::size_t index = order[i];
        if (i != 0) output << ',';
        output << "{\"name\":\"" << json_escape(config.players[index]) << "\",\"id\":\"";
        if (index < config.player_uuids.size() && !config.player_uuids[index].empty()) {
            output << json_escape(config.player_uuids[index]);
        } else {
            output << "00000000-0000-0000-0000-"
                   << std::setfill('0') << std::setw(12) << index + 1;
        }
        output << "\"}";
    }
    output << "]},\"description\":\"" << json_escape(config.motd) << '"';
    if (config.secure_chat != SecureChatSetting::Absent) {
        output << ",\"enforcesSecureChat\":"
               << (config.secure_chat == SecureChatSetting::Yes ? "true" : "false");
    }
    std::string favicon;
    std::string favicon_error;
    load_favicon_data_uri(config.favicon, favicon, favicon_error);
    if (!favicon.empty()) output << ",\"favicon\":\"" << json_escape(favicon) << '"';
    output << '}';
    return output.str();
}

bool send_json_packet(SocketHandle client, const std::string& json) {
    std::vector<std::uint8_t> payload;
    append_varint(payload, 0);
    append_varint(payload, static_cast<std::int32_t>(json.size()));
    payload.insert(payload.end(), json.begin(), json.end());
    std::vector<std::uint8_t> packet;
    append_varint(packet, static_cast<std::int32_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return send_all(client, packet);
}

std::string client_ip(const sockaddr_in& address) {
    std::array<char, INET_ADDRSTRLEN> buffer{};
    return inet_ntop(AF_INET, &address.sin_addr, buffer.data(), buffer.size())
        ? std::string(buffer.data()) : "unknown";
}

void handle_client(SocketHandle client,
                   const sockaddr_in& client_address,
                   const FakeServerConfig& config,
                   const FakeServerManager::LogHandler& log_handler) {
#ifdef _WIN32
    DWORD timeout = 1500;
#else
    timeval timeout{1, 500000};
#endif
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::vector<std::uint8_t> handshake;
    if (!receive_packet(client, handshake)) return;
    std::size_t offset = 0;
    std::int32_t packet_id = 0;
    std::int32_t protocol = 0;
    std::int32_t next_state = 0;
    std::string server_address;
    if (!read_varint(handshake, offset, packet_id) || packet_id != 0 ||
        !read_varint(handshake, offset, protocol) ||
        !read_string(handshake, offset, server_address) || offset + 2 > handshake.size()) {
        return;
    }
    offset += 2;
    if (!read_varint(handshake, offset, next_state)) return;

    const std::string ip = client_ip(client_address);
    if (next_state == 1) {
        if (log_handler) log_handler("slp", ip, "");
        std::vector<std::uint8_t> request;
        if (!receive_packet(client, request)) return;
        send_json_packet(client, build_status_json(config));
        return;
    }

    if (next_state == 2) {
        std::vector<std::uint8_t> login_start;
        if (!receive_packet(client, login_start)) return;
        std::size_t login_offset = 0;
        std::int32_t login_packet_id = 0;
        std::string player_name;
        if (!read_varint(login_start, login_offset, login_packet_id) || login_packet_id != 0 ||
            !read_string(login_start, login_offset, player_name)) {
            return;
        }
        if (log_handler) {
            log_handler("login", ip, player_name);
        }
        send_json_packet(client, "{\"text\":\"" + json_escape(config.kick_message) + "\"}");
    }
}

SocketHandle create_udp_socket(std::string& error) {
    const SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) {
        error = socket_error_text("无法创建 UDP 广播套接字");
        return invalid_socket;
    }
    int ttl = 1;
    setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    return socket;
}

SocketHandle create_tcp_listener(std::uint16_t port, std::string& error) {
    const SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == invalid_socket) {
        error = socket_error_text("无法创建 TCP 监听套接字");
        return invalid_socket;
    }
    int reuse = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(socket, 16) != 0 || !set_nonblocking(socket)) {
        error = socket_error_text("无法监听 TCP 端口 " + std::to_string(port));
        close_socket(socket);
        return invalid_socket;
    }
    return socket;
}

// The worker re-reads this every iteration, so edits to a running server take
// effect on the next broadcast or connection.
struct LiveConfig {
    mutable std::mutex mutex;
    FakeServerConfig config;

    FakeServerConfig snapshot() const {
        std::lock_guard lock(mutex);
        return config;
    }

    void assign(const FakeServerConfig& value) {
        std::lock_guard lock(mutex);
        config = value;
    }
};

// Connection events are kept per server so the GUI can show them per broadcast.
struct LiveLog {
    static constexpr std::size_t capacity = 1000;

    mutable std::mutex mutex;
    std::deque<FakeServerLogEntry> entries;

    void append(std::string event, std::string ip,
                std::string player, int server_id = 0) {
        std::lock_guard lock(mutex);
        entries.push_back(FakeServerLogEntry{std::chrono::system_clock::now(),
                                            server_id,
                                            std::move(event),
                                            std::move(ip),
                                            std::move(player)});
        if (entries.size() > capacity) {
            entries.pop_front();
        }
    }

    std::vector<FakeServerLogEntry> snapshot() const {
        std::lock_guard lock(mutex);
        return {entries.begin(), entries.end()};
    }

    void clear() {
        std::lock_guard lock(mutex);
        entries.clear();
    }
};

// Minecraft clients expect a LAN advertisement roughly every 1.5 seconds.
constexpr auto lan_broadcast_interval = std::chrono::milliseconds(1500);

void run_fake_server(std::stop_token stop_token,
                     std::shared_ptr<LiveConfig> live,
                     SocketHandle udp_socket,
                     SocketHandle tcp_socket,
                     FakeServerManager::LogHandler log_handler) {
    sockaddr_in multicast{};
    multicast.sin_family = AF_INET;
    multicast.sin_port = htons(multicast_port);
    inet_pton(AF_INET, multicast_address, &multicast.sin_addr);
    auto next_broadcast = std::chrono::steady_clock::now();

    while (!stop_token.stop_requested()) {
        const FakeServerConfig config = live->snapshot();
        const auto now = std::chrono::steady_clock::now();
        if (udp_socket != invalid_socket && now >= next_broadcast) {
            const std::string message = "[MOTD]" + config.lan_motd +
                "[/MOTD][AD]" + std::to_string(config.lan_port) + "[/AD]";
            sendto(udp_socket, message.data(), static_cast<int>(message.size()), 0,
                   reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast));
            next_broadcast = now + lan_broadcast_interval;
        }

        if (tcp_socket != invalid_socket) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(tcp_socket, &read_set);
            timeval wait{0, 100000};
#ifdef _WIN32
            const int selected = select(0, &read_set, nullptr, nullptr, &wait);
#else
            const int selected = select(tcp_socket + 1, &read_set, nullptr, nullptr, &wait);
#endif
            if (selected > 0) {
                sockaddr_in client_address{};
#ifdef _WIN32
                int client_size = sizeof(client_address);
#else
                socklen_t client_size = sizeof(client_address);
#endif
                const SocketHandle client = accept(
                    tcp_socket, reinterpret_cast<sockaddr*>(&client_address), &client_size);
                if (client != invalid_socket) {
                    if (set_blocking(client)) {
                        handle_client(client, client_address, config, log_handler);
                    }
                    close_socket(client);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    close_socket(tcp_socket);
    close_socket(udp_socket);
}

struct FakeServerRecord {
    int id = 0;
    FakeServerConfig config;
    bool running = false;
    bool starting = false;
    std::shared_ptr<LiveConfig> live;
    std::shared_ptr<LiveLog> live_log;
    FakeServerManager::LogHandler log_handler;
    std::jthread worker;
};

std::string normalize_attribute(std::string attribute) {
    if (attribute.starts_with("--")) attribute.erase(0, 2);
    if (attribute == "ver") return "version";
    if (attribute == "mp") return "max";
    if (attribute == "ol") return "online";
    if (attribute == "fav") return "favicon";
    if (attribute == "pr") return "protocol";
    if (attribute == "km") return "kickmsg";
    return attribute;
}

bool attribute_allowed_for_mode(FakeServerMode mode, const std::string& attribute) {
    // The group is bookkeeping rather than protocol data, so every mode takes it.
    if (attribute == "group") return true;
    const bool udp_attribute = attribute == "lan-port" || attribute == "lan-motd";
    const bool tcp_attribute = attribute == "port" || attribute == "motd" ||
        attribute == "version" || attribute == "max" || attribute == "online" ||
        attribute == "players" || attribute == "favicon" || attribute == "protocol" ||
        attribute == "kickmsg" || attribute == "securechat";
    if (mode == FakeServerMode::Udp) return udp_attribute;
    if (mode == FakeServerMode::Tcp) return tcp_attribute;
    return udp_attribute || tcp_attribute;
}

class FakeServerJsonParser {
public:
    explicit FakeServerJsonParser(std::string_view input) : input_(input) {}

    bool parse(std::vector<FakeServerConfig>& configs, std::string& error) {
        skip_whitespace();
        if (!consume('{')) return fail(error, "JSON 根节点必须是对象");
        bool servers_found = false;
        while (true) {
            skip_whitespace();
            if (consume('}')) break;
            std::string key;
            if (!read_string(key, error) || !expect(':', error)) return false;
            if (key == "servers") {
                if (!read_servers(configs, error)) return false;
                servers_found = true;
            } else if (!skip_value(error)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) break;
            if (!expect(',', error)) return false;
        }
        if (!servers_found) return fail(error, "JSON 缺少 servers 数组");
        skip_whitespace();
        return position_ == input_.size() || fail(error, "JSON 末尾存在额外内容");
    }

    bool parse_attributes(FakeServerConfig& config, std::string& error) {
        skip_whitespace();
        if (!consume('{')) return fail(error, "--attributes 必须是 JSON 对象");
        skip_whitespace();
        if (consume('}')) {
            skip_whitespace();
            return position_ == input_.size() || fail(error, "JSON 末尾存在额外内容");
        }

        while (true) {
            std::string attribute;
            if (!read_string(attribute, error) || !expect(':', error)) return false;
            const std::string normalized = normalize_attribute(attribute);
            if (normalized == "mode") {
                return fail(error, "mode 必须由 fs new <mode> 指定");
            }
            if (!attribute_allowed_for_mode(config.mode, normalized)) {
                return fail(error, attribute + " 不适用于 " + fake_server_mode_text(config.mode) + " 模式");
            }

            std::string value;
            if (normalized == "players") {
                std::vector<std::string> players;
                if (!read_player_array(config, error)) return false;
                (void)players;
            } else if (normalized == "lan-port" || normalized == "port" ||
                       normalized == "max" || normalized == "online" ||
                       normalized == "protocol") {
                int number = 0;
                if (!read_integer(number, error)) return false;
                if (!apply_fake_server_attribute(config, normalized,
                                                 std::to_string(number), error)) {
                    return false;
                }
            } else {
                if (!read_string(value, error)) return false;
                if (!apply_fake_server_attribute(config, normalized, value, error)) return false;
            }

            skip_whitespace();
            if (consume('}')) break;
            if (!expect(',', error)) return false;
        }
        skip_whitespace();
        return position_ == input_.size() || fail(error, "JSON 末尾存在额外内容");
    }

private:
    bool read_servers(std::vector<FakeServerConfig>& configs, std::string& error) {
        if (!expect('[', error)) return false;
        skip_whitespace();
        if (consume(']')) return true;
        while (true) {
            FakeServerConfig config;
            if (!read_server(config, error)) return false;
            configs.push_back(std::move(config));
            skip_whitespace();
            if (consume(']')) return true;
            if (!expect(',', error)) return false;
        }
    }

    bool read_server(FakeServerConfig& config, std::string& error) {
        if (!expect('{', error)) return false;
        skip_whitespace();
        if (consume('}')) return true;
        while (true) {
            std::string key;
            if (!read_string(key, error) || !expect(':', error)) return false;
            if (key == "mode") {
                std::string value;
                if (!read_string(value, error) || !parse_fake_server_mode(value, config.mode)) {
                    return fail(error, "无效的假服务器 mode");
                }
            } else if (key == "lanPort") {
                int value = 0;
                if (!read_integer(value, error) || value < 1 || value > 65535) {
                    return fail(error, "lanPort 无效");
                }
                config.lan_port = static_cast<std::uint16_t>(value);
            } else if (key == "lanMotd") {
                if (!read_string(config.lan_motd, error)) return false;
            } else if (key == "port") {
                int value = 0;
                if (!read_integer(value, error) || value < 1 || value > 65535) {
                    return fail(error, "port 无效");
                }
                config.port = static_cast<std::uint16_t>(value);
            } else if (key == "motd") {
                if (!read_string(config.motd, error)) return false;
            } else if (key == "version") {
                if (!read_string(config.version, error)) return false;
            } else if (key == "max") {
                if (!read_integer(config.max_players, error)) return false;
            } else if (key == "online") {
                if (!read_integer(config.online_players, error)) return false;
            } else if (key == "players") {
                if (!read_player_array(config, error)) return false;
            } else if (key == "favicon") {
                if (!read_string(config.favicon, error)) return false;
            } else if (key == "protocol") {
                if (!read_integer(config.protocol, error)) return false;
            } else if (key == "kickmsg" || key == "kickMessage") {
                if (!read_string(config.kick_message, error)) return false;
            } else if (key == "group") {
                if (!read_string(config.group, error)) return false;
            } else if (key == "securechat" || key == "secureChat") {
                std::string value;
                if (!read_string(value, error) ||
                    !apply_fake_server_attribute(config, "securechat", value, error)) {
                    return false;
                }
            } else if (!skip_value(error)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) break;
            if (!expect(',', error)) return false;
        }
        if (!config.favicon.empty()) {
            std::string loaded_favicon;
            std::string favicon_error;
            if (!load_favicon_data_uri(config.favicon, loaded_favicon, favicon_error)) {
                return fail(error, favicon_error);
            }
            config.favicon = std::move(loaded_favicon);
        }
        if (config.mode == FakeServerMode::Both) config.lan_port = config.port;
        return true;
    }

    // Presets written by this tool use {"name":..., "uuid":...}; older files and
    // scan exports use plain name strings.
    bool read_player_array(FakeServerConfig& config, std::string& error) {
        if (!expect('[', error)) return false;
        config.players.clear();
        config.player_uuids.clear();
        skip_whitespace();
        if (consume(']')) return true;
        while (true) {
            skip_whitespace();
            if (position_ < input_.size() && input_[position_] == '{') {
                if (!expect('{', error)) return false;
                std::string name;
                std::string uuid;
                skip_whitespace();
                if (!consume('}')) {
                    while (true) {
                        std::string key;
                        if (!read_string(key, error) || !expect(':', error)) return false;
                        if (key == "name") {
                            if (!read_string(name, error)) return false;
                        } else if (key == "uuid" || key == "id") {
                            if (!read_string(uuid, error)) return false;
                        } else if (!skip_value(error)) {
                            return false;
                        }
                        skip_whitespace();
                        if (consume('}')) break;
                        if (!expect(',', error)) return false;
                    }
                }
                config.players.push_back(std::move(name));
                config.player_uuids.push_back(std::move(uuid));
            } else {
                std::string name;
                if (!read_string(name, error)) return false;
                config.players.push_back(std::move(name));
                config.player_uuids.emplace_back();
            }
            skip_whitespace();
            if (consume(']')) return true;
            if (!expect(',', error)) return false;
        }
    }

    bool read_string_array(std::vector<std::string>& values, std::string& error) {
        if (!expect('[', error)) return false;
        skip_whitespace();
        if (consume(']')) return true;
        while (true) {
            std::string value;
            if (!read_string(value, error)) return false;
            values.push_back(std::move(value));
            skip_whitespace();
            if (consume(']')) return true;
            if (!expect(',', error)) return false;
        }
    }

    bool read_string(std::string& output, std::string& error) {
        skip_whitespace();
        if (!consume('"')) return fail(error, "JSON 字符串缺少引号");
        output.clear();
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return true;
            if (character != '\\') {
                output.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) return fail(error, "JSON 转义不完整");
            switch (input_[position_++]) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default: return fail(error, "JSON 包含不支持的转义");
            }
        }
        return fail(error, "JSON 字符串未结束");
    }

    bool read_integer(int& value, std::string& error) {
        skip_whitespace();
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        const std::string_view text = input_.substr(start, position_ - start);
        const auto [end, parse_error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parse_error != std::errc{} || end != text.data() + text.size()) {
            return fail(error, "JSON 整数无效");
        }
        return true;
    }

    bool skip_value(std::string& error) {
        skip_whitespace();
        if (position_ >= input_.size()) return fail(error, "JSON 意外结束");
        if (input_[position_] == '"') {
            std::string ignored;
            return read_string(ignored, error);
        }
        if (input_[position_] == '{' || input_[position_] == '[') {
            const char opening = input_[position_++];
            const char closing = opening == '{' ? '}' : ']';
            int depth = 1;
            bool in_string = false;
            bool escaped = false;
            while (position_ < input_.size() && depth > 0) {
                const char character = input_[position_++];
                if (in_string) {
                    if (escaped) escaped = false;
                    else if (character == '\\') escaped = true;
                    else if (character == '"') in_string = false;
                } else if (character == '"') in_string = true;
                else if (character == opening) ++depth;
                else if (character == closing) --depth;
            }
            return depth == 0 || fail(error, "JSON 容器未结束");
        }
        while (position_ < input_.size() && input_[position_] != ',' &&
               input_[position_] != '}' && input_[position_] != ']') {
            ++position_;
        }
        return true;
    }

    bool expect(char expected, std::string& error) {
        skip_whitespace();
        if (consume(expected)) return true;
        return fail(error, std::string("JSON 缺少字符 ") + expected);
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_whitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool fail(std::string& error, std::string message) {
        error = std::move(message) + "，位置 " + std::to_string(position_);
        return false;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

} // namespace

struct FakeServerManager::Impl {
    mutable std::mutex mutex;
    std::map<int, std::shared_ptr<FakeServerRecord>> servers;
    // Servers in the same group write to one log.
    std::map<std::string, std::shared_ptr<LiveLog>> group_logs;
    int next_id = 1;
    bool network_ready = true;

    // Call with mutex held.
    std::shared_ptr<LiveLog> resolve_log(FakeServerRecord& record) {
        if (record.config.group.empty()) {
            if (!record.live_log) {
                record.live_log = std::make_shared<LiveLog>();
            }
            return record.live_log;
        }
        auto& shared = group_logs[record.config.group];
        if (!shared) {
            shared = std::make_shared<LiveLog>();
        }
        return shared;
    }

#ifdef _WIN32
    Impl() {
        WSADATA data{};
        network_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Impl() { if (network_ready) WSACleanup(); }
#endif
};

FakeServerManager::FakeServerManager() : impl_(std::make_unique<Impl>()) {}

FakeServerManager::~FakeServerManager() {
    for (const int id : ids()) {
        std::string ignored;
        stop(id, ignored);
    }
}

int FakeServerManager::create(FakeServerConfig config) {
    if (config.mode == FakeServerMode::Both) config.lan_port = config.port;
    auto record = std::make_shared<FakeServerRecord>();
    std::lock_guard lock(impl_->mutex);
    record->id = impl_->next_id++;
    record->config = std::move(config);
    impl_->servers.emplace(record->id, record);
    return record->id;
}

bool FakeServerManager::start(int id,
                              const LogHandler& log_handler,
                              std::string& error) {
    std::shared_ptr<FakeServerRecord> record;
    FakeServerConfig config;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->servers.find(id);
        if (iterator == impl_->servers.end()) {
            error = "未找到假服务器 " + std::to_string(id);
            return false;
        }
        record = iterator->second;
        if (record->running || record->starting) {
            error = "假服务器 " + std::to_string(id) + " 已在运行";
            return false;
        }
        if (!impl_->network_ready) {
            error = "网络组件初始化失败";
            return false;
        }
        record->starting = true;
        config = record->config;
    }

    if (config.mode == FakeServerMode::Both) config.lan_port = config.port;
    SocketHandle udp_socket = invalid_socket;
    SocketHandle tcp_socket = invalid_socket;
    if (uses_udp(config.mode)) {
        udp_socket = create_udp_socket(error);
        if (udp_socket == invalid_socket) {
            std::lock_guard lock(impl_->mutex);
            record->starting = false;
            return false;
        }
    }
    if (uses_tcp(config.mode)) {
        tcp_socket = create_tcp_listener(config.port, error);
        if (tcp_socket == invalid_socket) {
            close_socket(udp_socket);
            std::lock_guard lock(impl_->mutex);
            record->starting = false;
            return false;
        }
    }

    {
        std::lock_guard lock(impl_->mutex);
        record->config = config;
        record->starting = false;
        record->running = true;
        record->log_handler = log_handler;
        record->live = std::make_shared<LiveConfig>();
        record->live->config = config;
        auto live = record->live;
        auto live_log = impl_->resolve_log(*record);
        FakeServerManager::LogHandler sink =
            [live_log, log_handler, id](const std::string& event,
                                         const std::string& ip,
                                         const std::string& player) {
                live_log->append(event, ip, player, id);
                if (log_handler) {
                    log_handler(event, ip, player);
                }
            };
        record->worker = std::jthread(
            [live, udp_socket, tcp_socket, sink](std::stop_token stop_token) {
                run_fake_server(stop_token, live, udp_socket, tcp_socket, sink);
            });
    }
    return true;
}

bool FakeServerManager::stop(int id, std::string& error) {
    std::jthread worker;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->servers.find(id);
        if (iterator == impl_->servers.end()) {
            error = "未找到假服务器 " + std::to_string(id);
            return false;
        }
        auto& record = *iterator->second;
        if (!record.running) {
            if (record.starting) {
                error = "假服务器 " + std::to_string(id) + " 正在启动";
                return false;
            }
            error = "假服务器 " + std::to_string(id) + " 未运行";
            return false;
        }
        worker = std::move(record.worker);
        record.running = false;
    }
    worker.request_stop();
    worker.join();
    return true;
}

bool FakeServerManager::remove(int id, std::string& error) {
    bool running = false;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->servers.find(id);
        if (iterator == impl_->servers.end()) {
            error = "未找到假服务器 " + std::to_string(id);
            return false;
        }
        if (iterator->second->starting) {
            error = "假服务器 " + std::to_string(id) + " 正在启动";
            return false;
        }
        running = iterator->second->running;
    }
    if (running && !stop(id, error)) return false;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->servers.find(id);
        if (iterator == impl_->servers.end()) {
            error = "未找到假服务器 " + std::to_string(id);
            return false;
        }
        impl_->servers.erase(iterator);
    }
    return true;
}

bool FakeServerManager::modify(int id, const std::string& attribute,
                               const std::string& value, std::string& error) {
    std::lock_guard lock(impl_->mutex);
    const auto iterator = impl_->servers.find(id);
    if (iterator == impl_->servers.end()) {
        error = "未找到假服务器 " + std::to_string(id);
        return false;
    }
    if (iterator->second->running || iterator->second->starting) {
        error = "请先停止假服务器再修改";
        return false;
    }
    return apply_fake_server_attribute(iterator->second->config, attribute, value, error);
}

bool FakeServerManager::update(int id, FakeServerConfig config, std::string& error) {
    if (config.mode == FakeServerMode::Both) config.lan_port = config.port;

    LogHandler log_handler;
    bool restart = false;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->servers.find(id);
        if (iterator == impl_->servers.end()) {
            error = "未找到假服务器 " + std::to_string(id);
            return false;
        }
        FakeServerRecord& record = *iterator->second;
        if (record.starting) {
            error = "假服务器 " + std::to_string(id) + " 正在启动";
            return false;
        }
        if (!record.running) {
            record.config = std::move(config);
            return true;
        }

        // Sockets depend on the mode and the ports, so only those need a restart.
        restart = record.config.mode != config.mode ||
                  record.config.port != config.port ||
                  record.config.lan_port != config.lan_port;
        record.config = config;
        if (!restart) {
            record.live->assign(config);
            return true;
        }
        log_handler = record.log_handler;
    }

    if (!stop(id, error)) {
        return false;
    }
    return start(id, log_handler ? log_handler : LogHandler{
        [](const std::string&, const std::string&, const std::string&) {}}, error);
}

std::vector<FakeServerLogEntry> FakeServerManager::log(int id) const {
    std::shared_ptr<LiveLog> live_log;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->servers.find(id);
        if (iterator == impl_->servers.end()) {
            return {};
        }
        live_log = impl_->resolve_log(*iterator->second);
    }
    return live_log ? live_log->snapshot() : std::vector<FakeServerLogEntry>{};
}

void FakeServerManager::clear_log(int id) {
    std::shared_ptr<LiveLog> live_log;
    {
        std::lock_guard lock(impl_->mutex);
        const auto iterator = impl_->servers.find(id);
        if (iterator == impl_->servers.end()) {
            return;
        }
        live_log = impl_->resolve_log(*iterator->second);
    }
    if (live_log) {
        live_log->clear();
    }
}

std::vector<FakeServerSnapshot> FakeServerManager::list() const {
    std::vector<FakeServerSnapshot> snapshots;
    std::lock_guard lock(impl_->mutex);
    for (const auto& [id, record] : impl_->servers) {
        snapshots.push_back({id, record->running, record->config});
    }
    return snapshots;
}

std::vector<int> FakeServerManager::ids() const {
    std::vector<int> result;
    std::lock_guard lock(impl_->mutex);
    for (const auto& [id, record] : impl_->servers) result.push_back(id);
    return result;
}

bool FakeServerManager::is_running(int id) const {
    std::lock_guard lock(impl_->mutex);
    const auto iterator = impl_->servers.find(id);
    return iterator != impl_->servers.end() && iterator->second->running;
}

std::filesystem::path FakeServerManager::export_all(std::string& error) const {
    return export_servers({}, {}, error);
}

std::filesystem::path FakeServerManager::export_servers(const std::vector<int>& ids,
                                                       const std::string& group,
                                                       std::string& error) const {
    return export_servers(ids, group, {}, error);
}

std::filesystem::path FakeServerManager::export_servers(const std::vector<int>& ids,
                                                       const std::string& group,
                                                       const std::filesystem::path& target,
                                                       std::string& error) const {
    static std::mutex export_mutex;
    std::lock_guard export_lock(export_mutex);
    auto snapshots = list();
    if (!ids.empty()) {
        std::vector<FakeServerSnapshot> selected;
        for (const FakeServerSnapshot& snapshot : snapshots) {
            if (std::find(ids.begin(), ids.end(), snapshot.id) != ids.end()) {
                selected.push_back(snapshot);
            }
        }
        snapshots = std::move(selected);
    }
    if (!group.empty()) {
        for (FakeServerSnapshot& snapshot : snapshots) {
            snapshot.config.group = group;
        }
    }
    std::filesystem::path path = target;
    if (path.empty()) {
        std::error_code filesystem_error;
        const std::filesystem::path directory = "export";
        std::filesystem::create_directories(directory, filesystem_error);
        if (filesystem_error) {
            error = "无法创建 export 目录: " + filesystem_error.message();
            return {};
        }
        path = directory / "fakeserver.json";
        for (int suffix = 2; std::filesystem::exists(path); ++suffix) {
            path = directory / ("fakeserver" + std::to_string(suffix) + ".json");
        }
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        error = "无法创建 " + path.generic_string();
        return {};
    }

    output << "{\n  \"servers\": [";
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        const auto& snapshot = snapshots[index];
        const auto& config = snapshot.config;
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"group\":\"" << json_escape(config.group)
               << "\", \"mode\": \"" << fake_server_mode_text(config.mode)
               << "\", \"lanPort\": " << config.lan_port
               << ", \"lanMotd\": \"" << json_escape(config.lan_motd)
               << "\", \"port\": " << config.port
               << ", \"motd\": \"" << json_escape(config.motd)
               << "\", \"version\": \"" << json_escape(config.version)
               << "\", \"max\": " << config.max_players
               << ", \"online\": " << config.online_players
               << ", \"players\": [";
        for (std::size_t player = 0; player < config.players.size(); ++player) {
            if (player != 0) output << ", ";
            const std::string uuid = player < config.player_uuids.size()
                ? config.player_uuids[player] : std::string{};
            output << "{\"name\":\"" << json_escape(config.players[player])
                   << "\", \"uuid\":\"" << json_escape(uuid) << "\"}";
        }
        output << "], \"favicon\": \"" << json_escape(config.favicon)
               << "\", \"protocol\": " << config.protocol
               << ", \"kickmsg\": \"" << json_escape(config.kick_message)
               << "\", \"securechat\": \""
               << (config.secure_chat == SecureChatSetting::Yes ? "yes"
                   : config.secure_chat == SecureChatSetting::No ? "no" : "absent")
               << "\"}";
    }
    if (!snapshots.empty()) output << '\n';
    output << "  ]\n}\n";
    if (!output) {
        error = "写入 " + path.generic_string() + " 失败";
        return {};
    }
    return path;
}

std::vector<int> FakeServerManager::import_file(const std::filesystem::path& path,
                                                std::string& error) {
    return import_file(path, {}, error);
}

std::vector<int> FakeServerManager::import_file(const std::filesystem::path& path,
                                                const std::string& group,
                                                std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "无法打开 " + path.generic_string();
        return {};
    }
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    std::vector<FakeServerConfig> configs;
    FakeServerJsonParser parser(json);
    if (!parser.parse(configs, error)) return {};

    std::vector<int> imported_ids;
    imported_ids.reserve(configs.size());
    for (auto& config : configs) {
        if (!group.empty()) config.group = group;
        imported_ids.push_back(create(std::move(config)));
    }
    return imported_ids;
}

bool parse_fake_server_mode(const std::string& text, FakeServerMode& mode) {
    if (text == "udp") mode = FakeServerMode::Udp;
    else if (text == "tcp") mode = FakeServerMode::Tcp;
    else if (text == "both") mode = FakeServerMode::Both;
    else return false;
    return true;
}

std::string fake_server_mode_text(FakeServerMode mode) {
    switch (mode) {
        case FakeServerMode::Udp: return "udp";
        case FakeServerMode::Tcp: return "tcp";
        case FakeServerMode::Both: return "both";
    }
    return "unknown";
}

bool apply_fake_server_attribute(FakeServerConfig& config,
                                 const std::string& attribute,
                                 const std::string& value,
                                 std::string& error) {
    const std::string name = normalize_attribute(attribute);
    int number = 0;
    if (name == "mode") {
        if (!parse_fake_server_mode(value, config.mode)) {
            error = "mode 必须是 udp、tcp 或 both";
            return false;
        }
    } else if (name == "lan-port") {
        if (!parse_integer(value, 1, 65535, number, error)) return false;
        config.lan_port = static_cast<std::uint16_t>(number);
        if (config.mode == FakeServerMode::Both) config.port = config.lan_port;
    } else if (name == "lan-motd") {
        config.lan_motd = value;
    } else if (name == "port") {
        if (!parse_integer(value, 1, 65535, number, error)) return false;
        config.port = static_cast<std::uint16_t>(number);
        if (config.mode == FakeServerMode::Both) config.lan_port = config.port;
    } else if (name == "motd") {
        config.motd = value;
    } else if (name == "version") {
        config.version = value;
    } else if (name == "max") {
        if (!parse_integer(value, 0, 1000000, number, error)) return false;
        config.max_players = number;
    } else if (name == "online") {
        if (!parse_integer(value, 0, 1000000, number, error)) return false;
        config.online_players = number;
    } else if (name == "players") {
        // "Steve:uuid,Alex" — the uuid half is optional.
        config.players.clear();
        config.player_uuids.clear();
        std::istringstream stream(value);
        std::string player;
        while (std::getline(stream, player, ',')) {
            player = trim(player);
            if (player.empty()) continue;
            const std::size_t colon = player.find(':');
            if (colon == std::string::npos) {
                config.players.push_back(player);
                config.player_uuids.emplace_back();
            } else {
                config.players.push_back(trim(player.substr(0, colon)));
                config.player_uuids.push_back(trim(player.substr(colon + 1)));
            }
        }
    } else if (name == "favicon") {
        if (value == "none") {
            config.favicon.clear();
        } else {
            std::string loaded_favicon;
            if (!load_favicon_data_uri(value, loaded_favicon, error)) return false;
            config.favicon = std::move(loaded_favicon);
        }
    } else if (name == "protocol") {
        if (!parse_integer(value, 0, 1000000, number, error)) return false;
        config.protocol = number;
    } else if (name == "kickmsg") {
        config.kick_message = value;
    } else if (name == "group") {
        config.group = value;
    } else if (name == "securechat") {
        if (value == "yes" || value == "true") config.secure_chat = SecureChatSetting::Yes;
        else if (value == "no" || value == "false") config.secure_chat = SecureChatSetting::No;
        else if (value == "absent" || value == "none") config.secure_chat = SecureChatSetting::Absent;
        else {
            error = "securechat 必须是 yes、no 或 absent";
            return false;
        }
    } else {
        error = "未知假服务器属性: " + attribute;
        return false;
    }
    if (config.mode == FakeServerMode::Both) config.lan_port = config.port;
    return true;
}

bool apply_fake_server_attributes_json(FakeServerConfig& config,
                                       const std::string& json,
                                       std::string& error) {
    FakeServerJsonParser parser(json);
    return parser.parse_attributes(config, error);
}

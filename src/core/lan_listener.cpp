#include "lan_listener.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle invalid_socket = -1;
#endif

namespace {

constexpr const char* multicast_address = "224.0.2.60";
constexpr unsigned short multicast_port = 4445;

#ifdef _WIN32
class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        valid_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockSession() {
        if (valid_) {
            WSACleanup();
        }
    }

    bool valid() const { return valid_; }

private:
    bool valid_ = false;
};
#endif

class SocketGuard {
public:
    explicit SocketGuard(SocketHandle socket) : socket_(socket) {}
    ~SocketGuard() {
        if (socket_ != invalid_socket) {
#ifdef _WIN32
            closesocket(socket_);
#else
            close(socket_);
#endif
        }
    }

private:
    SocketHandle socket_;
};

std::string socket_error(const std::string& prefix) {
#ifdef _WIN32
    return prefix + " (Winsock 错误 " + std::to_string(WSAGetLastError()) + ")";
#else
    return prefix + ": " + std::error_code(errno, std::generic_category()).message();
#endif
}

bool parse_packet(const std::string& packet, std::string& motd, unsigned short& port) {
    constexpr const char* motd_open = "[MOTD]";
    constexpr const char* motd_close = "[/MOTD]";
    constexpr const char* address_open = "[AD]";
    constexpr const char* address_close = "[/AD]";

    const auto motd_begin = packet.find(motd_open);
    const auto motd_end = packet.find(motd_close, motd_begin);
    const auto port_begin = packet.find(address_open, motd_end);
    const auto port_end = packet.find(address_close, port_begin);
    if (motd_begin == std::string::npos || motd_end == std::string::npos ||
        port_begin == std::string::npos || port_end == std::string::npos) {
        return false;
    }

    const auto motd_content_begin = motd_begin + std::char_traits<char>::length(motd_open);
    const auto port_content_begin = port_begin + std::char_traits<char>::length(address_open);
    motd = packet.substr(motd_content_begin, motd_end - motd_content_begin);
    const std::string port_text = packet.substr(port_content_begin, port_end - port_content_begin);

    unsigned int parsed_port = 0;
    const auto [position, error] = std::from_chars(
        port_text.data(), port_text.data() + port_text.size(), parsed_port);
    if (error != std::errc{} || position != port_text.data() + port_text.size() ||
        parsed_port == 0 || parsed_port > 65535) {
        return false;
    }

    port = static_cast<unsigned short>(parsed_port);
    return true;
}

} // namespace

ListenResult listen_lan_advertisements(
    std::optional<std::chrono::steady_clock::time_point> deadline,
    const AdvertisementHandler& on_advertisement,
    const ListenTickHandler& on_tick,
    const ListenCancelHandler& cancelled) {
    ListenResult result;

#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.valid()) {
        result.error = "无法初始化 Winsock";
        return result;
    }
#endif

    const SocketHandle raw_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw_socket == invalid_socket) {
        result.error = socket_error("无法创建 UDP 套接字");
        return result;
    }
    SocketGuard socket_guard(raw_socket);

    int reuse = 1;
    if (setsockopt(raw_socket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
        result.error = socket_error("无法设置套接字复用");
        return result;
    }

    sockaddr_in local_address{};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(multicast_port);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(raw_socket, reinterpret_cast<sockaddr*>(&local_address), sizeof(local_address)) != 0) {
        result.error = socket_error("无法监听 UDP 端口 4445");
        return result;
    }

    ip_mreq membership{};
    if (inet_pton(AF_INET, multicast_address, &membership.imr_multiaddr) != 1) {
        result.error = "无效的组播地址";
        return result;
    }
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(raw_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0) {
        result.error = socket_error("无法加入 Minecraft 局域网组播组");
        return result;
    }

    while (!deadline || std::chrono::steady_clock::now() < *deadline) {
        if (cancelled()) {
            result.cancelled = true;
            break;
        }
        on_tick();

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(raw_socket, &read_set);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;

#ifdef _WIN32
        const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
#else
        const int selected = select(raw_socket + 1, &read_set, nullptr, nullptr, &timeout);
#endif
        if (selected < 0) {
            result.error = socket_error("监听 UDP 数据时出错");
            return result;
        }
        if (selected == 0) {
            continue;
        }

        std::array<char, 65536> buffer{};
        sockaddr_in sender{};
#ifdef _WIN32
        int sender_size = sizeof(sender);
#else
        socklen_t sender_size = sizeof(sender);
#endif
        const int received = recvfrom(raw_socket, buffer.data(), static_cast<int>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr*>(&sender), &sender_size);
        if (received <= 0) {
            continue;
        }

        std::string motd;
        unsigned short port = 0;
        if (!parse_packet(std::string(buffer.data(), static_cast<std::size_t>(received)),
                          motd, port)) {
            continue;
        }

        std::array<char, INET_ADDRSTRLEN> ip_buffer{};
        if (inet_ntop(AF_INET, &sender.sin_addr, ip_buffer.data(), ip_buffer.size()) == nullptr) {
            continue;
        }

        on_advertisement(LanAdvertisement{ip_buffer.data(), port, motd});
    }

    return result;
}

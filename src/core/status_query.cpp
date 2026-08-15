#include "status_query.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

using Clock = std::chrono::steady_clock;
constexpr auto query_timeout = std::chrono::seconds(2);
constexpr std::size_t max_status_packet_size = 1024 * 1024;

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

int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool operation_in_progress(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINVAL;
#else
    return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS;
#endif
}

std::string socket_error_text(const std::string& prefix, int error = last_socket_error()) {
#ifdef _WIN32
    return prefix + " (Winsock 错误 " + std::to_string(error) + ")";
#else
    return prefix + ": " + std::error_code(error, std::generic_category()).message();
#endif
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

bool wait_for_socket(SocketHandle socket,
                     bool read,
                     const Clock::time_point& deadline,
                     const std::function<bool()>& cancelled) {
    while (Clock::now() < deadline) {
        if (cancelled()) {
            return false;
        }

        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        if (read) {
            FD_SET(socket, &read_set);
        } else {
            FD_SET(socket, &write_set);
        }

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
#ifdef _WIN32
        const int selected = select(0, read ? &read_set : nullptr,
                                    read ? nullptr : &write_set, nullptr, &timeout);
#else
        const int selected = select(socket + 1, read ? &read_set : nullptr,
                                    read ? nullptr : &write_set, nullptr, &timeout);
#endif
        if (selected > 0) {
            return true;
        }
        if (selected < 0) {
            return false;
        }
    }
    return false;
}

bool send_all(SocketHandle socket,
              const std::vector<std::uint8_t>& data,
              const Clock::time_point& deadline,
              const std::function<bool()>& cancelled,
              std::string& error) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        if (!wait_for_socket(socket, false, deadline, cancelled)) {
            error = cancelled() ? "TCP 查询已取消" : "发送状态查询包超时";
            return false;
        }

        const int result = send(socket,
                                reinterpret_cast<const char*>(data.data() + sent),
                                static_cast<int>(data.size() - sent), 0);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        const int socket_error = last_socket_error();
        if (!operation_in_progress(socket_error)) {
            error = socket_error_text("发送状态查询包失败", socket_error);
            return false;
        }
    }
    return true;
}

bool receive_exact(SocketHandle socket,
                   std::uint8_t* destination,
                   std::size_t size,
                   const Clock::time_point& deadline,
                   const std::function<bool()>& cancelled,
                   std::string& error) {
    std::size_t received = 0;
    while (received < size) {
        if (!wait_for_socket(socket, true, deadline, cancelled)) {
            error = cancelled() ? "TCP 查询已取消" : "等待服务器状态响应超时";
            return false;
        }

        const int result = recv(socket,
                                reinterpret_cast<char*>(destination + received),
                                static_cast<int>(size - received), 0);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            error = "服务器在返回完整状态前关闭了连接";
            return false;
        }
        const int socket_error = last_socket_error();
        if (!operation_in_progress(socket_error)) {
            error = socket_error_text("接收服务器状态失败", socket_error);
            return false;
        }
    }
    return true;
}

void append_varint(std::vector<std::uint8_t>& output, std::int32_t value) {
    std::uint32_t remaining = static_cast<std::uint32_t>(value);
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(remaining & 0x7F);
        remaining >>= 7;
        if (remaining != 0) {
            byte |= 0x80;
        }
        output.push_back(byte);
    } while (remaining != 0);
}

void append_string(std::vector<std::uint8_t>& output, const std::string& value) {
    append_varint(output, static_cast<std::int32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> frame_packet(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> framed;
    append_varint(framed, static_cast<std::int32_t>(payload.size()));
    framed.insert(framed.end(), payload.begin(), payload.end());
    return framed;
}

bool receive_varint(SocketHandle socket,
                    std::int32_t& value,
                    const Clock::time_point& deadline,
                    const std::function<bool()>& cancelled,
                    std::string& error) {
    value = 0;
    for (int byte_index = 0; byte_index < 5; ++byte_index) {
        std::uint8_t byte = 0;
        if (!receive_exact(socket, &byte, 1, deadline, cancelled, error)) {
            return false;
        }
        value |= static_cast<std::int32_t>(byte & 0x7F) << (7 * byte_index);
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    error = "服务器返回了过长的 VarInt";
    return false;
}

bool read_varint(const std::vector<std::uint8_t>& input,
                 std::size_t& position,
                 std::int32_t& value) {
    value = 0;
    for (int byte_index = 0; byte_index < 5 && position < input.size(); ++byte_index) {
        const std::uint8_t byte = input[position++];
        value |= static_cast<std::int32_t>(byte & 0x7F) << (7 * byte_index);
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue* get(const std::string& key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        const auto iterator = object.find(key);
        return iterator == object.end() ? nullptr : &iterator->second;
    }
};

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        skip_whitespace();
        if (!parse_value(value, 0)) {
            error = error_.empty() ? "无效的状态 JSON" : error_;
            return false;
        }
        skip_whitespace();
        if (position_ != input_.size()) {
            error = "状态 JSON 末尾存在额外内容";
            return false;
        }
        return true;
    }

private:
    bool parse_value(JsonValue& value, int depth) {
        if (depth > 256) {
            return fail("状态 JSON 嵌套过深");
        }
        skip_whitespace();
        if (position_ >= input_.size()) {
            return fail("状态 JSON 意外结束");
        }

        const char character = input_[position_];
        if (character == '"') {
            value.type = JsonValue::Type::String;
            return parse_string(value.string);
        }
        if (character == '{') {
            return parse_object(value, depth + 1);
        }
        if (character == '[') {
            return parse_array(value, depth + 1);
        }
        if (character == 't' && consume_literal("true")) {
            value.type = JsonValue::Type::Boolean;
            value.boolean = true;
            return true;
        }
        if (character == 'f' && consume_literal("false")) {
            value.type = JsonValue::Type::Boolean;
            value.boolean = false;
            return true;
        }
        if (character == 'n' && consume_literal("null")) {
            value.type = JsonValue::Type::Null;
            return true;
        }
        return parse_number(value);
    }

    bool parse_object(JsonValue& value, int depth) {
        value.type = JsonValue::Type::Object;
        ++position_;
        skip_whitespace();
        if (consume('}')) {
            return true;
        }

        while (position_ < input_.size()) {
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_whitespace();
            if (!consume(':')) {
                return fail("状态 JSON 对象缺少冒号");
            }
            JsonValue child;
            if (!parse_value(child, depth)) {
                return false;
            }
            value.object.insert_or_assign(std::move(key), std::move(child));
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return fail("状态 JSON 对象缺少逗号");
            }
            skip_whitespace();
        }
        return fail("状态 JSON 对象未结束");
    }

    bool parse_array(JsonValue& value, int depth) {
        value.type = JsonValue::Type::Array;
        ++position_;
        skip_whitespace();
        if (consume(']')) {
            return true;
        }

        while (position_ < input_.size()) {
            JsonValue child;
            if (!parse_value(child, depth)) {
                return false;
            }
            value.array.push_back(std::move(child));
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return fail("状态 JSON 数组缺少逗号");
            }
            skip_whitespace();
        }
        return fail("状态 JSON 数组未结束");
    }

    bool parse_string(std::string& output) {
        if (!consume('"')) {
            return fail("状态 JSON 字符串缺少引号");
        }
        output.clear();
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') {
                return true;
            }
            if (static_cast<unsigned char>(character) < 0x20) {
                return fail("状态 JSON 字符串包含控制字符");
            }
            if (character != '\\') {
                output.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                return fail("状态 JSON 转义序列不完整");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = 0;
                    if (!parse_hex4(codepoint)) {
                        return fail("状态 JSON Unicode 转义无效");
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
                        position_ + 6 <= input_.size() && input_[position_] == '\\' &&
                        input_[position_ + 1] == 'u') {
                        position_ += 2;
                        std::uint32_t low = 0;
                        if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return fail("状态 JSON Unicode 代理对无效");
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default: return fail("状态 JSON 包含未知转义序列");
            }
        }
        return fail("状态 JSON 字符串未结束");
    }

    bool parse_hex4(std::uint32_t& value) {
        if (position_ + 4 > input_.size()) {
            return false;
        }
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = input_[position_++];
            value <<= 4;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool parse_number(JsonValue& value) {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        if (position_ >= input_.size()) {
            return fail("状态 JSON 数字无效");
        }
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                return fail("状态 JSON 值无效");
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }

        const std::string number_text(input_.substr(start, position_ - start));
        char* end = nullptr;
        value.number = std::strtod(number_text.c_str(), &end);
        if (end != number_text.c_str() + number_text.size() || !std::isfinite(value.number)) {
            return fail("状态 JSON 数字无效");
        }
        value.type = JsonValue::Type::Number;
        return true;
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    bool consume_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    void skip_whitespace() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\n' && character != '\r' && character != '\t') {
                break;
            }
            ++position_;
        }
    }

    bool fail(std::string error) {
        error_ = std::move(error);
        return false;
    }

    std::string_view input_;
    std::size_t position_ = 0;
    std::string error_;
};

void flatten_chat_component(const JsonValue& value, std::string& output) {
    if (value.type == JsonValue::Type::String) {
        output += value.string;
        return;
    }
    if (value.type == JsonValue::Type::Array) {
        for (const auto& child : value.array) {
            flatten_chat_component(child, output);
        }
        return;
    }
    if (value.type != JsonValue::Type::Object) {
        return;
    }

    if (const JsonValue* text = value.get("text"); text && text->type == JsonValue::Type::String) {
        output += text->string;
    } else if (const JsonValue* translate = value.get("translate");
               translate && translate->type == JsonValue::Type::String) {
        output += translate->string;
    }
    if (const JsonValue* extra = value.get("extra")) {
        flatten_chat_component(*extra, output);
    }
}

int json_integer(const JsonValue* value) {
    if (!value || value->type != JsonValue::Type::Number) {
        return 0;
    }
    return static_cast<int>(value->number);
}

bool parse_status_json(const std::string& json, ServerStatus& status, bool include_favicon) {
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root, status.error) || root.type != JsonValue::Type::Object) {
        if (status.error.empty()) {
            status.error = "服务器状态 JSON 不是对象";
        }
        return false;
    }

    if (const JsonValue* description = root.get("description")) {
        flatten_chat_component(*description, status.motd);
    }
    if (const JsonValue* version = root.get("version"); version &&
        version->type == JsonValue::Type::Object) {
        if (const JsonValue* name = version->get("name"); name &&
            name->type == JsonValue::Type::String) {
            status.version_name = name->string;
        }
        status.protocol_version = json_integer(version->get("protocol"));
    }
    if (const JsonValue* players = root.get("players"); players &&
        players->type == JsonValue::Type::Object) {
        status.online_players = json_integer(players->get("online"));
        status.max_players = json_integer(players->get("max"));
        if (const JsonValue* sample = players->get("sample"); sample &&
            sample->type == JsonValue::Type::Array) {
            for (const auto& player : sample->array) {
                if (const JsonValue* name = player.get("name"); name &&
                    name->type == JsonValue::Type::String) {
                    status.player_names.push_back(name->string);
                    const JsonValue* id = player.get("id");
                    status.player_ids.push_back(
                        id && id->type == JsonValue::Type::String ? id->string : std::string{});
                }
            }
        }
    }
    if (const JsonValue* secure_chat = root.get("enforcesSecureChat");
        secure_chat && secure_chat->type == JsonValue::Type::Boolean) {
        status.secure_chat_known = true;
        status.enforces_secure_chat = secure_chat->boolean;
    }
    if (include_favicon) {
        if (const JsonValue* favicon = root.get("favicon"); favicon &&
            favicon->type == JsonValue::Type::String) {
            status.favicon = favicon->string;
        }
    }

    status.available = true;
    return true;
}

} // namespace

ServerStatus query_server_status(const std::string& ip,
                                 unsigned short port,
                                 const std::function<bool()>& cancelled,
                                 bool include_favicon) {
    ServerStatus status;

#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.valid()) {
        status.error = "无法初始化 Winsock";
        return status;
    }
#endif

    const SocketHandle socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == invalid_socket) {
        status.error = socket_error_text("无法创建 TCP 套接字");
        return status;
    }
    SocketGuard socket_guard(socket_handle);
    if (!set_nonblocking(socket_handle)) {
        status.error = socket_error_text("无法设置 TCP 套接字为非阻塞模式");
        return status;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        status.error = "无效的服务器 IP 地址";
        return status;
    }

    const auto deadline = Clock::now() + query_timeout;
    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int connect_error = last_socket_error();
        if (!operation_in_progress(connect_error)) {
            status.error = socket_error_text("连接 Minecraft 服务器失败", connect_error);
            return status;
        }
        if (!wait_for_socket(socket_handle, false, deadline, cancelled)) {
            status.error = cancelled() ? "TCP 查询已取消" : "连接 Minecraft 服务器超时";
            return status;
        }
        int socket_error = 0;
#ifdef _WIN32
        int option_size = sizeof(socket_error);
#else
        socklen_t option_size = sizeof(socket_error);
#endif
        if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socket_error), &option_size) != 0 ||
            socket_error != 0) {
            status.error = socket_error_text("连接 Minecraft 服务器失败", socket_error);
            return status;
        }
    }

    std::vector<std::uint8_t> handshake;
    append_varint(handshake, 0);
    append_varint(handshake, -1);
    append_string(handshake, ip);
    handshake.push_back(static_cast<std::uint8_t>((port >> 8) & 0xFF));
    handshake.push_back(static_cast<std::uint8_t>(port & 0xFF));
    append_varint(handshake, 1);

    std::vector<std::uint8_t> request = frame_packet(handshake);
    const auto status_request = frame_packet(std::vector<std::uint8_t>{0});
    request.insert(request.end(), status_request.begin(), status_request.end());
    // Measure the round-trip of the status request itself, like the vanilla
    // client's ping indicator.
    const auto request_sent_at = std::chrono::steady_clock::now();
    if (!send_all(socket_handle, request, deadline, cancelled, status.error)) {
        return status;
    }

    std::int32_t packet_length = 0;
    if (!receive_varint(socket_handle, packet_length, deadline, cancelled, status.error)) {
        return status;
    }
    if (packet_length <= 0 || static_cast<std::size_t>(packet_length) > max_status_packet_size) {
        status.error = "服务器状态响应长度无效";
        return status;
    }

    std::vector<std::uint8_t> packet(static_cast<std::size_t>(packet_length));
    if (!receive_exact(socket_handle, packet.data(), packet.size(), deadline, cancelled, status.error)) {
        return status;
    }

    std::size_t position = 0;
    std::int32_t packet_id = -1;
    std::int32_t json_length = 0;
    if (!read_varint(packet, position, packet_id) || packet_id != 0 ||
        !read_varint(packet, position, json_length) || json_length < 0 ||
        static_cast<std::size_t>(json_length) > packet.size() - position) {
        status.error = "服务器状态响应格式无效";
        return status;
    }

    const auto elapsed = std::chrono::steady_clock::now() - request_sent_at;
    const std::string json(reinterpret_cast<const char*>(packet.data() + position),
                           static_cast<std::size_t>(json_length));
    parse_status_json(json, status, include_favicon);
    status.latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    return status;
}

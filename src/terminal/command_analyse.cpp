#include "command_analyse.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::string_view, 1> scan_aliases{"scan"};
constexpr std::array<std::string_view, 2> ping_server_aliases{"pingserver", "ps"};
constexpr std::array<std::string_view, 2> fake_server_aliases{"fakeserver", "fs"};
constexpr std::array<std::string_view, 1> help_aliases{"help"};
constexpr std::array<std::string_view, 2> exit_aliases{"exit", "quit"};

std::string trim(std::string text) {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
        return std::isspace(value) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
        return std::isspace(value) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return {first, last};
}

bool tokenize(const std::string& input, std::vector<std::string>& tokens, std::string& error) {
    tokens.clear();
    std::string token;
    bool quoted = false;
    bool escaped = false;
    int json_depth = 0;
    bool json_quoted = false;
    bool json_escaped = false;
    for (const char character : input) {
        if (json_depth > 0) {
            token.push_back(character);
            if (json_quoted) {
                if (json_escaped) {
                    json_escaped = false;
                } else if (character == '\\') {
                    json_escaped = true;
                } else if (character == '"') {
                    json_quoted = false;
                }
            } else if (character == '"') {
                json_quoted = true;
            } else if (character == '{') {
                ++json_depth;
            } else if (character == '}') {
                --json_depth;
            }
            continue;
        }
        if (escaped) {
            token.push_back(character);
            escaped = false;
        } else if (character == '\\' && quoted) {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        } else if (std::isspace(static_cast<unsigned char>(character)) && !quoted) {
            if (!token.empty()) {
                tokens.push_back(std::move(token));
                token.clear();
            }
        } else {
            token.push_back(character);
            if (token == "--attributes={") {
                json_depth = 1;
            }
        }
    }
    if (json_depth != 0 || json_quoted) {
        error = "--attributes JSON 对象未结束";
        return false;
    }
    if (escaped || quoted) {
        error = "命令包含未结束的引号或转义";
        return false;
    }
    if (!token.empty()) tokens.push_back(std::move(token));
    return true;
}

template <std::size_t Size>
bool is_alias(const std::string& token, const std::array<std::string_view, Size>& aliases) {
    return std::find(aliases.begin(), aliases.end(), token) != aliases.end();
}

bool parse_positive_integer(const std::string& text, int& value) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto [position, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && position == end && value > 0;
}

bool parse_motd_option(const std::string& token, MotdMode& mode, std::string& error) {
    if (!token.starts_with("--motd=")) {
        return false;
    }

    const std::string value = token.substr(7);
    if (value == "raw") {
        mode = MotdMode::Raw;
    } else if (value == "clean") {
        mode = MotdMode::Clean;
    } else if (value == "format") {
        mode = MotdMode::Format;
    } else {
        error = "未知 MOTD 模式: " + value;
    }
    return true;
}

bool parse_endpoint(const std::string& endpoint,
                    std::string& host,
                    std::uint16_t& port,
                    std::string& error) {
    if (endpoint.empty()) {
        error = "服务器地址不能为空";
        return false;
    }
    if (endpoint.front() == '[' || std::count(endpoint.begin(), endpoint.end(), ':') > 1) {
        error = "当前仅支持 IPv4 地址，格式为 IP[:端口]";
        return false;
    }

    const std::size_t separator = endpoint.rfind(':');
    host = separator == std::string::npos ? endpoint : endpoint.substr(0, separator);
    if (host.empty()) {
        error = "服务器 IP 不能为空";
        return false;
    }
    if (separator == std::string::npos) {
        port = 25565;
        return true;
    }

    int parsed_port = 0;
    if (!parse_positive_integer(endpoint.substr(separator + 1), parsed_port) || parsed_port > 65535) {
        error = "服务器端口无效";
        return false;
    }
    port = static_cast<std::uint16_t>(parsed_port);
    return true;
}

Command parse_scan(const std::vector<std::string>& tokens, const std::string& source) {
    Command command;
    command.type = CommandType::Scan;
    command.source = source;

    bool seconds_seen = false;
    bool mode_seen = false;
    bool motd_seen = false;
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (token == "--export" || token == "--xp") {
            command.scan.export_results = true;
            continue;
        }
        if (token.starts_with("--mode=")) {
            if (mode_seen) {
                command.type = CommandType::Invalid;
                command.error = "scan 只能指定一次 --mode";
                return command;
            }
            mode_seen = true;
            const std::string mode = token.substr(7);
            if (mode == "only-udp") {
                command.scan.mode = ScanMode::OnlyUdp;
            } else if (mode == "full") {
                command.scan.mode = ScanMode::Full;
            } else {
                command.type = CommandType::Invalid;
                command.error = "未知扫描模式: " + mode;
                return command;
            }
            continue;
        }
        if (token.starts_with("--motd=")) {
            if (motd_seen) {
                command.type = CommandType::Invalid;
                command.error = "scan 只能指定一次 --motd";
                return command;
            }
            motd_seen = true;
            if (!parse_motd_option(token, command.scan.motd_mode, command.error) ||
                !command.error.empty()) {
                command.type = CommandType::Invalid;
                return command;
            }
            continue;
        }
        if (token.starts_with("--")) {
            command.type = CommandType::Invalid;
            command.error = "未知 scan 选项: " + token;
            return command;
        }

        int seconds = 0;
        if (!seconds_seen && parse_positive_integer(token, seconds)) {
            if (seconds > 86400) {
                command.type = CommandType::Invalid;
                command.error = "扫描时长不能超过 86400 秒";
                return command;
            }
            command.scan.seconds = seconds;
            seconds_seen = true;
            continue;
        }

        command.type = CommandType::Invalid;
        command.error = "scan 参数无效: " + token;
        return command;
    }
    return command;
}

Command parse_ping_server(const std::vector<std::string>& tokens, const std::string& source) {
    Command command;
    command.type = CommandType::PingServer;
    command.source = source;

    bool endpoint_seen = false;
    bool motd_seen = false;
    for (std::size_t index = 1; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (token == "--no-favicon") {
            command.ping_server.no_favicon = true;
            continue;
        }
        if (token.starts_with("--motd=")) {
            if (motd_seen) {
                command.type = CommandType::Invalid;
                command.error = "pingserver 只能指定一次 --motd";
                return command;
            }
            motd_seen = true;
            if (!parse_motd_option(token, command.ping_server.motd_mode, command.error) ||
                !command.error.empty()) {
                command.type = CommandType::Invalid;
                return command;
            }
            continue;
        }
        if (token.starts_with("--")) {
            command.type = CommandType::Invalid;
            command.error = "未知 pingserver 选项: " + token;
            return command;
        }
        if (endpoint_seen) {
            command.type = CommandType::Invalid;
            command.error = "pingserver 只能指定一个服务器地址";
            return command;
        }
        endpoint_seen = true;
        if (!parse_endpoint(token, command.ping_server.host,
                            command.ping_server.port, command.error)) {
            command.type = CommandType::Invalid;
            return command;
        }
    }

    if (!endpoint_seen) {
        command.type = CommandType::Invalid;
        command.error = "pingserver 缺少服务器地址";
    }
    return command;
}

bool parse_target(const std::string& token, FakeServerCommandOptions& options,
                  std::string& error) {
    if (token == "all") {
        options.all = true;
        return true;
    }
    if (!parse_positive_integer(token, options.id)) {
        error = "目标必须是正整数 ID 或 all";
        return false;
    }
    return true;
}

Command parse_fake_server(const std::vector<std::string>& tokens, const std::string& source) {
    Command command;
    command.type = CommandType::FakeServer;
    command.source = source;
    if (tokens.size() < 2) {
        command.type = CommandType::Invalid;
        command.error = "fs 缺少子命令";
        return command;
    }

    const std::string& action = tokens[1];
    if (action == "new") {
        command.fake_server.action = FakeServerAction::New;
        if (tokens.size() < 3 ||
            (tokens[2] != "udp" && tokens[2] != "tcp" && tokens[2] != "both")) {
            command.type = CommandType::Invalid;
            command.error = "用法: fs new <udp|tcp|both> [--attributes={<json text>}]";
            return command;
        }
        command.fake_server.mode = tokens[2];
        if (tokens.size() > 4 ||
            (tokens.size() == 4 && !tokens[3].starts_with("--attributes="))) {
            command.type = CommandType::Invalid;
            command.error = "fs new 仅支持 --attributes={<json text>} 预设属性";
            return command;
        }
        if (tokens.size() == 4) {
            command.fake_server.attributes_json = tokens[3].substr(13);
            if (command.fake_server.attributes_json.empty()) {
                command.type = CommandType::Invalid;
                command.error = "--attributes 后必须提供 JSON 对象";
            }
        }
        return command;
    }

    if (action == "start" || action == "stop" || action == "remove") {
        if (tokens.size() != 3 ||
            !parse_target(tokens[2], command.fake_server, command.error)) {
            command.type = CommandType::Invalid;
            if (command.error.empty()) command.error = "用法: fs " + action + " <ID|all>";
            return command;
        }
        command.fake_server.action = action == "start" ? FakeServerAction::Start
            : action == "stop" ? FakeServerAction::Stop : FakeServerAction::Remove;
        return command;
    }

    if (action == "modify") {
        command.fake_server.action = FakeServerAction::Modify;
        if (tokens.size() != 5 || !parse_positive_integer(tokens[2], command.fake_server.id)) {
            command.type = CommandType::Invalid;
            command.error = "用法: fs modify <ID> <属性> <值>，含空格的值请使用引号";
            return command;
        }
        command.fake_server.attribute = tokens[3];
        command.fake_server.value = tokens[4];
        return command;
    }

    if (action == "list") {
        command.fake_server.action = FakeServerAction::List;
        bool mode_seen = false;
        bool motd_seen = false;
        for (std::size_t index = 2; index < tokens.size(); ++index) {
            const std::string& option = tokens[index];
            if (option.starts_with("--mode=")) {
                if (mode_seen) {
                    command.type = CommandType::Invalid;
                    command.error = "fs list 只能指定一次 --mode";
                    return command;
                }
                mode_seen = true;
                const std::string mode = option.substr(7);
                if (mode == "only-udp") command.fake_server.display_mode = ScanMode::OnlyUdp;
                else if (mode == "full") command.fake_server.display_mode = ScanMode::Full;
                else {
                    command.type = CommandType::Invalid;
                    command.error = "未知列表模式: " + mode;
                    return command;
                }
                continue;
            }
            if (option.starts_with("--motd=")) {
                if (motd_seen) {
                    command.type = CommandType::Invalid;
                    command.error = "fs list 只能指定一次 --motd";
                    return command;
                }
                motd_seen = true;
                if (!parse_motd_option(option, command.fake_server.motd_mode, command.error) ||
                    !command.error.empty()) {
                    command.type = CommandType::Invalid;
                    return command;
                }
                continue;
            }
            command.type = CommandType::Invalid;
            command.error = "未知 fs list 选项: " + option;
            return command;
        }
        return command;
    }
    if ((action == "export" || action == "xp") && tokens.size() == 2) {
        command.fake_server.action = FakeServerAction::Export;
        return command;
    }
    if (action == "import" && tokens.size() == 3) {
        command.fake_server.action = FakeServerAction::Import;
        command.fake_server.filename = tokens[2];
        return command;
    }

    command.type = CommandType::Invalid;
    command.error = "无效的 fs 子命令或参数";
    return command;
}

} // namespace

Command CommandParser::parse(const std::string& input) const {
    const std::string cleaned = trim(input);
    if (cleaned.empty()) {
        return {};
    }

    if (cleaned.front() != '&') {
        return parse_regular(cleaned);
    }
    if (cleaned == "&") {
        Command command;
        command.type = CommandType::ListTasks;
        return command;
    }
    if (std::isspace(static_cast<unsigned char>(cleaned[1])) != 0) {
        std::vector<std::string> tokens;
        std::string token_error;
        if (!tokenize(trim(cleaned.substr(1)), tokens, token_error)) {
            Command command;
            command.type = CommandType::Invalid;
            command.error = token_error;
            return command;
        }
        int task_id = 0;
        if (tokens.size() == 1 && parse_positive_integer(tokens.front(), task_id)) {
            Command command;
            command.type = CommandType::ShowTask;
            command.task_id = task_id;
            return command;
        }
        if (tokens.size() == 2 && tokens.front() == "remove" &&
            parse_positive_integer(tokens.back(), task_id)) {
            Command command;
            command.type = CommandType::RemoveTask;
            command.task_id = task_id;
            return command;
        }
        Command command;
        command.type = CommandType::Invalid;
        command.error = "& 后有空格时仅支持 & <任务ID> 或 & remove <任务ID>";
        return command;
    }

    Command command = parse_regular(cleaned.substr(1));
    if (command.type == CommandType::Scan || command.type == CommandType::PingServer ||
        command.type == CommandType::FakeServer ||
        command.type == CommandType::Help) {
        command.asynchronous = true;
    } else if (command.type != CommandType::Invalid) {
        command.type = CommandType::Invalid;
        command.error = "该命令不支持异步执行";
    }
    return command;
}

Command CommandParser::parse_regular(const std::string& input) const {
    std::vector<std::string> tokens;
    std::string token_error;
    if (!tokenize(input, tokens, token_error)) {
        Command command;
        command.type = CommandType::Invalid;
        command.error = token_error;
        return command;
    }
    if (tokens.empty()) {
        return {};
    }
    if (is_alias(tokens.front(), scan_aliases)) {
        return parse_scan(tokens, input);
    }
    if (is_alias(tokens.front(), ping_server_aliases)) {
        return parse_ping_server(tokens, input);
    }
    if (is_alias(tokens.front(), fake_server_aliases)) {
        return parse_fake_server(tokens, input);
    }

    Command command;
    command.source = input;
    if (is_alias(tokens.front(), help_aliases)) {
        command.type = tokens.size() == 1 ? CommandType::Help : CommandType::Invalid;
        command.error = tokens.size() == 1 ? "" : "help 命令不接受参数";
        return command;
    }
    if (is_alias(tokens.front(), exit_aliases)) {
        command.type = tokens.size() == 1 ? CommandType::Exit : CommandType::Invalid;
        command.error = tokens.size() == 1 ? "" : "exit/quit 命令不接受参数";
        return command;
    }

    command.type = CommandType::Invalid;
    command.error = "未知命令，输入 help 查看帮助";
    return command;
}

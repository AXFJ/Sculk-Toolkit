#include "render_world.h"

#include "console.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>

namespace {

std::size_t display_width(const std::string& text) {
    std::size_t width = 0;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        std::size_t sequence_length = 1;
        std::uint32_t codepoint = byte;
        if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
            sequence_length = 2;
            codepoint = ((byte & 0x1F) << 6) |
                        (static_cast<unsigned char>(text[index + 1]) & 0x3F);
        } else if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
            sequence_length = 3;
            codepoint = ((byte & 0x0F) << 12) |
                        ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(text[index + 2]) & 0x3F);
        } else if ((byte & 0xF8) == 0xF0 && index + 3 < text.size()) {
            sequence_length = 4;
            codepoint = ((byte & 0x07) << 18) |
                        ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
                        ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(text[index + 3]) & 0x3F);
        }
        width += codepoint >= 0x2E80 ? 2 : 1;
        index += sequence_length;
    }
    return width;
}

std::string padded_field(const std::string& field, std::size_t width = 9) {
    const std::size_t current_width = display_width(field);
    return field + std::string(current_width < width ? width - current_width : 1, ' ');
}

std::string single_line(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (character == 0xC2 && index + 2 < text.size() &&
            static_cast<unsigned char>(text[index + 1]) == 0xA7) {
            index += 2;
            continue;
        }
        if (character == '\n') output += " / ";
        else if (character == '\t') output.push_back(' ');
        else if (character >= 0x20) output.push_back(static_cast<char>(character));
    }
    return output;
}

std::string raw_single_line(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (const unsigned char character : text) {
        if (character == '\n') output += " / ";
        else if (character == '\t') output.push_back(' ');
        else if (character >= 0x20) output.push_back(static_cast<char>(character));
    }
    return output;
}

int hexadecimal_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

std::string legacy_format_code(char code) {
    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(code)))) {
        case '0': return "\x1b[0m\x1b[30m";
        case '1': return "\x1b[0m\x1b[34m";
        case '2': return "\x1b[0m\x1b[32m";
        case '3': return "\x1b[0m\x1b[36m";
        case '4': return "\x1b[0m\x1b[31m";
        case '5': return "\x1b[0m\x1b[35m";
        case '6': return "\x1b[0m\x1b[33m";
        case '7': return "\x1b[0m\x1b[37m";
        case '8': return "\x1b[0m\x1b[90m";
        case '9': return "\x1b[0m\x1b[94m";
        case 'a': return "\x1b[0m\x1b[92m";
        case 'b': return "\x1b[0m\x1b[96m";
        case 'c': return "\x1b[0m\x1b[91m";
        case 'd': return "\x1b[0m\x1b[95m";
        case 'e': return "\x1b[0m\x1b[93m";
        case 'f': return "\x1b[0m\x1b[97m";
        case 'l': return "\x1b[1m";
        case 'm': return "\x1b[9m";
        case 'n': return "\x1b[4m";
        case 'o': return "\x1b[3m";
        case 'r': return "\x1b[0m\x1b[97m";
        default: return {};
    }
}

std::string formatted_motd(const std::string& text) {
    std::string output;
    output.reserve(text.size() + 32);
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (character == 0xC2 && index + 2 < text.size() &&
            static_cast<unsigned char>(text[index + 1]) == 0xA7) {
            const char code = text[index + 2];
            if ((code == 'x' || code == 'X') && index + 20 < text.size()) {
                int rgb = 0;
                bool valid = true;
                std::size_t position = index + 3;
                for (int digit = 0; digit < 6; ++digit, position += 3) {
                    if (position + 2 >= text.size() ||
                        static_cast<unsigned char>(text[position]) != 0xC2 ||
                        static_cast<unsigned char>(text[position + 1]) != 0xA7) {
                        valid = false;
                        break;
                    }
                    const int value = hexadecimal_value(text[position + 2]);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    rgb = (rgb << 4) | value;
                }
                if (valid) {
                    output += "\x1b[0m\x1b[38;2;" + std::to_string((rgb >> 16) & 0xFF) + ';' +
                              std::to_string((rgb >> 8) & 0xFF) + ';' +
                              std::to_string(rgb & 0xFF) + 'm';
                    index += 20;
                    continue;
                }
            }
            output += legacy_format_code(code);
            index += 2;
            continue;
        }
        if (character == '\n') output += " / ";
        else if (character == '\t') output.push_back(' ');
        else if (character >= 0x20) output.push_back(static_cast<char>(character));
    }
    return output;
}

std::string render_motd(const std::string& text, MotdMode mode) {
    if (mode == MotdMode::Raw) return raw_single_line(text);
    if (mode == MotdMode::Format) return formatted_motd(text);
    return single_line(text);
}

std::string colored_address(const std::string& host, unsigned short port) {
    return "{COLOR_GREEN}" + host + "{COLOR_DARK_GREEN}:" +
           std::to_string(port) + "{COLOR_RESET}";
}

std::string player_summary(const ServerStatus& status) {
    std::ostringstream output;
    output << "{COLOR_BLUE}" << status.online_players << '/' << status.max_players;
    if (!status.player_names.empty()) {
        output << "{COLOR_MAGENTA} (";
        for (std::size_t index = 0; index < status.player_names.size(); ++index) {
            if (index != 0) output << ", ";
            output << single_line(status.player_names[index]);
        }
        output << ')';
    }
    return output.str();
}

void append_field(std::ostringstream& output,
                  const std::string& prefix,
                  const std::string& name,
                  const std::string& value,
                  const std::string& value_color = "{COLOR_WHITE}") {
    output << "{COLOR_WHITE}" << prefix << "{COLOR_GRAY}" << padded_field(name)
           << value_color << value << "{COLOR_RESET}\n";
}

ServerStatus fake_status(const FakeServerConfig& config) {
    ServerStatus status;
    status.available = true;
    status.motd = config.motd;
    status.version_name = config.version;
    status.protocol_version = config.protocol;
    status.online_players = config.online_players;
    status.max_players = config.max_players;
    status.player_names = config.players;
    return status;
}

bool uses_udp(FakeServerMode mode) {
    return mode == FakeServerMode::Udp || mode == FakeServerMode::Both;
}

bool uses_tcp(FakeServerMode mode) {
    return mode == FakeServerMode::Tcp || mode == FakeServerMode::Both;
}

std::string fake_mode_label(FakeServerMode mode) {
    switch (mode) {
        case FakeServerMode::Udp: return "UDP模式";
        case FakeServerMode::Tcp: return "TCP模式";
        case FakeServerMode::Both: return "UDP与TCP模式";
    }
    return "未知模式";
}

} // namespace

std::string format_world(const LanWorld& world, ScanMode mode, MotdMode motd_mode) {
    std::ostringstream output;
    if (mode == ScanMode::OnlyUdp) {
        append_field(output, "*  │ ", "IP", colored_address(world.ip, world.port));
        append_field(output, "   │ ", "Lan MOTD", render_motd(world.lan_motd, motd_mode));
        return console::colorize(output.str());
    }

    if (mode == ScanMode::Compact) {
        append_field(output, "* │ ", "Lan MOTD",
                     render_motd(world.lan_motd, motd_mode) + " (" +
                     colored_address(world.ip, world.port) + "{COLOR_WHITE})");
    } else {
        append_field(output, "*  │ ", "IP", colored_address(world.ip, world.port));
        append_field(output, "   │ ", "Lan MOTD", render_motd(world.lan_motd, motd_mode));
    }

    const std::string continuation = mode == ScanMode::Compact ? "  │ " : "   │ ";
    if (!world.status || !world.status->available) {
        const std::string error = world.status && !world.status->error.empty()
            ? world.status->error : "服务器未返回状态";
        append_field(output, continuation, "TCP 查询", single_line(error), "{COLOR_YELLOW}");
        return console::colorize(output.str());
    }

    const ServerStatus& status = *world.status;
    if (mode == ScanMode::Compact) {
        append_field(output, continuation, "版本", single_line(status.version_name),
                     "{COLOR_LIGHT_YELLOW}");
        append_field(output, continuation, "在线", player_summary(status), "");
    } else {
        append_field(output, continuation, "MOTD", render_motd(status.motd, motd_mode));
        append_field(output, continuation, "版本", single_line(status.version_name),
                     "{COLOR_LIGHT_YELLOW}");
        append_field(output, continuation, "协议版本", std::to_string(status.protocol_version),
                     "{COLOR_LIGHT_YELLOW}");
        append_field(output, continuation, "在线", player_summary(status), "");
    }
    return console::colorize(output.str());
}

std::string format_server_status(const std::string& host,
                                 unsigned short port,
                                 const ServerStatus& status,
                                 MotdMode motd_mode) {
    std::ostringstream output;
    append_field(output, "*  │ ", "IP", colored_address(host, port));
    if (!status.available) {
        append_field(output, "   │ ", "TCP 查询", single_line(status.error), "{COLOR_YELLOW}");
        return console::colorize(output.str());
    }
    append_field(output, "   │ ", "MOTD", render_motd(status.motd, motd_mode));
    append_field(output, "   │ ", "版本", single_line(status.version_name),
                 "{COLOR_LIGHT_YELLOW}");
    append_field(output, "   │ ", "协议版本", std::to_string(status.protocol_version),
                 "{COLOR_LIGHT_YELLOW}");
    append_field(output, "   │ ", "在线", player_summary(status), "");
    return console::colorize(output.str());
}

std::string render_fake_server(const FakeServerSnapshot& server,
                               ScanMode mode,
                               MotdMode motd_mode) {
    const FakeServerConfig& config = server.config;
    const bool udp = uses_udp(config.mode);
    const bool tcp = uses_tcp(config.mode);
    const unsigned short display_port = tcp ? config.port : config.lan_port;
    const std::string continuation = "  │ ";
    std::ostringstream output;

    output << "{COLOR_WHITE}* │ #" << server.id << "，" << fake_mode_label(config.mode) << "，"
           << (server.running ? "{COLOR_GREEN}运行中" : "{COLOR_YELLOW}已停止")
           << "{COLOR_WHITE}  (" << colored_address("0.0.0.0", display_port)
           << "{COLOR_WHITE}){COLOR_RESET}\n";

    if (mode == ScanMode::OnlyUdp) {
        if (!udp) {
            append_field(output, continuation, "UDP 广播", "不可用（tcp 模式）", "{COLOR_YELLOW}");
        } else {
            append_field(output, continuation, "Lan MOTD", render_motd(config.lan_motd, motd_mode));
        }
        return console::colorize(output.str());
    }

    if (mode == ScanMode::Full) {
        if (udp) {
            append_field(output, continuation, "Lan MOTD", render_motd(config.lan_motd, motd_mode));
        }
        if (tcp) {
            const ServerStatus status = fake_status(config);
            append_field(output, continuation, "MOTD", render_motd(config.motd, motd_mode));
            append_field(output, continuation, "版本", single_line(config.version),
                         "{COLOR_LIGHT_YELLOW}");
            append_field(output, continuation, "协议版本", std::to_string(config.protocol),
                         "{COLOR_LIGHT_YELLOW}");
            append_field(output, continuation, "在线", player_summary(status), "");
            append_field(output, continuation, "踢出消息",
                         render_motd(config.kick_message, motd_mode), "{COLOR_WHITE}");
        }
        return console::colorize(output.str());
    }

    if (udp) {
        append_field(output, continuation, "Lan MOTD", render_motd(config.lan_motd, motd_mode));
    }
    if (tcp) {
        const ServerStatus status = fake_status(config);
        if (!udp) {
            append_field(output, continuation, "MOTD", render_motd(config.motd, motd_mode));
        }
        append_field(output, continuation, "版本", single_line(config.version),
                     "{COLOR_LIGHT_YELLOW}");
        append_field(output, continuation, "在线", player_summary(status), "");
    }
    return console::colorize(output.str());
}

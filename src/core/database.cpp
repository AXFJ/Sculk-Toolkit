#include "database.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

namespace {

// Escapes a string for a JSON value.
std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        if (c == '"')       escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else                escaped.push_back(c);
    }
    return escaped;
}

// A minimal parser for a fake-server log file ({"fakeserverLogItems":[...]}).
// It keeps only "login" events and reads the ip/player pairs from them, skipping
// unknown keys and "slp" events.
class LoginLogParser {
public:
    explicit LoginLogParser(std::string_view input) : input_(input) {}

    bool parse(std::vector<DatabaseItem>& items, std::string& error) {
        skip_whitespace();
        if (!consume('{')) {
            return fail(error, "JSON 根节点必须是对象");
        }
        bool found_items = false;
        while (true) {
            skip_whitespace();
            if (consume('}')) {
                break;
            }
            std::string key;
            if (!read_string(key, error) || !expect(':', error)) {
                return false;
            }
            if (key == "fakeserverLogItems") {
                if (!read_log_entries(items, error)) {
                    return false;
                }
                found_items = true;
            } else if (!skip_value(error)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) {
                break;
            }
            if (!expect(',', error)) {
                return false;
            }
        }
        if (!found_items) {
            return fail(error, "JSON 缺少 fakeserverLogItems 数组");
        }
        skip_whitespace();
        return position_ == input_.size() || fail(error, "JSON 末尾存在多余内容");
    }

private:
    std::string_view input_;
    std::size_t position_ = 0;

    void skip_whitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool consume(char c) {
        if (position_ < input_.size() && input_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }

    bool expect(char c, std::string& error) {
        if (consume(c)) {
            return true;
        }
        std::string message = "期望字符 ";
        message.push_back(c);
        return fail(error, message);
    }

    bool fail(std::string& error, const std::string& message) {
        error = message;
        return false;
    }

    bool read_string(std::string& out, std::string& error) {
        if (!consume('"')) {
            return fail(error, "期望字符串");
        }
        out.clear();
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c == '"') {
                ++position_;
                return true;
            }
            if (c == '\\') {
                ++position_;
                if (position_ >= input_.size()) {
                    break;
                }
                const char escaped = input_[position_++];
                switch (escaped) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    default:
                        // Rare escapes pass through; player names are ASCII.
                        out.push_back('\\');
                        out.push_back(escaped);
                        break;
                }
            } else {
                out.push_back(c);
                ++position_;
            }
        }
        return fail(error, "字符串未闭合");
    }

    bool read_log_entries(std::vector<DatabaseItem>& items, std::string& error) {
        if (!expect('[', error)) {
            return false;
        }
        skip_whitespace();
        if (consume(']')) {
            return true;
        }
        while (true) {
            skip_whitespace();
            if (!read_log_entry(items, error)) {
                return false;
            }
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!expect(',', error)) {
                return false;
            }
        }
    }

    // One log entry is {"time":"...","event":"login","ip":"...","player":"..."}.
    bool read_log_entry(std::vector<DatabaseItem>& items, std::string& error) {
        if (!expect('{', error)) {
            return false;
        }
        skip_whitespace();
        if (consume('}')) {
            return true;
        }
        std::string event;
        std::string ip;
        std::string player;
        while (true) {
            std::string key;
            if (!read_string(key, error) || !expect(':', error)) {
                return false;
            }
            if (key == "event") {
                if (!read_string(event, error)) {
                    return false;
                }
            } else if (key == "ip") {
                if (!read_string(ip, error)) {
                    return false;
                }
            } else if (key == "player") {
                if (!read_string(player, error)) {
                    return false;
                }
            } else if (!skip_value(error)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) {
                break;
            }
            if (!expect(',', error)) {
                return false;
            }
        }
        if (event == "login" && !ip.empty() && !player.empty()) {
            DatabaseItem item;
            std::snprintf(item.ip.data(), item.ip.size(), "%s", ip.c_str());
            std::snprintf(item.player.data(), item.player.size(), "%s",
                          player.c_str());
            items.push_back(item);
        }
        return true;
    }

    bool skip_value(std::string& error) {
        skip_whitespace();
        if (position_ >= input_.size()) {
            return fail(error, "缺少值");
        }
        const char c = input_[position_];
        if (c == '{') {
            ++position_;
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            while (true) {
                std::string key;
                if (!read_string(key, error) || !expect(':', error)) {
                    return false;
                }
                if (!skip_value(error)) {
                    return false;
                }
                skip_whitespace();
                if (consume('}')) {
                    return true;
                }
                if (!expect(',', error)) {
                    return false;
                }
            }
        }
        if (c == '[') {
            ++position_;
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            while (true) {
                if (!skip_value(error)) {
                    return false;
                }
                skip_whitespace();
                if (consume(']')) {
                    return true;
                }
                if (!expect(',', error)) {
                    return false;
                }
            }
        }
        if (c == '"') {
            std::string dummy;
            return read_string(dummy, error);
        }
        // Numbers and literals run to the next structural character.
        while (position_ < input_.size() && input_[position_] != ',' &&
               input_[position_] != '}' && input_[position_] != ']') {
            ++position_;
        }
        return true;
    }
};

// A minimal parser for the {"databaseItems":[...]} file format. It skips unknown
// keys so future extra fields stay harmless.
class DatabaseJsonParser {
public:
    explicit DatabaseJsonParser(std::string_view input) : input_(input) {}

    bool parse(std::vector<DatabaseItem>& items, std::string& error) {
        skip_whitespace();
        if (!consume('{')) {
            return fail(error, "JSON 根节点必须是对象");
        }
        bool found_items = false;
        while (true) {
            skip_whitespace();
            if (consume('}')) {
                break;
            }
            std::string key;
            if (!read_string(key, error) || !expect(':', error)) {
                return false;
            }
            if (key == "databaseItems") {
                if (!read_items(items, error)) {
                    return false;
                }
                found_items = true;
            } else if (!skip_value(error)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) {
                break;
            }
            if (!expect(',', error)) {
                return false;
            }
        }
        if (!found_items) {
            return fail(error, "JSON 缺少 databaseItems 数组");
        }
        skip_whitespace();
        return position_ == input_.size() || fail(error, "JSON 末尾存在多余内容");
    }

private:
    std::string_view input_;
    std::size_t position_ = 0;

    void skip_whitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool consume(char c) {
        if (position_ < input_.size() && input_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }

    bool expect(char c, std::string& error) {
        if (consume(c)) {
            return true;
        }
        std::string message = "期望字符 ";
        message.push_back(c);
        return fail(error, message);
    }

    bool fail(std::string& error, const std::string& message) {
        error = message;
        return false;
    }

    bool read_string(std::string& out, std::string& error) {
        if (!consume('"')) {
            return fail(error, "期望字符串");
        }
        out.clear();
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c == '"') {
                ++position_;
                return true;
            }
            if (c == '\\') {
                ++position_;
                if (position_ >= input_.size()) {
                    break;
                }
                const char escaped = input_[position_++];
                switch (escaped) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    default:
                        // Rare escapes pass through; player names are ASCII.
                        out.push_back('\\');
                        out.push_back(escaped);
                        break;
                }
            } else {
                out.push_back(c);
                ++position_;
            }
        }
        return fail(error, "字符串未闭合");
    }

    bool read_items(std::vector<DatabaseItem>& items, std::string& error) {
        if (!expect('[', error)) {
            return false;
        }
        skip_whitespace();
        if (consume(']')) {
            return true;
        }
        while (true) {
            skip_whitespace();
            std::string ip;
            std::vector<std::string> names;
            if (!read_item(ip, names, error)) {
                return false;
            }
            if (names.empty()) {
                DatabaseItem item;
                std::snprintf(item.ip.data(), item.ip.size(), "%s", ip.c_str());
                items.push_back(item);
            } else {
                for (const std::string& name : names) {
                    DatabaseItem item;
                    std::snprintf(item.ip.data(), item.ip.size(), "%s", ip.c_str());
                    std::snprintf(item.player.data(), item.player.size(), "%s",
                                  name.c_str());
                    items.push_back(item);
                }
            }
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!expect(',', error)) {
                return false;
            }
        }
    }

    bool read_item(std::string& ip, std::vector<std::string>& names,
                   std::string& error) {
        if (!expect('{', error)) {
            return false;
        }
        skip_whitespace();
        if (consume('}')) {
            return true;
        }
        while (true) {
            std::string key;
            if (!read_string(key, error) || !expect(':', error)) {
                return false;
            }
            if (key == "IP") {
                if (!read_string(ip, error)) {
                    return false;
                }
            } else if (key == "playerNames") {
                if (!read_names(names, error)) {
                    return false;
                }
            } else if (!skip_value(error)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!expect(',', error)) {
                return false;
            }
        }
    }

    bool read_names(std::vector<std::string>& names, std::string& error) {
        if (!expect('[', error)) {
            return false;
        }
        skip_whitespace();
        if (consume(']')) {
            return true;
        }
        while (true) {
            std::string name;
            if (!read_string(name, error)) {
                return false;
            }
            names.push_back(std::move(name));
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!expect(',', error)) {
                return false;
            }
        }
    }

    bool skip_value(std::string& error) {
        skip_whitespace();
        if (position_ >= input_.size()) {
            return fail(error, "缺少值");
        }
        const char c = input_[position_];
        if (c == '{') {
            ++position_;
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            while (true) {
                std::string key;
                if (!read_string(key, error) || !expect(':', error)) {
                    return false;
                }
                if (!skip_value(error)) {
                    return false;
                }
                skip_whitespace();
                if (consume('}')) {
                    return true;
                }
                if (!expect(',', error)) {
                    return false;
                }
            }
        }
        if (c == '[') {
            ++position_;
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            while (true) {
                if (!skip_value(error)) {
                    return false;
                }
                skip_whitespace();
                if (consume(']')) {
                    return true;
                }
                if (!expect(',', error)) {
                    return false;
                }
            }
        }
        if (c == '"') {
            std::string dummy;
            return read_string(dummy, error);
        }
        // Numbers and literals run to the next structural character.
        while (position_ < input_.size() && input_[position_] != ',' &&
               input_[position_] != '}' && input_[position_] != ']') {
            ++position_;
        }
        return true;
    }
};

} // namespace

std::vector<DatabaseGroup> group_database_items(const std::vector<DatabaseItem>& items) {
    std::vector<DatabaseGroup> groups;
    std::map<std::string, std::size_t> positions;
    for (int index = 0; index < static_cast<int>(items.size()); ++index) {
        const std::string ip(items[static_cast<std::size_t>(index)].ip.data());
        const auto [found, inserted] = positions.emplace(ip, groups.size());
        if (inserted) {
            groups.push_back(DatabaseGroup{ip, {}});
        }
        groups[found->second].item_indices.push_back(index);
    }
    return groups;
}

std::vector<DatabaseItem> deduplicate_database_items(
    const std::vector<DatabaseItem>& existing,
    const std::vector<DatabaseItem>& incoming) {
    std::set<std::pair<std::string, std::string>> seen;
    for (const DatabaseItem& item : existing) {
        seen.emplace(item.ip.data(), item.player.data());
    }
    std::vector<DatabaseItem> deduped;
    for (const DatabaseItem& item : incoming) {
        if (seen.emplace(item.ip.data(), item.player.data()).second) {
            deduped.push_back(item);
        }
    }
    return deduped;
}

bool has_database_item(const std::vector<DatabaseItem>& items,
                       const std::string& ip, const std::string& player) {
    return std::any_of(items.begin(), items.end(),
                       [&ip, &player](const DatabaseItem& item) {
                           return ip == item.ip.data() && player == item.player.data();
                       });
}

std::set<std::string> duplicate_database_players(const std::vector<DatabaseItem>& items) {
    std::map<std::string, std::set<std::string>> player_ips;
    for (const DatabaseItem& item : items) {
        const std::string player(item.player.data());
        const std::string ip(item.ip.data());
        if (!player.empty() && !ip.empty()) {
            player_ips[player].insert(ip);
        }
    }

    std::set<std::string> duplicates;
    for (const auto& [player, ips] : player_ips) {
        if (ips.size() > 1) {
            duplicates.insert(player);
        }
    }
    return duplicates;
}

bool write_database_json(const std::string& path,
                         const std::vector<DatabaseItem>& items) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    const std::vector<DatabaseGroup> groups = group_database_items(items);
    file << "{\n";
    file << "    \"databaseItems\":[\n";
    for (std::size_t g = 0; g < groups.size(); ++g) {
        file << "        {\"IP\":\"" << json_escape(groups[g].ip)
             << "\",\"playerNames\":[";
        bool first = true;
        for (const int index : groups[g].item_indices) {
            const std::string player(items[static_cast<std::size_t>(index)].player.data());
            if (player.empty()) {
                continue;
            }
            if (!first) {
                file << ',';
            }
            file << '"' << json_escape(player) << '"';
            first = false;
        }
        file << "]}";
        if (g + 1 < groups.size()) {
            file << ',';
        }
        file << '\n';
    }
    file << "    ]\n";
    file << "}\n";
    return static_cast<bool>(file);
}

bool read_database_json(const std::string& path,
                        std::vector<DatabaseItem>& items,
                        std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "无法打开文件";
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    if (json.empty()) {
        error = "文件为空";
        return false;
    }
    return DatabaseJsonParser(json).parse(items, error);
}

bool read_login_log(const std::string& path,
                    std::vector<DatabaseItem>& items,
                    std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "无法打开文件";
        return false;
    }
    const std::string json((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    if (json.empty()) {
        error = "文件为空";
        return false;
    }
    return LoginLogParser(json).parse(items, error);
}

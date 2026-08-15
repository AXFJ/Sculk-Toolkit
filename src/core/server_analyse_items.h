#pragma once

#include "lan_monitor.h"
#include "utils/minecraft_protocol_versions.h"

#include <algorithm>
#include <cctype>
#include <string>

/// Analysis results derived from a LAN world record: every field is computed by
/// combining the LAN broadcast MOTD and the optional SLP status response.
struct ServerAnalyseItems {
    // ---- basic items ----------------------------------------------------------
    bool sa_is_available = false;           ///< Valid SLP information is present
    bool sa_is_vanilla_version = false;     ///< Version matches 1.xx.xx or xx.xx
    bool sa_is_lan_motd_clean = false;      ///< LAN MOTD has no color codes
    bool sa_is_motd_clean = false;          ///< MOTD has no color codes
    bool sa_is_regular_lan_motd = false;    ///< LAN MOTD matches "xxx - xxx" (colors OK)
    std::string sa_str_version;             ///< Game version extracted from version_name
    std::string sa_str_server_type;         ///< Server type before the version string
    short sa_int_actual_online = 0;         ///< Real online count from player list
    std::string sa_str_actual_server_version; ///< Version reversed from protocol number
    bool sa_is_default_port = false;        ///< Port is 25565
    bool sa_is_correct_online = false;      ///< Actual online equals reported online
    bool sa_is_correct_protocol = false;    ///< Actual server version equals parsed version
    short sa_int_lan_motd_color_char = 0;   ///< Count of § characters in LAN MOTD
    bool sa_is_motds_equal = false;         ///< LAN MOTD equals server MOTD

    // ---- advanced items -------------------------------------------------------
    bool sa_is_lan_world = false;           ///< Composite: correct_online && vanilla && regular_lan_motd && empty server type
};

namespace {

/// True when @p ch is a Minecraft formatting code character (0-9, a-f, k-o, r, x).
inline bool is_motd_color_char(char ch) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'k' && ch <= 'o') || ch == 'r' || ch == 'x';
}

/// Count § sequences in @p text: each § followed by a valid color/format char.
inline short count_color_chars(const std::string& text) {
    short count = 0;
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xC2 &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            // UTF-8 encoded § (C2 A7)
            if (i + 2 < text.size() && is_motd_color_char(text[i + 2])) {
                ++count;
            }
        } else if (text[i] == '\xA7' || text[i] == '&') {
            // Raw § or alternate &
            if (is_motd_color_char(text[i + 1])) {
                ++count;
            }
        }
    }
    return count;
}

/// True when @p text contains no § / & formatting codes.
inline bool is_motd_clean(const std::string& text) {
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        if (static_cast<unsigned char>(text[i]) == 0xC2 &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            if (i + 2 < text.size() && is_motd_color_char(text[i + 2])) return false;
        } else if ((text[i] == '\xA7' || text[i] == '&') &&
                   is_motd_color_char(text[i + 1])) {
            return false;
        }
    }
    return true;
}

/// Returns true when @p version matches "1.xx.xx" or "xx.xx".
inline bool is_vanilla_version_format(const std::string& version) {
    if (version.empty()) return false;
    std::size_t dot1 = version.find('.');
    if (dot1 == std::string::npos || dot1 == 0) return false;
    std::size_t dot2 = version.find('.', dot1 + 1);
    if (dot2 == std::string::npos) {
        // "xx.xx" format — two-part
        return version.find_first_not_of("0123456789.", dot1 + 1) == std::string::npos;
    }
    // "1.xx.xx" format — three-part
    return version.find_first_not_of("0123456789.", dot1 + 1) == std::string::npos;
}

/// Extracts a version string like "1.xx.xx" or "xx.xx" from @p text.
inline std::string extract_version(const std::string& text) {
    // Look for a digit followed by more digits, dot, digits pattern.
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) {
            // Try matching: digits . digits [. digits]
            std::size_t j = i;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
            if (j < text.size() && text[j] == '.') {
                std::size_t k = j + 1;
                while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
                if (k > j + 1) {
                    // Found at least x.y
                    if (k < text.size() && text[k] == '.') {
                        std::size_t l = k + 1;
                        while (l < text.size() && std::isdigit(static_cast<unsigned char>(text[l]))) ++l;
                        if (l > k + 1) {
                            return text.substr(i, l - i);
                        }
                    }
                    return text.substr(i, k - i);
                }
            }
        }
    }
    return {};
}

/// Extracts the server type before the version string (e.g. "Paper" from "Paper 1.21.4").
inline std::string extract_server_type(const std::string& text) {
    std::string version = extract_version(text);
    if (version.empty()) return {};
    auto pos = text.find(version);
    if (pos == std::string::npos || pos == 0) return {};
    std::string before = text.substr(0, pos);
    // Trim trailing spaces, hyphens, and common delimiters.
    while (!before.empty() && (before.back() == ' ' || before.back() == '-' ||
                                before.back() == '/' || before.back() == '(' ||
                                before.back() == '[')) {
        before.pop_back();
    }
    return before;
}

/// True when LAN MOTD matches "xxx - xxx" (allowing color codes).
inline bool is_regular_lan_motd_format(const std::string& lan_motd) {
    if (lan_motd.empty()) return false;
    // Strip § and & codes for the structural check.
    std::string stripped;
    stripped.reserve(lan_motd.size());
    for (std::size_t i = 0; i < lan_motd.size(); ++i) {
        if (static_cast<unsigned char>(lan_motd[i]) == 0xC2 &&
            i + 2 < lan_motd.size() &&
            static_cast<unsigned char>(lan_motd[i + 1]) == 0xA7 &&
            is_motd_color_char(lan_motd[i + 2])) {
            i += 2;
            continue;
        }
        if ((lan_motd[i] == '\xA7' || lan_motd[i] == '&') &&
            i + 1 < lan_motd.size() && is_motd_color_char(lan_motd[i + 1])) {
            ++i;
            continue;
        }
        stripped.push_back(lan_motd[i]);
    }
    // Check for " - " separator.
    auto dash = stripped.find(" - ");
    return dash != std::string::npos && dash > 0 && dash + 3 < stripped.size();
}

/// Compare two MOTD strings ignoring color codes.
inline bool motds_equal_ignore_colors(const std::string& a, const std::string& b) {
    auto strip = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (static_cast<unsigned char>(s[i]) == 0xC2 &&
                i + 2 < s.size() &&
                static_cast<unsigned char>(s[i + 1]) == 0xA7 &&
                is_motd_color_char(s[i + 2])) {
                i += 2;
                continue;
            }
            if ((s[i] == '\xA7' || s[i] == '&') &&
                i + 1 < s.size() && is_motd_color_char(s[i + 1])) {
                ++i;
                continue;
            }
            out.push_back(s[i]);
        }
        return out;
    };
    return strip(a) == strip(b);
}

} // anonymous namespace

/// Compute every analysis item from a single LAN world record.
inline ServerAnalyseItems compute_server_analyse_items(const LanWorldRecord& record) {
    ServerAnalyseItems items;
    const LanWorld& world = record.world;
    const auto& status = world.status;

    // ---- availability ----
    items.sa_is_available = status.has_value() && status->available;

    // ---- default port ----
    items.sa_is_default_port = (world.port == 25565);

    // ---- LAN MOTD colour chars and cleanness ----
    items.sa_int_lan_motd_color_char = count_color_chars(world.lan_motd);
    items.sa_is_lan_motd_clean = is_motd_clean(world.lan_motd);

    // ---- regular LAN MOTD ----
    items.sa_is_regular_lan_motd = is_regular_lan_motd_format(world.lan_motd);

    if (items.sa_is_available) {
        // ---- version / server type ----
        items.sa_str_version = extract_version(status->version_name);
        items.sa_str_server_type = extract_server_type(status->version_name);
        items.sa_is_vanilla_version = is_vanilla_version_format(status->version_name);

        // ---- actual server version (from protocol) ----
        items.sa_str_actual_server_version = getVersionByProtocol(status->protocol_version);

        // ---- correct protocol ----
        items.sa_is_correct_protocol =
            !items.sa_str_version.empty() &&
            items.sa_str_version == items.sa_str_actual_server_version;

        // ---- actual online ----
        items.sa_int_actual_online = static_cast<short>(status->player_names.size());

        // ---- correct online ----
        items.sa_is_correct_online =
            (items.sa_int_actual_online == static_cast<short>(status->online_players));

        // ---- MOTD clean ----
        items.sa_is_motd_clean = is_motd_clean(status->motd);

        // ---- MOTDs equal ----
        items.sa_is_motds_equal = motds_equal_ignore_colors(world.lan_motd, status->motd);
    }

    // ---- composite: LAN world ----
    // A real LAN world has no extra server-type tag before its version string.
    items.sa_is_lan_world =
        items.sa_is_correct_online &&
        items.sa_is_vanilla_version &&
        items.sa_is_regular_lan_motd &&
        items.sa_str_server_type.empty();

    return items;
}

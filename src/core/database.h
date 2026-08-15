#pragma once

#include <array>
#include <set>
#include <string>
#include <vector>

/// One manual IP-to-player-name mapping entry of the database tab.
struct DatabaseItem {
    std::array<char, 64> ip{};
    std::array<char, 128> player{};
};

/// Items that share an IP, in first-seen order; used to render the merged IP cell.
struct DatabaseGroup {
    std::string ip;
    std::vector<int> item_indices;
};

// Groups items by IP, preserving first-seen order of the IPs themselves.
std::vector<DatabaseGroup> group_database_items(const std::vector<DatabaseItem>& items);

// Player names that map to more than one distinct IP, used for the duplicate
// warning. Each name appears once.
std::set<std::string> duplicate_database_players(const std::vector<DatabaseItem>& items);

// Writes the mapping table to @p path as
// {"databaseItems":[{"IP":"...","playerNames":["steve","steve2"]}]}, grouped by
// IP with each IP appearing once. Returns false when the file cannot be written.
bool write_database_json(const std::string& path, const std::vector<DatabaseItem>& items);

// Reads the same format back into flat items (one entry per player name; an IP
// with no players yields a single entry with an empty player name). Unknown
// keys are skipped. Returns false and fills @p error on failure.
bool read_database_json(const std::string& path, std::vector<DatabaseItem>& items,
                        std::string& error);

// Reads a fake-server log file ({"fakeserverLogItems":[...]}) and extracts the
// login entries as ip/player pairs. Entries without a player name are skipped.
// Returns false and fills @p error on failure.
bool read_login_log(const std::string& path, std::vector<DatabaseItem>& items,
                    std::string& error);

// Merges @p incoming into the table: entries whose (IP, player) pair already
// exists in @p existing, or repeats an earlier pair within @p incoming, are
// dropped. Returns the entries that should be appended.
std::vector<DatabaseItem> deduplicate_database_items(
    const std::vector<DatabaseItem>& existing,
    const std::vector<DatabaseItem>& incoming);

// True when an item with the same (IP, player) pair already exists in @p items.
bool has_database_item(const std::vector<DatabaseItem>& items,
                       const std::string& ip, const std::string& player);

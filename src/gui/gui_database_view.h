#pragma once

#include "core/database.h"
#include "gui_status_bar.h"

#include <array>
#include <string>
#include <vector>

namespace gui_view {

enum class DatabaseSelection {
    None,
    Ip,
    Player
};

struct DatabasePanelState {
    std::array<char, 64> ip_input{};
    std::array<char, 128> player_input{};
    std::vector<DatabaseItem> items;
    int selected_index = -1;
    DatabaseSelection selection = DatabaseSelection::None;
    int editing_index = -1;
    DatabaseSelection editing = DatabaseSelection::None;
    std::array<char, 64> editing_ip{};
    std::string editing_original_ip;
    // Import/export path.
    std::array<char, 512> path_input{};
    gui_status::Bar status;
    // Log import awaiting the confirmation dialog.
    std::vector<DatabaseItem> import_items;
    int import_total = 0;  // login events found in the log (before dedup)
    bool import_confirm_open = false;
    // Find request: the item index to jump to and highlight (-1 = none), and a
    // flag that scrolls to that row once.
    int find_index = -1;
    bool find_scroll = false;
};

// Draws the manually maintained IP-to-player-name mapping table.
void draw_database_panel(DatabasePanelState& state, float dpi_scale);

} // namespace gui_view

#include "gui_database_view.h"

#include "core/database.h"
#include "gui_dialog.h"
#include "gui_text.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gui_view {

namespace {

bool input_present(const char* text) {
    return text != nullptr && text[0] != '\0';
}

// Saves the current table to the path field, or asks for one when it is empty.
void save_database(DatabasePanelState& state) {
    std::string path(state.path_input.data());
    if (path.empty()) {
        const std::wstring chosen = gui_dialog::save_json_file(L"database.db.json");
        if (chosen.empty()) {
            return;
        }
        path = gui_dialog::to_utf8(chosen);
        std::snprintf(state.path_input.data(), state.path_input.size(), "%s",
                      path.c_str());
    }
    if (write_database_json(path, state.items)) {
        // Show just the file name so the green notice stays short.
        const std::size_t slash = path.find_last_of("/\\");
        const std::string name = slash == std::string::npos
            ? path
            : path.substr(slash + 1);
        char message[1024]{};
        std::snprintf(message, sizeof(message), gui_text::database_save_ok,
                      name.c_str());
        gui_status::show(state.status, message);
    } else {
        char message[1024]{};
        std::snprintf(message, sizeof(message), gui_text::database_io_error,
                      "无法写入文件");
        gui_status::show_error(state.status, message);
    }
}

// Loads the table from the path field, or asks for one when it is empty.
void open_database(DatabasePanelState& state) {
    std::string path(state.path_input.data());
    if (path.empty()) {
        const std::wstring chosen = gui_dialog::open_json_file();
        if (chosen.empty()) {
            return;
        }
        path = gui_dialog::to_utf8(chosen);
        std::snprintf(state.path_input.data(), state.path_input.size(), "%s",
                      path.c_str());
    }
    std::vector<DatabaseItem> loaded;
    std::string parse_error;
    if (read_database_json(path, loaded, parse_error)) {
        state.items = std::move(loaded);
        state.selected_index = -1;
        state.selection = DatabaseSelection::None;
        state.editing_index = -1;
        state.editing = DatabaseSelection::None;
        state.editing_original_ip.clear();
        // Show just the file name so the green notice stays short.
        const std::size_t slash = path.find_last_of("/\\");
        const std::string name = slash == std::string::npos
            ? path
            : path.substr(slash + 1);
        char message[1024]{};
        std::snprintf(message, sizeof(message), gui_text::database_open_ok,
                      name.c_str());
        gui_status::show(state.status, message);
    } else {
        char message[1024]{};
        std::snprintf(message, sizeof(message), gui_text::database_io_error,
                      parse_error.c_str());
        gui_status::show_error(state.status, message);
    }
}

// Reads the chosen log file's login events into the pending import list and
// asks the user to confirm the count. Entries whose (IP, player) pair already
// exists in the table, or repeats within the log, are deduplicated.
void start_log_import(DatabasePanelState& state) {
    const std::wstring chosen = gui_dialog::open_json_file();
    if (chosen.empty()) {
        return;
    }
    std::vector<DatabaseItem> loaded;
    std::string parse_error;
    if (read_login_log(gui_dialog::to_utf8(chosen), loaded, parse_error)) {
        if (loaded.empty()) {
            gui_status::show_error(state.status, gui_text::database_import_none);
        } else {
            state.import_total = static_cast<int>(loaded.size());
            state.import_items = deduplicate_database_items(state.items, loaded);
            state.import_confirm_open = true;
        }
    } else {
        char message[1024]{};
        std::snprintf(message, sizeof(message), gui_text::database_io_error,
                      parse_error.c_str());
        gui_status::show_error(state.status, message);
    }
}

// Implements the 查找 button. IP-only searches the IP column; otherwise the
// player name is searched (optionally within the typed IP). The first match is
// highlighted and the table scrolls to it; the status line reports the count.
void find_database(DatabasePanelState& state) {
    const std::string ip(state.ip_input.data());
    const std::string player(state.player_input.data());
    const std::vector<DatabaseGroup> groups = group_database_items(state.items);

    int count = 0;
    int first_index = -1;
    DatabaseSelection highlight = DatabaseSelection::None;

    if (ip.empty() && !player.empty()) {
        // Player search: every item whose player matches (possibly several IPs).
        for (std::size_t i = 0; i < state.items.size(); ++i) {
            if (player == state.items[i].player.data()) {
                ++count;
                if (first_index < 0) {
                    first_index = static_cast<int>(i);
                    highlight = DatabaseSelection::Player;
                }
            }
        }
    } else if (!ip.empty()) {
        // IP search, optionally narrowed by a player name.
        for (std::size_t g = 0; g < groups.size(); ++g) {
            if (groups[g].ip != ip) {
                continue;
            }
            if (player.empty()) {
                // One match for the IP itself.
                first_index = groups[g].item_indices.front();
                highlight = DatabaseSelection::Ip;
                count = 1;
                break;
            }
            for (const int index : groups[g].item_indices) {
                if (player == state.items[static_cast<std::size_t>(index)].player.data()) {
                    ++count;
                    if (first_index < 0) {
                        first_index = index;
                        highlight = DatabaseSelection::Player;
                    }
                }
            }
            break;
        }
    }

    if (count == 0) {
        gui_status::show_error(state.status, gui_text::database_find_none);
        state.find_index = -1;
        return;
    }

    state.find_index = first_index;
    state.find_scroll = true;
    state.selected_index = first_index;
    state.selection = highlight;
    char message[64]{};
    std::snprintf(message, sizeof(message), gui_text::database_find_count, count);
    gui_status::show(state.status, message);
}

// Left column: the mapping editor.
void draw_database_editor(DatabasePanelState& state, float ip_width,
                          float player_width) {
    // Row one: new mapping inputs and add button.
    ImGui::SetNextItemWidth(ip_width);
    ImGui::InputTextWithHint("##database_ip", gui_text::database_ip,
                             state.ip_input.data(), state.ip_input.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(player_width);
    ImGui::InputTextWithHint("##database_player", gui_text::database_player,
                             state.player_input.data(), state.player_input.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!input_present(state.ip_input.data()) ||
                         !input_present(state.player_input.data()));
    if (ImGui::Button(gui_text::database_add)) {
        if (has_database_item(state.items, state.ip_input.data(),
                              state.player_input.data())) {
            gui_status::show_error(state.status, gui_text::database_add_duplicate);
        } else {
            DatabaseItem item;
            std::snprintf(item.ip.data(), item.ip.size(), "%s", state.ip_input.data());
            std::snprintf(item.player.data(), item.player.size(), "%s",
                          state.player_input.data());
            state.items.push_back(item);
            state.ip_input.fill('\0');
            state.player_input.fill('\0');
            state.selected_index = static_cast<int>(state.items.size()) - 1;
            state.selection = DatabaseSelection::Player;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!input_present(state.ip_input.data()) &&
                         !input_present(state.player_input.data()));
    if (ImGui::Button(gui_text::database_find)) {
        find_database(state);
    }
    ImGui::EndDisabled();

    // Row two: operations on the selected mapping.
    const bool valid_selection =
        state.selected_index >= 0 &&
        state.selected_index < static_cast<int>(state.items.size());
    ImGui::BeginDisabled(!valid_selection);
    if (ImGui::Button(gui_text::database_delete)) {
        if (state.selection == DatabaseSelection::Ip) {
            const std::string ip(
                state.items[static_cast<std::size_t>(state.selected_index)].ip.data());
            state.items.erase(
                std::remove_if(state.items.begin(), state.items.end(),
                               [&ip](const DatabaseItem& item) {
                                   return ip == item.ip.data();
                               }),
                state.items.end());
        } else {
            state.items.erase(state.items.begin() + state.selected_index);
        }
        state.selected_index = -1;
        state.selection = DatabaseSelection::None;
        state.editing_index = -1;
        state.editing = DatabaseSelection::None;
    }
    ImGui::SameLine();
    if (ImGui::Button(gui_text::database_edit)) {
        state.editing_index = state.selected_index;
        state.editing = state.selection;
        if (state.editing == DatabaseSelection::Ip) {
            state.editing_original_ip =
                state.items[static_cast<std::size_t>(state.selected_index)].ip.data();
            std::snprintf(state.editing_ip.data(), state.editing_ip.size(), "%s",
                          state.editing_original_ip.c_str());
        }
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    const std::vector<DatabaseGroup> groups = group_database_items(state.items);
    const std::set<std::string> duplicate_names =
        duplicate_database_players(state.items);
    // One footer line always holds the totals (plus the duplicate warning on
    // the same line when any exist).
    const float footer_height = ImGui::GetTextLineHeightWithSpacing();
    const float id_width = ImGui::CalcTextSize("000").x +
                           ImGui::GetStyle().CellPadding.x * 2.0F;

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("database_table", 3, flags,
                           ImVec2(0.0F, -footer_height))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(gui_text::database_id,
                            ImGuiTableColumnFlags_WidthFixed, id_width);
    ImGui::TableSetupColumn(gui_text::database_ip,
                            ImGuiTableColumnFlags_WidthFixed, ip_width);
    ImGui::TableSetupColumn(gui_text::database_player,
                            ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    int display_id = 1;
    for (const DatabaseGroup& group : groups) {
        const float row_height = ImGui::GetFrameHeightWithSpacing() *
                                 static_cast<float>(group.item_indices.size());
        ImGui::PushID(group.ip.c_str());
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(std::to_string(display_id++).c_str());
        ImGui::TableNextColumn();

        const bool ip_selected =
            state.selection == DatabaseSelection::Ip &&
            state.selected_index >= 0 &&
            std::find(group.item_indices.begin(), group.item_indices.end(),
                      state.selected_index) != group.item_indices.end();
        const bool ip_editing =
            state.editing == DatabaseSelection::Ip &&
            state.editing_original_ip == group.ip;
        bool ip_committed = false;
        bool ip_hovered = false;
        if (ip_editing) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ip_committed = ImGui::InputText(
                "##edit_ip", state.editing_ip.data(), state.editing_ip.size(),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ip_hovered = ImGui::IsItemHovered();
        } else if (ImGui::Selectable(group.ip.c_str(), ip_selected)) {
            state.selected_index = group.item_indices.front();
            state.selection = DatabaseSelection::Ip;
        }

        ImGui::TableNextColumn();
        bool player_committed = false;
        bool player_hovered = false;
        for (const int index : group.item_indices) {
            DatabaseItem& item = state.items[static_cast<std::size_t>(index)];
            ImGui::PushID(index);
            const bool player_editing =
                state.editing == DatabaseSelection::Player &&
                state.editing_index == index;
            if (player_editing) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                player_committed = ImGui::InputText(
                    "##edit_player", item.player.data(), item.player.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                player_hovered = ImGui::IsItemHovered();
            } else {
                const std::string player(item.player.data());
                const bool duplicate = duplicate_names.contains(player);
                if (duplicate) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImVec4(1.0F, 0.82F, 0.20F, 1.0F));
                }
                const bool player_selected =
                    state.selection == DatabaseSelection::Player &&
                    state.selected_index == index;
                if (ImGui::Selectable(item.player.data(), player_selected)) {
                    state.selected_index = index;
                    state.selection = DatabaseSelection::Player;
                }
                if (duplicate && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", gui_text::database_duplicate_player);
                }
                if (duplicate) {
                    ImGui::PopStyleColor();
                }
            }
            ImGui::PopID();
        }

        const bool editing_this_group = ip_editing ||
            (state.editing == DatabaseSelection::Player &&
             std::find(group.item_indices.begin(), group.item_indices.end(),
                       state.editing_index) != group.item_indices.end());
        if (editing_this_group) {
            const bool clicked_elsewhere =
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ip_hovered && !player_hovered;
            if (ip_committed || player_committed || clicked_elsewhere) {
                if (ip_editing) {
                    for (DatabaseItem& mapped : state.items) {
                        if (state.editing_original_ip == mapped.ip.data()) {
                            std::snprintf(mapped.ip.data(), mapped.ip.size(), "%s",
                                          state.editing_ip.data());
                        }
                    }
                }
                state.editing_index = -1;
                state.editing = DatabaseSelection::None;
                state.editing_original_ip.clear();
            }
        }

        // Scroll to the row the user searched for, once. SetScrollHereY() takes
        // effect from the last submitted item, so it runs after the cells.
        if (state.find_scroll &&
            std::find(group.item_indices.begin(), group.item_indices.end(),
                      state.find_index) != group.item_indices.end()) {
            ImGui::SetScrollHereY(0.5F);
            state.find_scroll = false;
        }
        ImGui::PopID();
    }

    ImGui::EndTable();

    // Footer: the IP/player totals always show; the duplicate warning shares the
    // line when any duplicate players exist.
    const int total_players = std::count_if(
        state.items.begin(), state.items.end(),
        [](const DatabaseItem& item) { return item.player[0] != '\0'; });
    ImGui::Text(gui_text::database_total, static_cast<int>(groups.size()),
                total_players);
    if (!duplicate_names.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0F, 0.82F, 0.20F, 1.0F),
                           gui_text::database_duplicate_warning,
                           static_cast<int>(duplicate_names.size()));
    }
}

// Right column: import/export controls.
void draw_database_io(DatabasePanelState& state, float dpi_scale) {
    // Row one: path input plus a browse button.
    const float browse_width = ImGui::GetFrameHeight() * 1.5F;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_width -
                            ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##database_path", gui_text::database_path_hint,
                             state.path_input.data(), state.path_input.size());
    ImGui::SameLine();
    if (ImGui::Button(gui_text::database_path_button)) {
        // The open dialog (not save-as) is used so an existing file is picked
        // without an "overwrite?" prompt.
        const std::wstring chosen = gui_dialog::open_json_file();
        if (!chosen.empty()) {
            const std::string narrow = gui_dialog::to_utf8(chosen);
            std::snprintf(state.path_input.data(), state.path_input.size(), "%s",
                          narrow.c_str());
        }
    }

    // Row two: save and open, both defaulting to the path field.
    if (ImGui::Button(gui_text::database_save)) {
        save_database(state);
    }
    ImGui::SameLine();
    if (ImGui::Button(gui_text::database_open)) {
        open_database(state);
    }

    // Row three: import login events from a fake-server log file.
    ImGui::SeparatorText(gui_text::database_import_section);
    if (ImGui::Button(gui_text::database_import_pick)) {
        start_log_import(state);
    }

    // Confirmation for a pending log import.
    if (state.import_confirm_open) {
        ImGui::OpenPopup(gui_text::database_import_confirm_title);
        state.import_confirm_open = false;
    }
    if (ImGui::BeginPopupModal(gui_text::database_import_confirm_title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text(gui_text::database_import_confirm_format,
                    static_cast<int>(state.import_items.size()));
        if (ImGui::Button(gui_text::database_import_yes)) {
            const int imported = static_cast<int>(state.import_items.size());
            const int duplicates = std::max(0, state.import_total - imported);
            state.items.insert(state.items.end(), state.import_items.begin(),
                               state.import_items.end());
            char message[128]{};
            std::snprintf(message, sizeof(message), gui_text::database_import_result,
                          imported, duplicates);
            gui_status::show(state.status, message);
            state.import_items.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(gui_text::database_import_no)) {
            state.import_items.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

void draw_database_panel(DatabasePanelState& state, float dpi_scale) {
    const float ip_width = 190.0F * dpi_scale;
    const float player_width = 240.0F * dpi_scale;
    const float right_width = 320.0F * dpi_scale;

    // The split table lives in a child that leaves a separator plus one line at
    // the bottom for the status/error message, so the message is always visible
    // even while the table content is being rebuilt.
    const float status_height = ImGui::GetTextLineHeightWithSpacing() +
                                ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("database_split_host", ImVec2(0.0F, -status_height));

    const float split_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("database_split", 2,
                          ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_Resizable,
                          ImVec2(0.0F, split_height))) {
        ImGui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthFixed,
                                right_width);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, split_height);

        ImGui::TableNextColumn();
        draw_database_editor(state, ip_width, player_width);

        ImGui::TableNextColumn();
        draw_database_io(state, dpi_scale);

        ImGui::EndTable();
    }
    ImGui::EndChild();

    gui_status::draw(state.status);
}

} // namespace gui_view

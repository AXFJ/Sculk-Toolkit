#include "gui_scan_columns_dialog.h"

#include "gui_text.h"

#include "imgui.h"

namespace gui_view {

namespace {
    constexpr int motd_columns = 60;
    constexpr int ip_columns = 15;
    constexpr int port_columns = 5;
    constexpr int players_columns = 6;
    constexpr int version_columns = 16;
    constexpr int player_list_columns = 30;
} // namespace

// Analysis item names, as shown in the column picker.
const char* analyse_label(int column) {
    switch (static_cast<gui_view::ScanColumn>(column)) {
        case gui_view::ScanColumn::LanWorld: return gui_text::column_lan_world;
        case gui_view::ScanColumn::IsAvailable: return gui_text::column_is_available;
        case gui_view::ScanColumn::IsVanillaVersion:
            return gui_text::column_is_vanilla_version;
        case gui_view::ScanColumn::IsLanMotdClean:
            return gui_text::column_is_lan_motd_clean;
        case gui_view::ScanColumn::IsMotdClean: return gui_text::column_is_motd_clean;
        case gui_view::ScanColumn::IsRegularLanMotd:
            return gui_text::column_is_regular_lan_motd;
        case gui_view::ScanColumn::StrVersion: return gui_text::column_str_version;
        case gui_view::ScanColumn::StrServerType: return gui_text::column_str_server_type;
        case gui_view::ScanColumn::IntActualOnline:
            return gui_text::column_int_actual_online;
        case gui_view::ScanColumn::StrActualServerVersion:
            return gui_text::column_str_actual_server_version;
        case gui_view::ScanColumn::IsDefaultPort: return gui_text::column_is_default_port;
        case gui_view::ScanColumn::IsCorrectOnline: return gui_text::column_is_correct_online;
        case gui_view::ScanColumn::IsCorrectProtocol:
            return gui_text::column_is_correct_protocol;
        case gui_view::ScanColumn::IntLanMotdColorChar:
            return gui_text::column_int_lan_motd_color_char;
        default: return gui_text::column_is_motds_equal;
    }
}

// The field identifier appended to analysis labels in the picker.
const char* analyse_id(int column) {
    switch (static_cast<gui_view::ScanColumn>(column)) {
        case gui_view::ScanColumn::LanWorld: return "(sa_is_lan_world)";
        case gui_view::ScanColumn::IsAvailable: return "(sa_is_available)";
        case gui_view::ScanColumn::IsVanillaVersion: return "(sa_is_vanilla_version)";
        case gui_view::ScanColumn::IsLanMotdClean: return "(sa_is_lan_motd_clean)";
        case gui_view::ScanColumn::IsMotdClean: return "(sa_is_motd_clean)";
        case gui_view::ScanColumn::IsRegularLanMotd: return "(sa_is_regular_lan_motd)";
        case gui_view::ScanColumn::StrVersion: return "(sa_str_version)";
        case gui_view::ScanColumn::StrServerType: return "(sa_str_server_type)";
        case gui_view::ScanColumn::IntActualOnline: return "(sa_int_actual_online)";
        case gui_view::ScanColumn::StrActualServerVersion:
            return "(sa_str_actual_server_version)";
        case gui_view::ScanColumn::IsDefaultPort: return "(sa_is_default_port)";
        case gui_view::ScanColumn::IsCorrectOnline: return "(sa_is_correct_online)";
        case gui_view::ScanColumn::IsCorrectProtocol: return "(sa_is_correct_protocol)";
        case gui_view::ScanColumn::IntLanMotdColorChar:
            return "(sa_int_lan_motd_color_char)";
        default: return "(sa_is_motds_equal)";
    }
}

// True when the column is one of the analysis item columns.
bool is_analyse_column(int column) {
    return column >= static_cast<int>(gui_view::ScanColumn::LanWorld);
}

// True when the analysis column renders a boolean ✓/✗ instead of text.
bool is_analyse_bool_column(int column) {
    switch (static_cast<gui_view::ScanColumn>(column)) {
        case gui_view::ScanColumn::StrVersion:
        case gui_view::ScanColumn::StrServerType:
        case gui_view::ScanColumn::IntActualOnline:
        case gui_view::ScanColumn::StrActualServerVersion:
        case gui_view::ScanColumn::IntLanMotdColorChar:
            return false;
        default:
            return true;
    }
}

ColumnInfo column_info(int column) {
    switch (static_cast<gui_view::ScanColumn>(column)) {
        case gui_view::ScanColumn::Motd: return {gui_text::column_motd, motd_columns};
        case gui_view::ScanColumn::Ip: return {gui_text::column_ip, ip_columns};
        case gui_view::ScanColumn::Port: return {gui_text::column_port, port_columns};
        case gui_view::ScanColumn::Players: return {gui_text::column_players, players_columns};
        case gui_view::ScanColumn::Version: return {gui_text::column_version, version_columns};
        case gui_view::ScanColumn::PlayerList:
            return {gui_text::column_player_list, player_list_columns};
        case gui_view::ScanColumn::Latency: return {gui_text::column_latency, 1};
        default:
            if (is_analyse_column(column)) {
                // Boolean analysis items are a single ✓/✗ character; the rest
                // are wider text/numbers.
                return {analyse_label(column), is_analyse_bool_column(column) ? 1 : 10};
            }
            return {gui_text::column_latency, 1};
    }
}

void draw_columns_window(ScanPanelState& state, float dpi_scale) {
    if (!state.columns_open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(300.0F * dpi_scale, 330.0F * dpi_scale),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(gui_text::scan_columns_title, &state.columns_open)) {
        ImGui::End();
        return;
    }

    // Reserve a fixed footer for the close button; the content child scrolls so
    // all 22 rows stay reachable.
    const float footer_height = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("columns_content", ImVec2(0.0F, -footer_height));

    constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_Borders;
    if (ImGui::BeginTable("columns_picker", 2, table_flags)) {
        ImGui::TableSetupColumn("##drag", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFrameHeight() * 1.6F);
        ImGui::TableSetupColumn(gui_text::scan_columns_title,
                                ImGuiTableColumnFlags_WidthStretch);

        const int count = static_cast<int>(state.column_order.size());
        for (int index = 0; index < count; ++index) {
            const int column = state.column_order[index];
            if (column < 0 || column >= static_cast<int>(gui_view::ScanColumn::Count)) {
                continue;
            }

            ImGui::PushID(column);
            ImGui::TableNextRow();

            // Analysis columns get a dark-gray background; the identifier after
            // the Chinese name is drawn in gray text.
            const bool is_analysis_col = is_analyse_column(column);
            if (is_analysis_col) {
                ImGui::TableSetBgColor(
                    ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImVec4(0.20F, 0.20F, 0.22F, 0.60F)));
            }

            // Drag handle: a button filling the column that serves as the drag source.
            ImGui::TableNextColumn();
            ImGui::Button("≡", ImVec2(-FLT_MIN, 0.0F));
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("COLUMN_ORDER", &index, sizeof(int));
                ImGui::TextUnformatted(column_info(column).label);
                ImGui::EndDragDropSource();
            }

            // Drag target: dropping on the handle or the label row swaps positions.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("COLUMN_ORDER")) {
                    const int source = *static_cast<const int*>(payload->Data);
                    if (source >= 0 && source < count && source != index) {
                        std::swap(state.column_order[source], state.column_order[index]);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Label and checkbox column. Analysis rows show the short name, then
            // the field identifier in gray; ordinary columns show their label.
            ImGui::TableNextColumn();
            bool visible = state.column_visible[static_cast<std::size_t>(column)];
            if (ImGui::Checkbox(column_info(column).label, &visible)) {
                state.column_visible[static_cast<std::size_t>(column)] = visible;
            }
            if (is_analysis_col) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55F, 0.55F, 0.60F, 1.0F));
                ImGui::TextUnformatted(analyse_id(column));
                ImGui::PopStyleColor();
            }

            // Also accept drops on the label column.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("COLUMN_ORDER")) {
                    const int source = *static_cast<const int*>(payload->Data);
                    if (source >= 0 && source < count && source != index) {
                        std::swap(state.column_order[source], state.column_order[index]);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (ImGui::Button(gui_text::scan_columns_close)) {
        state.columns_open = false;
    }
    ImGui::End();
}

} // namespace gui_view

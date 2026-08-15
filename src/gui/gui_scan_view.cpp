#include "gui_scan_view.h"

#include "core/server_analyse_items.h"
#include "dialog/gui_analyse_details_dialog.h"
#include "dialog/gui_scan_columns_dialog.h"
#include "gui_fonts.h"
#include "gui_motd.h"
#include "gui_text.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

// 0 means continuous; the rest are the presets offered by the combo.
constexpr std::array<int, 8> duration_presets{0, 5, 10, 15, 30, 60, 120, 300};

// Column budgets in monospace character cells, per the UI specification.
constexpr int motd_columns = 60;
constexpr int ip_columns = 15;
constexpr int port_columns = 5;
constexpr int players_columns = 6;
constexpr int version_columns = 16;
constexpr int player_list_columns = 30;

using Endpoint = std::pair<std::string, unsigned short>;

// 0 disables the automatic re-query; the rest are the offered intervals.
constexpr std::array<int, 6> auto_slp_presets{0, 10, 30, 60, 120, 300};

std::string duration_label(int seconds) {
    if (seconds == 0) {
        return gui_text::duration_continuous;
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), gui_text::duration_seconds_format, seconds);
    return text;
}

std::string auto_slp_label(int seconds) {
    if (seconds == 0) {
        return gui_text::auto_slp_never;
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), gui_text::duration_seconds_format, seconds);
    return text;
}

// Empty or unparsable input means a continuous scan.
int typed_duration(const std::array<char, 8>& input) {
    int seconds = 0;
    for (const char character : input) {
        if (character == '\0') {
            break;
        }
        if (character < '0' || character > '9') {
            return 0;
        }
        seconds = std::min(seconds * 10 + (character - '0'), 3600);
    }
    return seconds;
}

std::size_t sequence_length(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;
}

std::size_t codepoint_columns(const char* text, std::size_t length) {
    unsigned int codepoint = 0;
    if (length == 1) {
        codepoint = static_cast<unsigned char>(text[0]);
    } else {
        codepoint = static_cast<unsigned char>(text[0]) & (0xFFU >> (length + 1));
        for (std::size_t index = 1; index < length; ++index) {
            codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[index]) & 0x3FU);
        }
    }
    // CJK and other wide glyphs occupy two monospace cells.
    return codepoint >= 0x2E80 ? 2 : 1;
}

// Draws text clipped to the cell it sits in: it grows to the column edge and
// only then ends in three dots.
void draw_clipped_text(const std::string& text) {
    const float available = ImGui::GetContentRegionAvail().x;
    if (text.empty() || ImGui::CalcTextSize(text.c_str()).x <= available) {
        ImGui::TextUnformatted(text.c_str());
        return;
    }

    const float dots = ImGui::CalcTextSize("...").x;
    std::string clipped;
    float used = 0.0F;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t length = std::min(sequence_length(
            static_cast<unsigned char>(text[index])), text.size() - index);
        const std::string glyph = text.substr(index, length);
        const float glyph_width = ImGui::CalcTextSize(glyph.c_str()).x;
        if (used + glyph_width + dots > available) {
            break;
        }
        clipped += glyph;
        used += glyph_width;
        index += length;
    }
    ImGui::TextUnformatted((clipped + "...").c_str());
}

// Set by draw_world_table before each row's cells are drawn so that the
// right-click copy popups can also offer an "analyse this world" item.
static const LanWorldRecord* g_ctx_world = nullptr;
static gui_view::ScanPanelState* g_ctx_state = nullptr;

void draw_analyse_menu_item() {
    if (g_ctx_world == nullptr || g_ctx_state == nullptr) return;
    if (ImGui::MenuItem(gui_text::analyse_view_item)) {
        g_ctx_state->analyse_ip = g_ctx_world->world.ip;
        g_ctx_state->analyse_port = g_ctx_world->world.port;
        g_ctx_state->analyse_details_open = true;
    }
}

// Every table cell offers a right-click copy of its untruncated text.
void draw_copyable_cell(const char* popup_id, const std::string& text) {
    draw_clipped_text(text);
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(popup_id);
    }
    if (ImGui::BeginPopup(popup_id)) {
        if (ImGui::MenuItem(gui_text::copy_menu_item)) {
            ImGui::SetClipboardText(text.c_str());
        }
        draw_analyse_menu_item();
        ImGui::EndPopup();
    }
}

gui_motd::Mode motd_mode(int index) {
    if (index == 1) return gui_motd::Mode::Format;
    if (index == 2) return gui_motd::Mode::Raw;
    return gui_motd::Mode::Clean;
}

// MOTD cells may consist of several styled runs, so hovering is tested against
// the whole cell instead of a single item.
void draw_motd_cell(const char* popup_id,
                    const std::string& text,
                    gui_motd::Mode mode,
                    bool multiline,
                    bool monochrome = false) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    if (multiline) {
        gui_motd::draw_multiline(text, mode, monochrome);
    } else {
        gui_motd::draw_line(text, mode, monochrome);
    }
    const ImVec2 end(start.x + width, ImGui::GetItemRectMax().y);
    if (ImGui::IsMouseHoveringRect(start, end) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup(popup_id);
    }
    if (ImGui::BeginPopup(popup_id)) {
        if (ImGui::MenuItem(gui_text::copy_menu_item)) {
            ImGui::SetClipboardText(gui_motd::flatten(text, mode).c_str());
        }
        draw_analyse_menu_item();
        ImGui::EndPopup();
    }
}

// Minecraft MOTDs embed U+00A7 formatting codes and newlines that must not
// reach the table as-is.
std::string plain_motd(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        if (character == 0xC2 && index + 2 < text.size() &&
            static_cast<unsigned char>(text[index + 1]) == 0xA7) {
            index += 2;
            continue;
        }
        if (character == '\n') {
            output += " / ";
        } else if (character == '\t') {
            output.push_back(' ');
        } else if (character >= 0x20) {
            output.push_back(static_cast<char>(character));
        }
    }
    return output;
}

// Column widths follow the character budgets; the font is monospace for ASCII.
float column_width(int characters) {
    return ImGui::CalcTextSize("0").x * static_cast<float>(characters) +
           ImGui::GetStyle().CellPadding.x * 2.0F;
}

// "5/20", or "12345/..." when the pair does not fit the budget.
std::string players_cell(const std::optional<ServerStatus>& status) {
    if (!status || !status->available) {
        return {};
    }
    const std::string online = std::to_string(status->online_players);
    const std::string full = online + '/' + std::to_string(status->max_players);
    return full.size() <= static_cast<std::size_t>(players_columns) ? full : online + "/...";
}

std::string player_list_cell(const std::optional<ServerStatus>& status, gui_motd::Mode mode) {
    if (!status || !status->available) {
        return {};
    }
    std::string joined;
    for (const std::string& name : status->player_names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return gui_motd::flatten(joined, mode);
}

std::string version_cell(const std::optional<ServerStatus>& status, gui_motd::Mode mode) {
    if (!status || !status->available) {
        return {};
    }
    return gui_motd::flatten(status->version_name, mode);
}

// Returns the endpoint of a clicked row so the caller can query it.
// Latency shown like the vanilla client: five bars, with the exact value in a
// tooltip.
void draw_latency_cell(const std::optional<ServerStatus>& status) {
    const int latency = status && status->available ? status->latency_ms : -1;
    const float height = ImGui::GetTextLineHeight();

    // The bars have a natural size tied to the line height. When the column is
    // narrower they shrink to fit, but they never grow: resizing the column
    // wider must not stretch them.
    const float fixed_bar = std::max(2.0F, height * 0.18F);
    const float fixed_spacing = std::max(1.0F, height * 0.09F);
    const float fixed_total = fixed_bar * 5.0F + fixed_spacing * 4.0F;
    const float available = std::max(1.0F, ImGui::GetContentRegionAvail().x);
    const float scale = std::min(1.0F, available / fixed_total);
    const float bar_width = fixed_bar * scale;
    const float spacing = fixed_spacing * scale;
    const float width = bar_width * 5.0F + spacing * 4.0F;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));

    const int bars = latency < 0 ? 0
                   : latency < 150 ? 5
                   : latency < 300 ? 4
                   : latency < 600 ? 3
                   : latency < 1000 ? 2 : 1;
    const ImU32 filled = ImGui::GetColorU32(
        bars >= 4 ? ImVec4(0.35F, 0.85F, 0.35F, 1.0F)
        : bars >= 2 ? ImVec4(0.95F, 0.80F, 0.30F, 1.0F)
                    : ImVec4(0.90F, 0.35F, 0.35F, 1.0F));
    const ImU32 empty = ImGui::GetColorU32(ImVec4(0.35F, 0.35F, 0.38F, 1.0F));

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    for (int index = 0; index < 5; ++index) {
        const float bar_height = height * (0.28F + 0.18F * static_cast<float>(index));
        const float x = origin.x + static_cast<float>(index) * (bar_width + spacing);
        const ImVec2 minimum(x, origin.y + height - bar_height);
        const ImVec2 maximum(x + bar_width, origin.y + height);
        draw_list->AddRectFilled(minimum, maximum, index < bars ? filled : empty);
    }

    if (ImGui::IsItemHovered()) {
        if (latency >= 0) {
            ImGui::SetTooltip(gui_text::latency_format, latency);
        } else {
            ImGui::SetTooltip("%s", gui_text::latency_unknown);
        }
    }
}

// Draws the green/red ✓/✗ icon for a boolean analysis item. The caller's pushed
// text colour wins when the world is dimmed, so no colour is pushed then.
void draw_analyse_bool(bool value, bool dimmed) {
    if (!dimmed) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            value ? ImVec4(0.30F, 0.85F, 0.35F, 1.0F)
                  : ImVec4(0.95F, 0.30F, 0.30F, 1.0F));
    }
    ImGui::TextUnformatted(value ? gui_text::bool_yes_icon : gui_text::bool_no_icon);
    if (!dimmed) {
        ImGui::PopStyleColor();
    }
}

// Draws one analysis-item cell. Boolean items show the green/red ✓/✗ icon; the
// rest are plain text. The cell keeps the caller's dimmed colour when the world
// stopped broadcasting.
void draw_analyse_cell(int column, const ServerAnalyseItems& items,
                       bool analyse_enabled, bool dimmed) {
    if (!analyse_enabled) {
        ImGui::TextUnformatted("-");
        return;
    }
    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                           ImGui::GetColorU32(ImVec4(0.25F, 0.25F, 0.25F, 0.35F)));

    const auto item = static_cast<gui_view::ScanColumn>(column);
    switch (item) {
        case gui_view::ScanColumn::LanWorld:
            draw_analyse_bool(items.sa_is_lan_world, dimmed);
            break;
        case gui_view::ScanColumn::IsAvailable:
            draw_analyse_bool(items.sa_is_available, dimmed);
            break;
        case gui_view::ScanColumn::IsVanillaVersion:
            draw_analyse_bool(items.sa_is_vanilla_version, dimmed);
            break;
        case gui_view::ScanColumn::IsLanMotdClean:
            draw_analyse_bool(items.sa_is_lan_motd_clean, dimmed);
            break;
        case gui_view::ScanColumn::IsMotdClean:
            draw_analyse_bool(items.sa_is_motd_clean, dimmed);
            break;
        case gui_view::ScanColumn::IsRegularLanMotd:
            draw_analyse_bool(items.sa_is_regular_lan_motd, dimmed);
            break;
        case gui_view::ScanColumn::StrVersion:
            draw_copyable_cell("copy_an_ver", items.sa_str_version);
            break;
        case gui_view::ScanColumn::StrServerType:
            draw_copyable_cell("copy_an_type", items.sa_str_server_type);
            break;
        case gui_view::ScanColumn::IntActualOnline:
            draw_copyable_cell("copy_an_online",
                               std::to_string(items.sa_int_actual_online));
            break;
        case gui_view::ScanColumn::StrActualServerVersion:
            draw_copyable_cell("copy_an_real_ver",
                               items.sa_str_actual_server_version);
            break;
        case gui_view::ScanColumn::IsDefaultPort:
            draw_analyse_bool(items.sa_is_default_port, dimmed);
            break;
        case gui_view::ScanColumn::IsCorrectOnline:
            draw_analyse_bool(items.sa_is_correct_online, dimmed);
            break;
        case gui_view::ScanColumn::IsCorrectProtocol:
            draw_analyse_bool(items.sa_is_correct_protocol, dimmed);
            break;
        case gui_view::ScanColumn::IntLanMotdColorChar:
            draw_copyable_cell("copy_an_color",
                               std::to_string(items.sa_int_lan_motd_color_char));
            break;
        default:
            draw_analyse_bool(items.sa_is_motds_equal, dimmed);
            break;
    }
}

void draw_world_cell(int column, const LanWorldRecord& record, gui_motd::Mode mode,
                     bool analyse_enabled) {
    // A world that stopped broadcasting is drawn entirely in the caller's dimmed
    // colour, so per-cell colours are suppressed.
    const bool dimmed = !record.alive;

    // Every column from LanWorld onward is an analysis-item column.
    if (column >= static_cast<int>(gui_view::ScanColumn::LanWorld)) {
        if (dimmed) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55F, 0.55F, 0.58F, 1.0F));
        }
        draw_analyse_cell(column, compute_server_analyse_items(record),
                          analyse_enabled, dimmed);
        if (dimmed) {
            ImGui::PopStyleColor();
        }
        return;
    }

    switch (static_cast<gui_view::ScanColumn>(column)) {
        case gui_view::ScanColumn::Motd:
            draw_motd_cell("copy_motd", record.world.lan_motd, mode, false, dimmed);
            break;
        case gui_view::ScanColumn::Ip:
            draw_copyable_cell("copy_ip", record.world.ip);
            break;
        case gui_view::ScanColumn::Port:
            draw_copyable_cell("copy_port", std::to_string(record.world.port));
            break;
        case gui_view::ScanColumn::Players:
            draw_copyable_cell("copy_players", players_cell(record.world.status));
            break;
        case gui_view::ScanColumn::Version:
            draw_copyable_cell("copy_version", version_cell(record.world.status, mode));
            break;
        case gui_view::ScanColumn::PlayerList:
            draw_copyable_cell("copy_list", player_list_cell(record.world.status, mode));
            break;
        default:
            draw_latency_cell(record.world.status);
            break;
    }
}

// Returns the endpoint of a clicked row so the caller can query it.
std::optional<Endpoint> draw_world_table(const std::vector<LanWorldRecord>& worlds,
                                         gui_view::ScanPanelState& state,
                                         float reserved_height,
                                         gui_motd::Mode mode,
                                         bool analyse_enabled) {
    std::vector<int> columns;
    for (const int column : state.column_order) {
        if (column >= 0 && column < static_cast<int>(gui_view::ScanColumn::Count) &&
            state.column_visible[static_cast<std::size_t>(column)]) {
            columns.push_back(column);
        }
    }
    if (columns.empty()) {
        return std::nullopt;
    }

    // NoSavedSettings: otherwise the table restores the column display order it
    // remembered, which would undo the order chosen in the picker.
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_ScrollX;
    // The table id encodes the layout: a table keeps the display order it was
    // created with, so a changed layout has to start a fresh table.
    std::string table_id = gui_text::table_id;
    for (const int column : columns) {
        table_id += '_';
        table_id += static_cast<char>('0' + column);
    }
    if (!ImGui::BeginTable(table_id.c_str(), static_cast<int>(columns.size()), flags,
                           ImVec2(0.0F, -reserved_height))) {
        return std::nullopt;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    for (const int column : columns) {
        const gui_view::ColumnInfo info = gui_view::column_info(column);
        ImGui::TableSetupColumn(info.label, ImGuiTableColumnFlags_WidthFixed,
                                column_width(info.characters));
    }
    ImGui::TableHeadersRow();

    std::optional<Endpoint> clicked;
    // Worlds that stopped broadcasting sink to the bottom of the list; within
    // each group the discovery order is kept.
    std::vector<const LanWorldRecord*> ordered;
    ordered.reserve(worlds.size());
    for (const LanWorldRecord& record : worlds) {
        if (record.alive) {
            ordered.push_back(&record);
        }
    }
    for (const LanWorldRecord& record : worlds) {
        if (!record.alive) {
            ordered.push_back(&record);
        }
    }

    for (const LanWorldRecord* entry : ordered) {
        const LanWorldRecord& record = *entry;
        const std::string key = record.world.ip + ':' + std::to_string(record.world.port);
        ImGui::PushID(key.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns)) {
            clicked = Endpoint{record.world.ip, record.world.port};
        }
        // The tooltip is submitted before the dimmed style is pushed so it keeps
        // the normal text colour.
        if (!record.alive) {
            ImGui::SetItemTooltip("%s", gui_text::world_dead_tooltip);
        }

        // Dead worlds stay listed but are drawn grey.
        const bool dimmed = !record.alive;
        if (dimmed) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55F, 0.55F, 0.58F, 1.0F));
        }

        ImGui::SameLine();
        g_ctx_world = &record;
        g_ctx_state = &state;
        draw_world_cell(columns.front(), record, mode, analyse_enabled);
        for (std::size_t index = 1; index < columns.size(); ++index) {
            ImGui::TableNextColumn();
            draw_world_cell(columns[index], record, mode, analyse_enabled);
        }
        g_ctx_world = nullptr;
        g_ctx_state = nullptr;

        if (dimmed) {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    return clicked;
}

void draw_status_summary(const ServerStatus& status,
                        gui_favicon::Texture& favicon,
                        ID3D11Device* device,
                        float dpi_scale,
                        gui_motd::Mode mode) {
    const auto draw_label = [](const char* label) {
        if (ImFont* bold = gui_fonts::bold(); bold != nullptr) {
            ImGui::PushFont(bold, 0.0F);
            ImGui::TextUnformatted(label);
            ImGui::PopFont();
        } else {
            ImGui::TextUnformatted(label);
        }
    };
    const auto field = [&draw_label](const char* label, const std::string& value) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_label(label);
        ImGui::TableNextColumn();
        draw_copyable_cell("copy_field", value);
    };

    // The icon sits to the left of the fields, which occupy the rest of the row.
    gui_favicon::update(favicon, status.favicon, device);
    if (favicon.view != nullptr) {
        const float side = 64.0F * dpi_scale;
        ImGui::Image(reinterpret_cast<ImTextureID>(favicon.view), ImVec2(side, side));
    } else {
        ImGui::TextUnformatted(gui_text::details_favicon_missing);
    }
    ImGui::SameLine();
    ImGui::BeginGroup();

    // Fixed label column plus a stretching value column: auto-sizing would
    // fight the width-aware clipping and collapse every value to "...".
    if (ImGui::BeginTable("summary_fields", 2)) {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed,
                                column_width(10));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::PushID("summary");
        // A multi-line MOTD keeps its line breaks here.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_label(gui_text::details_motd);
        ImGui::TableNextColumn();
        draw_motd_cell("copy_motd_field", status.motd, mode, true);
        field(gui_text::details_version, gui_motd::flatten(status.version_name, mode));
        field(gui_text::details_protocol, std::to_string(status.protocol_version));
        field(gui_text::details_players,
              std::to_string(status.online_players) + '/' +
                  std::to_string(status.max_players));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_label(gui_text::details_secure_chat);
        ImGui::TableNextColumn();
        if (status.secure_chat_known) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                status.enforces_secure_chat
                    ? ImVec4(0.30F, 0.85F, 0.35F, 1.0F)
                    : ImVec4(0.95F, 0.30F, 0.30F, 1.0F));
            ImGui::TextUnformatted(status.enforces_secure_chat
                                       ? gui_text::bool_yes_icon
                                       : gui_text::bool_no_icon);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(gui_text::details_unknown);
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_label(gui_text::details_latency);
        ImGui::TableNextColumn();
        draw_latency_cell(status);
        ImGui::PopID();
        ImGui::EndTable();
    }
    ImGui::EndGroup();
}

void draw_player_samples(const ServerStatus& status, float height) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("details_players", 2, flags, ImVec2(0.0F, height))) {
        return;
    }
    ImGui::TableSetupColumn(gui_text::details_player_name, ImGuiTableColumnFlags_WidthFixed,
                            column_width(20));
    ImGui::TableSetupColumn(gui_text::details_player_uuid, ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (std::size_t index = 0; index < status.player_names.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_copyable_cell("copy_name", plain_motd(status.player_names[index]));
        ImGui::TableNextColumn();
        const bool has_id = index < status.player_ids.size() &&
                            !status.player_ids[index].empty();
        draw_copyable_cell("copy_uuid", has_id ? status.player_ids[index]
                                               : gui_text::details_player_omitted);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void start_query(gui_view::ScanPanelState& state, ServerProbe& probe) {
    std::string ip;
    unsigned short port = 0;
    if (parse_server_endpoint(state.endpoint_input.data(), ip, port)) {
        state.endpoint_error.clear();
        probe.start(ip, port);
    } else {
        state.endpoint_error = gui_text::details_invalid_endpoint;
    }
}

void draw_details_pane(ServerProbe& probe,
                       gui_view::ScanPanelState& state,
                       ID3D11Device* device,
                       float dpi_scale,
                       float height) {
    ImGui::BeginChild("details", ImVec2(0.0F, height), ImGuiChildFlags_Borders);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(gui_text::details_title);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0F * dpi_scale);
    const bool submitted = ImGui::InputTextWithHint(
        "##endpoint", gui_text::details_ip_hint, state.endpoint_input.data(),
        state.endpoint_input.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(gui_text::details_query_button) || submitted) {
        start_query(state, probe);
    }

    if (!state.endpoint_error.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), "%s", state.endpoint_error.c_str());
    }
    ImGui::Separator();

    const ServerProbe::Snapshot snapshot = probe.snapshot();
    if (snapshot.running) {
        ImGui::TextUnformatted(gui_text::details_querying);
        ImGui::EndChild();
        return;
    }
    if (!snapshot.has_result) {
        ImGui::TextUnformatted(gui_text::details_empty);
        ImGui::EndChild();
        return;
    }
    if (!snapshot.status.available) {
        ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), gui_text::details_failed_format,
                           snapshot.status.error.c_str());
        ImGui::EndChild();
        return;
    }

    // Both dividers span the same height: the split border and the player
    // table's column border are given the identical remaining height.
    const float split_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("details_split", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
                          ImVec2(0.0F, split_height))) {
        ImGui::TableSetupColumn("##summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##players", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, split_height);
        ImGui::TableNextColumn();
        draw_status_summary(snapshot.status, state.favicon, device, dpi_scale,
                            motd_mode(state.motd_mode_index));
        ImGui::TableNextColumn();
        draw_player_samples(snapshot.status, split_height);
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace

namespace gui_view {

void draw_scan_panel(LanMonitor& monitor,
                     ServerProbe& probe,
                     ScanPanelState& state,
                     ID3D11Device* device,
                     float dpi_scale) {
    const LanMonitor::Snapshot snapshot = monitor.snapshot();

    // One button drives both directions: it starts a scan when idle and stops
    // the running one when clicked again. Its height matches the three rows of
    // status text, duration combo, and mode combo beside it.
    const ImVec2 button_size(200.0F * dpi_scale,
                             ImGui::GetTextLineHeightWithSpacing() * 3.0F);
    const char* label = snapshot.running ? gui_text::scan_button_running
                                         : gui_text::scan_button_idle;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5F);
    const bool clicked = ImGui::Button(label, button_size);
    ImGui::PopFont();
    if (clicked) {
        if (snapshot.running) {
            monitor.request_stop();
        } else {
            LanMonitorOptions options;
            const int seconds = typed_duration(state.seconds_input);
            if (seconds > 0) {
                options.duration = std::chrono::seconds(seconds);
            }
            options.mode = state.mode_index == 0 ? LanScanMode::UdpOnly
                                                 : LanScanMode::UdpAndStatusQuery;
            monitor.start(options);
        }
    }

    // Four regions: scan controls, status-query controls, global MOTD
    // rendering, and the analysis button.
    ImGui::SameLine();
    if (ImGui::BeginTable("top_controls", 4,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        const float input_width = 140.0F * dpi_scale;

        // ---- Row 1 ----
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (snapshot.running && snapshot.continuous) {
            ImGui::Text(gui_text::continuous_running_format, snapshot.elapsed_seconds);
        } else if (snapshot.running) {
            ImGui::Text(gui_text::remaining_format, snapshot.remaining_seconds);
        } else {
            ImGui::Text(gui_text::found_format, static_cast<int>(snapshot.stats.listed_worlds));
        }

        ImGui::TableNextColumn();
        if (state.mode_index == 1) {
            std::string progress(128, '\0');
            int length = std::snprintf(progress.data(), progress.size(),
                                       gui_text::slp_progress_format,
                                       static_cast<int>(snapshot.stats.queried_worlds),
                                       static_cast<int>(snapshot.stats.listed_worlds));
            if (snapshot.stats.pending_queries > 0 && length > 0) {
                length += std::snprintf(progress.data() + length, progress.size() - length,
                                        gui_text::slp_pending_format,
                                        static_cast<int>(snapshot.stats.pending_queries));
            }
            if (snapshot.next_auto_query_seconds > 0 && length > 0) {
                length += std::snprintf(progress.data() + length, progress.size() - length,
                                        gui_text::slp_next_format,
                                        snapshot.next_auto_query_seconds);
            }
            progress.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
            ImGui::TextUnformatted(progress.c_str());
        }

        // Global MOTD rendering mode, applied to the list, details, and logs.
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(gui_text::motd_mode_label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(input_width);
        const char* motd_modes[] = {gui_text::motd_mode_clean, gui_text::motd_mode_format,
                                    gui_text::motd_mode_raw};
        if (ImGui::BeginCombo("##motd_mode", motd_modes[state.motd_mode_index])) {
            for (int index = 0; index < 3; ++index) {
                const bool selected = index == state.motd_mode_index;
                if (ImGui::Selectable(motd_modes[index], selected)) {
                    state.motd_mode_index = index;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // Column 4, row 1: analysis button for the details-pane world.
        ImGui::TableNextColumn();
        {
            const bool has_endpoint = (state.endpoint_input[0] != '\0');
            ImGui::BeginDisabled(!has_endpoint);
            if (ImGui::Button(gui_text::analyse_view_item)) {
                std::string ip;
                unsigned short port = 0;
                if (parse_server_endpoint(state.endpoint_input.data(), ip, port)) {
                    state.analyse_ip = ip;
                    state.analyse_port = port;
                    state.analyse_details_open = true;
                }
            }
            ImGui::EndDisabled();
        }

        // ---- Row 2 ----
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(snapshot.running);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(gui_text::seconds_label);
        ImGui::SameLine();
        const float arrow_width = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(input_width - arrow_width);
        ImGui::InputTextWithHint("##seconds", gui_text::duration_continuous,
                                 state.seconds_input.data(), state.seconds_input.size(),
                                 ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(0.0F, 0.0F);
        if (ImGui::BeginCombo("##seconds_presets", "", ImGuiComboFlags_NoPreview)) {
            for (const int seconds : duration_presets) {
                if (ImGui::Selectable(duration_label(seconds).c_str(),
                                      typed_duration(state.seconds_input) == seconds)) {
                    if (seconds == 0) {
                        state.seconds_input.fill('\0');
                    } else {
                        std::snprintf(state.seconds_input.data(), state.seconds_input.size(),
                                      "%d", seconds);
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        ImGui::TableNextColumn();
        if (state.mode_index == 1) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(gui_text::auto_slp_label);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(input_width);
            const std::string auto_preview = auto_slp_label(
                auto_slp_presets[static_cast<std::size_t>(state.auto_slp_index)]);
            if (ImGui::BeginCombo("##auto_slp", auto_preview.c_str())) {
                for (int index = 0; index < static_cast<int>(auto_slp_presets.size()); ++index) {
                    const bool selected = index == state.auto_slp_index;
                    if (ImGui::Selectable(
                            auto_slp_label(auto_slp_presets[static_cast<std::size_t>(index)])
                                .c_str(), selected)) {
                        state.auto_slp_index = index;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            monitor.set_auto_query_interval(
                auto_slp_presets[static_cast<std::size_t>(state.auto_slp_index)]);
        }

        // Third column, second row: the world-list column picker.
        ImGui::TableNextColumn();
        if (ImGui::Button(gui_text::scan_columns_button)) {
            state.columns_open = true;
        }
        // Column 4, row 2: empty.
        ImGui::TableNextColumn();

        // ---- Row 3 ----
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(snapshot.running);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(gui_text::mode_label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(input_width);
        const char* modes[] = {gui_text::mode_udp, gui_text::mode_udp_and_slp};
        if (ImGui::BeginCombo("##mode", modes[state.mode_index])) {
            for (int index = 0; index < 2; ++index) {
                const bool selected = index == state.mode_index;
                if (ImGui::Selectable(modes[index], selected)) {
                    state.mode_index = index;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        ImGui::TableNextColumn();
        if (state.mode_index == 1) {
            ImGui::BeginDisabled(!snapshot.running);
            if (ImGui::Button(gui_text::refresh_button)) {
                monitor.refresh_status_queries();
            }
            ImGui::EndDisabled();
        }
        // Columns 3 and 4, row 3: empty.
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();

        ImGui::EndTable();
    }

    if (!snapshot.error.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), gui_text::error_format,
                           snapshot.error.c_str());
    }

    ImGui::Separator();

    const float details_height = ImGui::GetTextLineHeightWithSpacing() * 11.0F;
    const float reserved = details_height + ImGui::GetFrameHeightWithSpacing() +
                           ImGui::GetStyle().ItemSpacing.y;
    if (const auto endpoint = draw_world_table(snapshot.worlds, state, reserved,
                                              motd_mode(state.motd_mode_index),
                                              true)) {
        const std::string text = endpoint->first + ':' + std::to_string(endpoint->second);
        std::snprintf(state.endpoint_input.data(), state.endpoint_input.size(), "%s",
                      text.c_str());
        start_query(state, probe);
    }

    std::string counters(256, '\0');
    int written = std::snprintf(counters.data(), counters.size(), gui_text::counters_format,
                                static_cast<unsigned long long>(
                                    snapshot.stats.received_advertisements),
                                static_cast<int>(snapshot.stats.listed_worlds));
    if (snapshot.continuous && written > 0) {
        written += std::snprintf(counters.data() + written, counters.size() - written,
                                 gui_text::counters_dead_format,
                                 static_cast<int>(snapshot.stats.dead_worlds));
    }
    counters.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(counters.c_str());

    ImGui::SameLine();
    if (ImGui::Button(gui_text::log_button)) {
        state.log_open = true;
    }

    draw_details_pane(probe, state, device, dpi_scale, details_height);
    draw_analyse_details_window(state, snapshot.worlds, dpi_scale);
}

} // namespace gui_view








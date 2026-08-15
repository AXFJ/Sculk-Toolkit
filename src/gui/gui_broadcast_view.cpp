#include "gui_broadcast_view.h"

#include "gui_dialog.h"
#include "gui_text.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

// Connection logs are not surfaced yet; the manager still needs a sink.
void ignore_log(const std::string&, const std::string&, const std::string&) {}

// Builds a display string from structured log entry fields.
std::string format_log_entry(const FakeServerLogEntry& entry) {
    if (entry.event == "login") {
        std::string text = "登录尝试: ";
        text += entry.ip;
        text += "，玩家 ";
        text += entry.player;
        return text;
    }
    // "slp" or any other event
    std::string text = "Ping 请求: ";
    text += entry.ip;
    return text;
}

const char* mode_text(FakeServerMode mode) {
    if (mode == FakeServerMode::Udp) return gui_text::form_mode_lan;
    if (mode == FakeServerMode::Both) return gui_text::form_mode_both;
    return gui_text::form_mode_fake;
}

// The LAN MOTD is what the world list shows, so prefer it when both exist.
std::string list_motd(const FakeServerConfig& config) {
    return config.mode == FakeServerMode::Tcp ? config.motd : config.lan_motd;
}

std::string list_port(const FakeServerConfig& config) {
    return std::to_string(config.mode == FakeServerMode::Tcp ? config.port : config.lan_port);
}

bool selected(const std::vector<int>& selection, int id) {
    return std::find(selection.begin(), selection.end(), id) != selection.end();
}

void apply_click(gui_view::BroadcastPanelState& state,
                 const std::vector<FakeServerSnapshot>& servers,
                 int id) {
    if (ImGui::GetIO().KeyShift && state.anchor_id != 0) {
        const auto find = [&servers](int target) {
            return std::find_if(servers.begin(), servers.end(),
                                [target](const FakeServerSnapshot& server) {
                                    return server.id == target;
                                });
        };
        const auto anchor = find(state.anchor_id);
        const auto clicked = find(id);
        if (anchor != servers.end() && clicked != servers.end()) {
            auto first = anchor;
            auto last = clicked;
            if (first > last) {
                std::swap(first, last);
            }
            state.selection.clear();
            for (auto entry = first; entry <= last; ++entry) {
                state.selection.push_back(entry->id);
            }
            return;
        }
    }
    state.selection.assign(1, id);
    state.anchor_id = id;
}

void run_on(FakeServerManager& servers,
            const std::vector<int>& ids,
            gui_view::BroadcastPanelState& state,
            bool start) {
    for (const int id : ids) {
        std::string error;
        const bool ok = start ? servers.start(id, ignore_log, error)
                              : servers.stop(id, error);
        if (!ok && !error.empty()) {
            gui_status::show_error(state.status, error);
        }
    }
}

void remove_all(FakeServerManager& servers,
                const std::vector<int>& ids,
                gui_view::BroadcastPanelState& state) {
    for (const int id : ids) {
        std::string error;
        if (!servers.remove(id, error) && !error.empty()) {
            gui_status::show_error(state.status, error);
        }
    }
    state.selection.clear();
    state.anchor_id = 0;
}

// Applies the form: an id overwrites that broadcast (hot reload), an empty id
// creates a new one. The form is cleared once the change lands.
void submit(FakeServerManager& servers, gui_view::BroadcastPanelState& state, bool start) {
    FakeServerConfig config;
    std::string error;
    if (!gui_form::build(state.form, config, error)) {
        gui_status::show_error(state.status, error);
        return;
    }

    const int target = gui_form::target_id(state.form);
    int id = target;
    if (target == 0) {
        id = servers.create(config);
    } else if (!servers.update(target, config, error)) {
        gui_status::show_error(state.status, error);
        return;
    }

    if (start && !servers.start(id, ignore_log, error) && !servers.is_running(id)) {
        gui_status::show_error(state.status, error);
    }
    gui_form::clear(state.form);
}

void draw_row_menu(FakeServerManager& servers,
                   gui_view::BroadcastPanelState& state,
                   const std::vector<FakeServerSnapshot>& snapshots,
                   int id);

void export_to_file(FakeServerManager& servers,
                    gui_view::BroadcastPanelState& state,
                    const std::vector<int>& ids);

void report_export(gui_view::BroadcastPanelState& state,
                   const std::filesystem::path& path,
                   const std::string& error) {
    if (path.empty()) {
        gui_status::show_error(state.status, error);
        return;
    }
    char message[512]{};
    std::snprintf(message, sizeof(message), gui_text::broadcast_export_done,
                  path.generic_string().c_str());
    gui_status::show(state.status, message);
}

void report_import(gui_view::BroadcastPanelState& state,
                   std::size_t count,
                   const std::string& error) {
    if (!error.empty()) {
        gui_status::show_error(state.status, error);
        return;
    }
    char message[128]{};
    std::snprintf(message, sizeof(message), gui_text::broadcast_import_done,
                  static_cast<int>(count));
    gui_status::show(state.status, message);
}

// The user picks the folder and file name; the group field overrides groups.
void export_to_file(FakeServerManager& servers,
                    gui_view::BroadcastPanelState& state,
                    const std::vector<int>& ids) {
    const std::wstring file = gui_dialog::save_json_file();
    if (file.empty()) {
        return;
    }
    std::string error;
    const auto path = servers.export_servers(ids, state.group_input.data(),
                                             std::filesystem::path{file}, error);
    report_export(state, path, error);
}

void begin_edit(gui_view::BroadcastPanelState& state,
                const std::vector<FakeServerSnapshot>& snapshots,
                int id) {
    const auto found = std::find_if(snapshots.begin(), snapshots.end(),
                                    [id](const FakeServerSnapshot& server) {
                                        return server.id == id;
                                    });
    if (found == snapshots.end()) {
        return;
    }
    gui_form::load(state.form, found->config);
    gui_form::set_id(state.form, id);
    gui_status::clear(state.status);
}

std::string clock_text(std::chrono::system_clock::time_point at) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(at);
    std::tm parts{};
    localtime_s(&parts, &raw);
    char text[16]{};
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d",
                  parts.tm_hour, parts.tm_min, parts.tm_sec);
    return text;
}

// Only servers with a TCP listener ever see connections.
bool has_fake_server(FakeServerMode mode) {
    return mode != FakeServerMode::Udp;
}

// Escapes a string for safe inclusion in a JSON value.
std::string json_escape_log(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char c : value) {
        if (c == '"')  escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else escaped.push_back(c);
    }
    return escaped;
}

// Writes the current broadcast log as JSON to the given file path.
bool export_broadcast_log(const std::string& path,
                          const std::vector<FakeServerLogEntry>& entries) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    file << "{\n    \"fakeserverLogItems\":[\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const FakeServerLogEntry& entry = entries[i];

        const std::time_t raw = std::chrono::system_clock::to_time_t(entry.at);
        std::tm parts{};
        localtime_s(&parts, &raw);
        char time_text[16]{};
        std::snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d",
                      parts.tm_hour, parts.tm_min, parts.tm_sec);

        file << "        {\"time\":\"" << time_text
             << "\",\"event\":\"" << entry.event
             << "\",\"ip\":\"" << json_escape_log(entry.ip);
        if (!entry.player.empty()) {
            file << "\",\"player\":\"" << json_escape_log(entry.player);
        }
        file << "\"}";
        if (i + 1 < entries.size()) file << ',';
        file << '\n';
    }
    file << "    ]\n}\n";
    return static_cast<bool>(file);
}

// Builds a default export file name: fakeserver_<id>_<date>.log.json
std::string default_log_export_name(const FakeServerConfig& /*config*/,
                                    int server_id) {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm parts{};
    localtime_s(&parts, &now);
    char date[16]{};
    std::snprintf(date, sizeof(date), "%04d-%02d-%02d",
                  1900 + parts.tm_year, 1 + parts.tm_mon, parts.tm_mday);

    char filename[128]{};
    std::snprintf(filename, sizeof(filename), "fakeserver_%d_%s.log.json",
                  server_id, date);
    return filename;
}

// Connection log of the single selected broadcast.
void draw_log(FakeServerManager& servers,
              gui_view::BroadcastPanelState& state,
              const std::vector<FakeServerSnapshot>& snapshots,
              float height) {
    ImGui::BeginChild("broadcast_log", ImVec2(0.0F, height), ImGuiChildFlags_Borders);

    const int id = state.selection.size() == 1 ? state.selection.front() : 0;
    const auto found = std::find_if(snapshots.begin(), snapshots.end(),
                                    [id](const FakeServerSnapshot& server) {
                                        return server.id == id;
                                    });

    // Title on the left, export toolbar on the right (always visible).
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(gui_text::broadcast_log_title);

    const bool can_export = (id != 0 && found != snapshots.end() &&
                             has_fake_server(found->config.mode));
    const std::vector<FakeServerLogEntry> entries =
        can_export ? servers.log(id) : std::vector<FakeServerLogEntry>{};

    {
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_export || entries.empty());
        if (ImGui::Button(gui_text::broadcast_log_export)) {
            const std::string path(state.export_path.data());
            if (!path.empty() && !entries.empty()) {
                if (export_broadcast_log(path, entries)) {
                    char msg[512]{};
                    std::snprintf(msg, sizeof(msg),
                                  gui_text::broadcast_log_export_done,
                                  entries.size(), path.c_str());
                    gui_status::show(state.status, msg);
                } else {
                    gui_status::show_error(state.status,
                                           gui_text::broadcast_log_export_error);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const float input_width = 400.0F * ImGui::GetIO().FontGlobalScale;
        ImGui::SetNextItemWidth(input_width);
        ImGui::InputTextWithHint("##log_path", gui_text::broadcast_log_export,
                                 state.export_path.data(), state.export_path.size());
        ImGui::SameLine(0.0F, 2.0F);
        if (ImGui::Button(gui_text::broadcast_log_folder)) {
            if (can_export) {
                const std::string name = default_log_export_name(found->config, id);
                const std::wstring default_name(name.begin(), name.end());
                const std::wstring chosen = gui_dialog::save_json_file(default_name);
                if (!chosen.empty()) {
                    const std::string narrow = gui_dialog::to_utf8(chosen);
                    std::snprintf(state.export_path.data(), state.export_path.size(),
                                  "%s", narrow.c_str());
                }
            }
        }
    }

    if (id == 0 || found == snapshots.end()) {
        ImGui::Separator();
        ImGui::TextUnformatted(gui_text::broadcast_log_select);
        ImGui::EndChild();
        return;
    }
    if (!has_fake_server(found->config.mode)) {
        ImGui::Separator();
        ImGui::TextUnformatted(gui_text::broadcast_log_udp_only);
        ImGui::EndChild();
        return;
    }

    ImGui::Separator();

    if (entries.empty()) {
        ImGui::TextUnformatted(gui_text::broadcast_log_empty);
        ImGui::EndChild();
        return;
    }

    const bool is_group = !found->config.group.empty();
    ImGui::BeginChild("broadcast_log_lines");
    for (const FakeServerLogEntry& entry : entries) {
        const std::string display = format_log_entry(entry);
        if (is_group) {
            ImGui::Text(gui_text::broadcast_log_line_group, clock_text(entry.at).c_str(),
                        entry.server_id, display.c_str());
        } else {
            ImGui::Text(gui_text::broadcast_log_line, clock_text(entry.at).c_str(),
                        display.c_str());
        }
    }
    // Follow new lines while the newest one is already in view.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void draw_toolbar(FakeServerManager& servers,
                  gui_view::BroadcastPanelState& state,
                  const std::vector<FakeServerSnapshot>& snapshots,
                  float dpi_scale) {
    const std::vector<int> all = servers.ids();
    const bool has_selection = !state.selection.empty();

    // The same construct the listen page uses for its dividers: a table with
    // inner vertical borders, so the rule looks and aligns identically.
    if (!ImGui::BeginTable("broadcast_toolbar", 2,
                           ImGuiTableFlags_SizingFixedFit |
                           ImGuiTableFlags_BordersInnerV)) {
        return;
    }

    // Row one: everything that acts on all broadcasts.
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Button(gui_text::broadcast_start_all)) {
        run_on(servers, all, state, true);
    }
    ImGui::SameLine();
    if (ImGui::Button(gui_text::broadcast_stop_all)) {
        run_on(servers, all, state, false);
    }
    ImGui::SameLine();
    if (ImGui::Button(gui_text::broadcast_remove_all)) {
        remove_all(servers, all, state);
    }
    ImGui::TableNextColumn();
    if (ImGui::Button(gui_text::broadcast_export_all)) {
        export_to_file(servers, state, {});
    }

    // Row two: everything that acts on the selection, then the preset files.
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!has_selection);
    if (ImGui::Button(gui_text::broadcast_start)) {
        run_on(servers, state.selection, state, true);
    }
    ImGui::SameLine();
    if (ImGui::Button(gui_text::broadcast_stop)) {
        run_on(servers, state.selection, state, false);
    }
    ImGui::SameLine();
    // Editing works on exactly one server.
    ImGui::BeginDisabled(state.selection.size() != 1);
    if (ImGui::Button(gui_text::broadcast_edit)) {
        begin_edit(state, snapshots, state.selection.front());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(gui_text::broadcast_remove)) {
        remove_all(servers, state.selection, state);
    }
    ImGui::EndDisabled();

    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!has_selection);
    if (ImGui::Button(gui_text::broadcast_export)) {
        export_to_file(servers, state, state.selection);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(gui_text::broadcast_import)) {
        const std::wstring file = gui_dialog::open_json_file();
        if (!file.empty()) {
            std::string error;
            const std::vector<int> ids = servers.import_file(
                std::filesystem::path{file}, state.group_input.data(), error);
            report_import(state, ids.size(), error);
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0F * dpi_scale);
    ImGui::InputTextWithHint("##group", gui_text::broadcast_group_hint,
                             state.group_input.data(), state.group_input.size());
    ImGui::EndTable();
}

// Right-clicking a row selects it when needed and offers the same actions as
// the toolbar.
void draw_row_menu(FakeServerManager& servers,
                   gui_view::BroadcastPanelState& state,
                   const std::vector<FakeServerSnapshot>& snapshots,
                   int id) {
    if (!ImGui::BeginPopupContextItem("row_menu")) {
        return;
    }
    if (!selected(state.selection, id)) {
        state.selection.assign(1, id);
        state.anchor_id = id;
    }
    if (ImGui::MenuItem(gui_text::broadcast_start)) {
        run_on(servers, state.selection, state, true);
    }
    if (ImGui::MenuItem(gui_text::broadcast_stop)) {
        run_on(servers, state.selection, state, false);
    }
    if (ImGui::MenuItem(gui_text::broadcast_edit)) {
        begin_edit(state, snapshots, id);
    }
    if (ImGui::MenuItem(gui_text::broadcast_remove)) {
        remove_all(servers, state.selection, state);
    }
    ImGui::EndPopup();
}

void draw_list(FakeServerManager& servers,
               gui_view::BroadcastPanelState& state,
               const std::vector<FakeServerSnapshot>& snapshots,
               float dpi_scale,
               float reserved_height) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable(gui_text::broadcast_table_id, 4, flags,
                           ImVec2(0.0F, -reserved_height))) {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(gui_text::broadcast_column_id, ImGuiTableColumnFlags_WidthFixed,
                            150.0F * dpi_scale);
    ImGui::TableSetupColumn(gui_text::broadcast_column_mode, ImGuiTableColumnFlags_WidthFixed,
                            170.0F * dpi_scale);
    ImGui::TableSetupColumn(gui_text::broadcast_column_port, ImGuiTableColumnFlags_WidthFixed,
                            80.0F * dpi_scale);
    ImGui::TableSetupColumn(gui_text::broadcast_column_motd, ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    // One row per server, except that servers sharing a group collapse under a
    // single expandable row.
    const auto draw_row = [&](const FakeServerSnapshot& server, bool indented) {
        ImGui::PushID(server.id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (server.running) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45F, 0.85F, 0.45F, 1.0F));
        }
        if (ImGui::Selectable("##row", selected(state.selection, server.id),
                              ImGuiSelectableFlags_SpanAllColumns)) {
            apply_click(state, snapshots, server.id);
        }
        draw_row_menu(servers, state, snapshots, server.id);
        ImGui::SameLine();
        if (indented) {
            ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        }
        ImGui::Text("%d", server.id);
        if (indented) {
            ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(mode_text(server.config.mode));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(list_port(server.config).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(list_motd(server.config).c_str());
        if (server.running) {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    };

    std::vector<std::string> group_order;
    std::map<std::string, std::vector<const FakeServerSnapshot*>> groups;
    for (const FakeServerSnapshot& server : snapshots) {
        if (server.config.group.empty()) {
            continue;
        }
        if (groups.find(server.config.group) == groups.end()) {
            group_order.push_back(server.config.group);
        }
        groups[server.config.group].push_back(&server);
    }

    std::vector<std::string> drawn_groups;
    for (const FakeServerSnapshot& server : snapshots) {
        const std::string& group = server.config.group;
        if (group.empty()) {
            draw_row(server, false);
            continue;
        }
        if (std::find(drawn_groups.begin(), drawn_groups.end(), group) != drawn_groups.end()) {
            continue;
        }
        drawn_groups.push_back(group);

        const std::vector<const FakeServerSnapshot*>& members = groups[group];
        const bool running = std::ranges::any_of(members,
                                                 [](const FakeServerSnapshot* member) {
                                                     return member->running;
                                                 });
        bool& expanded = state.expanded_groups[group];
        std::vector<int> member_ids;
        member_ids.reserve(members.size());
        for (const FakeServerSnapshot* member : members) {
            member_ids.push_back(member->id);
        }

        ImGui::PushID(group.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (running) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45F, 0.85F, 0.45F, 1.0F));
        }
        // Selecting the group row selects every member, so the toolbar acts on
        // the whole group.
        const bool group_selected = std::all_of(member_ids.begin(), member_ids.end(),
                                                [&state](int id) {
                                                    return selected(state.selection, id);
                                                });
        // AllowOverlap lets the expander button on top of the row keep its own
        // hit box instead of the row selectable swallowing every click.
        if (ImGui::Selectable("##group", group_selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                              ImGuiSelectableFlags_AllowOverlap)) {
            state.selection = member_ids;
            state.anchor_id = member_ids.front();
        }
        draw_row_menu(servers, state, snapshots, member_ids.front());
        ImGui::SameLine();
        // Plain ASCII: the triangle glyphs are missing from the merged fonts.
        if (ImGui::SmallButton(expanded ? gui_text::broadcast_group_collapse
                                        : gui_text::broadcast_group_expand)) {
            expanded = !expanded;
        }
        ImGui::SameLine();
        ImGui::Text(gui_text::broadcast_group_row, group.c_str(),
                    static_cast<int>(members.size()));
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        if (running) {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();

        if (expanded) {
            for (const FakeServerSnapshot* member : members) {
                draw_row(*member, true);
            }
        }
    }
    ImGui::EndTable();
}

} // namespace

namespace gui_view {

void draw_broadcast_panel(FakeServerManager& servers,
                          BroadcastPanelState& state,
                          float dpi_scale) {
    const std::vector<FakeServerSnapshot> snapshots = servers.list();

    // Left: toolbar and list. Right: the create form. The whole layout lives in
    // a child that leaves one line at the bottom for the status/error message,
    // so the message is always pinned to the bottom of the page.
    const float form_width = 460.0F * dpi_scale;
    const float list_width = std::max(320.0F * dpi_scale,
                                      ImGui::GetContentRegionAvail().x - form_width -
                                          ImGui::GetStyle().ItemSpacing.x);

    // The footer holds a separator plus one status line.
    const float status_height = ImGui::GetTextLineHeightWithSpacing() +
                                ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("broadcast_host", ImVec2(0.0F, -status_height));

    ImGui::BeginChild("broadcast_list", ImVec2(list_width, 0.0F));
    draw_toolbar(servers, state, snapshots, dpi_scale);
    const float log_height = ImGui::GetTextLineHeightWithSpacing() * 10.0F;
    draw_list(servers, state, snapshots, dpi_scale, log_height);
    draw_log(servers, state, snapshots, log_height);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("broadcast_form", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(gui_text::broadcast_form_title);
    ImGui::Separator();

    // The buttons stay pinned below a scrolling field area.
    const float buttons_height = ImGui::GetFrameHeightWithSpacing() +
                                 ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("broadcast_fields", ImVec2(0.0F, -buttons_height));
    gui_form::draw(state.form, dpi_scale);
    ImGui::EndChild();

    ImGui::Separator();
    const int target = gui_form::target_id(state.form);
    const bool editing = target != 0;
    if (ImGui::Button(editing ? gui_text::broadcast_save_and_start
                              : gui_text::broadcast_add_and_start)) {
        submit(servers, state, true);
    }
    ImGui::SameLine();
    if (ImGui::Button(editing ? gui_text::broadcast_save : gui_text::broadcast_add)) {
        submit(servers, state, false);
    }
    ImGui::EndChild();

    ImGui::EndChild();  // broadcast_host

    gui_status::draw(state.status);
}

} // namespace gui_view

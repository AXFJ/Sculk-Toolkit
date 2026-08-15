#include "gui_broadcast_form.h"

#include "gui_dialog.h"
#include "gui_favicon.h"
#include "gui_text.h"

#include "imgui.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string_view>

namespace {

template <std::size_t Size>
void assign(std::array<char, Size>& field, const std::string& value) {
    field.fill('\0');
    std::snprintf(field.data(), field.size(), "%s", value.c_str());
}

bool parse_int(const std::array<char, 8>& field, int minimum, int maximum,
               const char* name, int& value, std::string& error) {
    const std::string_view text(field.data());
    int parsed = 0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || code != std::errc{} || end != text.data() + text.size() ||
        parsed < minimum || parsed > maximum) {
        char message[128]{};
        std::snprintf(message, sizeof(message), gui_text::form_invalid_number, name,
                      minimum, maximum);
        error = message;
        return false;
    }
    value = parsed;
    return true;
}

std::string trim_field(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// "Steve:uuid, Alex" — the uuid half is optional.
void split_players(const std::string& text,
                   std::vector<std::string>& names,
                   std::vector<std::string>& uuids) {
    names.clear();
    uuids.clear();
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string::npos ? text.size() : comma;
        const std::string entry = trim_field(text.substr(start, end - start));
        if (!entry.empty()) {
            const std::size_t colon = entry.find(':');
            if (colon == std::string::npos) {
                names.push_back(entry);
                uuids.emplace_back();
            } else {
                names.push_back(trim_field(entry.substr(0, colon)));
                uuids.push_back(trim_field(entry.substr(colon + 1)));
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
}

std::string join_players(const std::vector<std::string>& names,
                         const std::vector<std::string>& uuids) {
    std::string joined;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += names[index];
        if (index < uuids.size() && !uuids[index].empty()) {
            joined += ':';
            joined += uuids[index];
        }
    }
    return joined;
}

bool uses_lan(int mode_index) {
    return mode_index == 0 || mode_index == 1;
}

bool uses_fake(int mode_index) {
    return mode_index == 1 || mode_index == 2;
}

// Every row is a label plus one full-width input, so the form stays readable at
// any pane width.
template <std::size_t Size>
void draw_row(const char* label, std::array<char, Size>& field, float width,
              bool disabled = false, const char* hint = nullptr) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(width);
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::BeginDisabled(disabled);
    ImGui::PushID(label);
    if (hint != nullptr) {
        ImGui::InputTextWithHint("##field", hint, field.data(), field.size());
    } else {
        ImGui::InputText("##field", field.data(), field.size());
    }
    ImGui::PopID();
    ImGui::EndDisabled();
}

void draw_combo(const char* label, const char* const* items, int count,
                int& index, float width) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(width);
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::PushID(label);
    if (ImGui::BeginCombo("##combo", items[index])) {
        for (int option = 0; option < count; ++option) {
            const bool selected = option == index;
            if (ImGui::Selectable(items[option], selected)) {
                index = option;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

} // namespace

namespace gui_form {

BroadcastForm::BroadcastForm() {
    reset(*this);
}

void reset(BroadcastForm& form) {
    assign(form.id, "");
    assign(form.group, "");
    form.mode_index = 1;
    assign(form.lan_port, "25565");
    assign(form.lan_motd, "Steve - 新的世界");
    assign(form.port, "25565");
    assign(form.motd, "A Minecraft Server");
    assign(form.version, "1.21.11");
    assign(form.protocol, "774");
    assign(form.online_players, "1");
    assign(form.max_players, "20");
    assign(form.players, "Steve");
    form.secure_chat_index = 1;
    assign(form.kick_message, "");
    assign(form.favicon, "");
    form.favicon_path.clear();
    form.favicon_error.clear();
}

void clear(BroadcastForm& form) {
    assign(form.id, "");
    assign(form.group, "");
    form.mode_index = 1;
    assign(form.lan_port, "");
    assign(form.lan_motd, "");
    assign(form.port, "");
    assign(form.motd, "");
    assign(form.version, "");
    assign(form.protocol, "");
    assign(form.online_players, "");
    assign(form.max_players, "");
    assign(form.players, "");
    form.secure_chat_index = 1;
    assign(form.kick_message, "");
    assign(form.favicon, "");
    form.favicon_path.clear();
    form.favicon_error.clear();
}

void load(BroadcastForm& form, const FakeServerConfig& config) {
    form.mode_index = config.mode == FakeServerMode::Udp ? 0
                    : config.mode == FakeServerMode::Both ? 1 : 2;
    assign(form.lan_port, std::to_string(config.lan_port));
    assign(form.lan_motd, config.lan_motd);
    assign(form.port, std::to_string(config.port));
    assign(form.motd, config.motd);
    assign(form.version, config.version);
    assign(form.protocol, std::to_string(config.protocol));
    assign(form.online_players, std::to_string(config.online_players));
    assign(form.max_players, std::to_string(config.max_players));
    assign(form.players, join_players(config.players, config.player_uuids));
    form.secure_chat_index = config.secure_chat == SecureChatSetting::Yes ? 0
                          : config.secure_chat == SecureChatSetting::No ? 1 : 2;
    assign(form.kick_message, config.kick_message);
    assign(form.favicon, config.favicon);
    assign(form.group, config.group);
    form.random_player_list_order = config.random_player_list_order;
    form.favicon_path.clear();
    form.favicon_error.clear();
}

// Every value field empty: the user wants the documented defaults.
bool fields_blank(const BroadcastForm& form) {
    const std::array<const char*, 11> fields{
        form.lan_port.data(), form.lan_motd.data(), form.port.data(), form.motd.data(),
        form.version.data(), form.protocol.data(), form.online_players.data(),
        form.max_players.data(), form.players.data(), form.kick_message.data(),
        form.favicon.data()};
    return std::all_of(fields.begin(), fields.end(),
                       [](const char* field) { return field[0] == '\0'; });
}

bool build(const BroadcastForm& form, FakeServerConfig& config, std::string& error) {
    if (fields_blank(form)) {
        BroadcastForm defaults;
        defaults.mode_index = form.mode_index;
        defaults.secure_chat_index = form.secure_chat_index;
        assign(defaults.id, form.id.data());
        assign(defaults.group, form.group.data());
        return build(defaults, config, error);
    }

    config = FakeServerConfig{};
    config.mode = form.mode_index == 0 ? FakeServerMode::Udp
                : form.mode_index == 1 ? FakeServerMode::Both : FakeServerMode::Tcp;

    if (uses_lan(form.mode_index)) {
        int lan_port = 0;
        if (!parse_int(form.lan_port, 1, 65535, gui_text::form_lan_port, lan_port, error)) {
            return false;
        }
        config.lan_port = static_cast<std::uint16_t>(lan_port);
        config.lan_motd = form.lan_motd.data();
    }

    if (uses_fake(form.mode_index)) {
        int port = 0;
        // In the combined mode the TCP port must match the advertised LAN port.
        const std::array<char, 8>& port_field =
            form.mode_index == 1 ? form.lan_port : form.port;
        if (!parse_int(port_field, 1, 65535, gui_text::form_port, port, error)) {
            return false;
        }
        config.port = static_cast<std::uint16_t>(port);
        config.motd = form.motd.data();
        config.version = form.version.data();
        if (!parse_int(form.protocol, 0, 1000000, gui_text::form_protocol,
                       config.protocol, error) ||
            !parse_int(form.online_players, 0, 1000000, gui_text::form_online,
                       config.online_players, error) ||
            !parse_int(form.max_players, 0, 1000000, gui_text::form_max,
                       config.max_players, error)) {
            return false;
        }
        split_players(form.players.data(), config.players, config.player_uuids);
        config.secure_chat = form.secure_chat_index == 0 ? SecureChatSetting::Yes
                           : form.secure_chat_index == 1 ? SecureChatSetting::No
                                                         : SecureChatSetting::Absent;
        config.kick_message = form.kick_message.data();
        config.favicon = form.favicon.data();
    }

    if (config.mode == FakeServerMode::Both) {
        config.lan_port = config.port;
    }
    config.group = form.group.data();
    config.random_player_list_order = form.random_player_list_order;
    return true;
}

int target_id(const BroadcastForm& form) {
    const std::string_view text(form.id.data());
    int id = 0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (text.empty() || code != std::errc{} || end != text.data() + text.size() || id <= 0) {
        return 0;
    }
    return id;
}

void set_id(BroadcastForm& form, int id) {
    assign(form.id, std::to_string(id));
}

// Modal dialog that lets the user add, remove, reorder, and edit individual
// player name / UUID pairs.
void draw_player_list_editor(BroadcastForm& form, float dpi_scale) {
    if (!form.player_list_editor_open) return;

    ImGui::SetNextWindowSize(ImVec2(440.0F * dpi_scale, 320.0F * dpi_scale),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(gui_text::form_player_editor_title,
                      &form.player_list_editor_open)) {
        ImGui::End();
        return;
    }

    // Add row.
    static std::array<char, 128> new_name{};
    static std::array<char, 64>  new_uuid{};
    ImGui::SetNextItemWidth(120.0F * dpi_scale);
    ImGui::InputTextWithHint("##edit_name", gui_text::form_player_editor_name,
                             new_name.data(), new_name.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190.0F * dpi_scale);
    ImGui::InputTextWithHint("##edit_uuid", gui_text::form_player_editor_uuid,
                             new_uuid.data(), new_uuid.size());
    ImGui::SameLine();
    if (ImGui::Button(gui_text::form_player_editor_add) && new_name[0] != '\0') {
        form.edit_names.emplace_back(new_name.data());
        form.edit_uuids.emplace_back(new_uuid.data());
        new_name.fill('\0');
        new_uuid.fill('\0');
    }

    // Drag-to-reorder list — fills all remaining space above the buttons.
    const float button_area = ImGui::GetFrameHeightWithSpacing() * 1.2F;
    ImGui::BeginChild("player_edit_list",
                      ImVec2(0.0F, -button_area),
                      ImGuiChildFlags_Borders);
    const int count = static_cast<int>(form.edit_names.size());
    for (int index = 0; index < count; ++index) {
        ImGui::PushID(index);

        // Drag source.
        ImGui::Button("≡", ImVec2(ImGui::GetFrameHeight(), 0.0F));
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload("PLAYER_EDIT", &index, sizeof(int));
            ImGui::TextUnformatted(form.edit_names[index].c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("PLAYER_EDIT")) {
                const int source = *static_cast<const int*>(payload->Data);
                if (source >= 0 && source < count && source != index) {
                    std::swap(form.edit_names[source], form.edit_names[index]);
                    std::swap(form.edit_uuids[source], form.edit_uuids[index]);
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0F * dpi_scale);
        ImGui::InputText("##pname", form.edit_names[index].data(),
                         form.edit_names[index].size() + 1,
                         ImGuiInputTextFlags_CallbackResize,
                         [](ImGuiInputTextCallbackData* data) -> int {
                             auto* vec = static_cast<std::string*>(data->UserData);
                             vec->resize(data->BufTextLen);
                             return 0;
                         },
                         &form.edit_names[index]);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0F * dpi_scale);
        ImGui::InputText("##puuid", form.edit_uuids[index].data(),
                         form.edit_uuids[index].size() + 1,
                         ImGuiInputTextFlags_CallbackResize,
                         [](ImGuiInputTextCallbackData* data) -> int {
                             auto* vec = static_cast<std::string*>(data->UserData);
                             vec->resize(data->BufTextLen);
                             return 0;
                         },
                         &form.edit_uuids[index]);
        ImGui::SameLine();
        if (ImGui::Button("×")) {
            form.edit_names.erase(form.edit_names.begin() + index);
            form.edit_uuids.erase(form.edit_uuids.begin() + index);
            ImGui::PopID();
            break;  // iterate fresh next frame
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    if (ImGui::Button(gui_text::form_confirm)) {
        form.player_list_editor_open = false;
        assign(form.players,
               join_players(form.edit_names, form.edit_uuids));
    }
    ImGui::SameLine();
    if (ImGui::Button(gui_text::form_cancel)) {
        form.player_list_editor_open = false;
    }
    ImGui::End();
}

void draw(BroadcastForm& form, float dpi_scale) {
    const float label_width = 150.0F * dpi_scale;
    // An id targets an existing broadcast; leaving it empty creates a new one.
    draw_row(gui_text::form_id, form.id, label_width, false, gui_text::form_id_hint);
    draw_row(gui_text::form_group, form.group, label_width, false, gui_text::form_group_hint);
    const char* modes[] = {gui_text::form_mode_lan, gui_text::form_mode_both,
                           gui_text::form_mode_fake};
    draw_combo(gui_text::form_mode, modes, 3, form.mode_index, label_width);

    if (uses_lan(form.mode_index)) {
        ImGui::SeparatorText(gui_text::form_section_lan);
        draw_row(gui_text::form_lan_port, form.lan_port, label_width);
        draw_row(gui_text::form_lan_motd, form.lan_motd, label_width);
    }

    if (uses_fake(form.mode_index)) {
        ImGui::SeparatorText(gui_text::form_section_fake);
        if (form.mode_index == 1) {
            // Mirror the LAN port so both halves always agree.
            std::array<char, 8> mirrored = form.lan_port;
            draw_row(gui_text::form_port, mirrored, label_width, true);
        } else {
            draw_row(gui_text::form_port, form.port, label_width);
        }
        draw_row(gui_text::form_motd, form.motd, label_width);
        draw_row(gui_text::form_version, form.version, label_width);
        draw_row(gui_text::form_protocol, form.protocol, label_width);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(gui_text::form_players_count);
        ImGui::SameLine(label_width);
        const float number_width = 90.0F * dpi_scale;
        ImGui::SetNextItemWidth(number_width);
        ImGui::InputText("##online", form.online_players.data(), form.online_players.size());
        ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x * 0.5F);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("/");
        ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x * 0.5F);
        ImGui::SetNextItemWidth(number_width);
        ImGui::InputText("##max", form.max_players.data(), form.max_players.size());

        // Player list row: text input plus a "..." button to open the editor.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(gui_text::form_player_list);
        ImGui::SameLine(label_width);
        const float button_w = ImGui::CalcTextSize("...").x +
                               ImGui::GetStyle().FramePadding.x * 2.0F +
                               ImGui::GetStyle().ItemSpacing.x;
        const float input_w = ImGui::GetContentRegionAvail().x - button_w;
        ImGui::SetNextItemWidth(input_w);
        ImGui::InputTextWithHint("##players", gui_text::form_player_list_hint,
                                 form.players.data(), form.players.size());
        ImGui::SameLine(0.0F, 2.0F);
        if (ImGui::Button("...")) {
            split_players(form.players.data(),
                          form.edit_names, form.edit_uuids);
            form.player_list_editor_open = true;
        }

        draw_player_list_editor(form, dpi_scale);

        // Random player-list order.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(gui_text::form_random_player_order);
        ImGui::SameLine(label_width);
        ImGui::Checkbox("##random_order", &form.random_player_list_order);

        const char* secure[] = {gui_text::details_yes, gui_text::details_no,
                                gui_text::form_secure_absent};
        draw_combo(gui_text::form_secure_chat, secure, 3, form.secure_chat_index, label_width);
        draw_row(gui_text::form_kick, form.kick_message, label_width, false,
                 gui_text::form_kick_hint);

        // First row imports a file and shows its path; the second row holds the
        // base64 the server actually serves, filled in by the import.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(gui_text::form_favicon);
        ImGui::SameLine(label_width);
        if (ImGui::Button(gui_text::form_favicon_import)) {
            const std::wstring path = gui_dialog::open_png_file();
            if (!path.empty()) {
                std::string error;
                const std::string data_uri = gui_favicon::encode_png_data_uri(path, error);
                form.favicon_path = gui_dialog::to_utf8(path);
                form.favicon_error = error;
                if (!data_uri.empty()) {
                    assign(form.favicon, data_uri);
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(form.favicon_path.empty() ? gui_text::form_favicon_path_hint
                                                         : form.favicon_path.c_str());

        draw_row(gui_text::form_favicon_base64, form.favicon, label_width, false,
                 gui_text::form_favicon_hint);
        if (!form.favicon_error.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), "%s",
                               form.favicon_error.c_str());
        }
    }
}

} // namespace gui_form

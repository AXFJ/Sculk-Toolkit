#pragma once

#include "core/fake_server.h"

#include <array>
#include <string>

// The broadcast form is reused by the create pane and the edit dialog, so its
// state and drawing live apart from either.
namespace gui_form {

struct BroadcastForm {
    // Target server id; empty means "create a new broadcast".
    std::array<char, 8> id{};
    // Broadcasts sharing a group collapse into one list row.
    std::array<char, 128> group{};
    // 0 = LAN only, 1 = LAN plus fake server, 2 = fake server only.
    int mode_index = 1;
    std::array<char, 8> lan_port{};
    std::array<char, 256> lan_motd{};
    std::array<char, 8> port{};
    std::array<char, 512> motd{};
    std::array<char, 64> version{};
    std::array<char, 8> protocol{};
    std::array<char, 8> online_players{};
    std::array<char, 8> max_players{};
    std::array<char, 1024> players{};
    bool random_player_list_order = false;
    // Player list editor dialog.
    bool player_list_editor_open = false;
    std::vector<std::string> edit_names;
    std::vector<std::string> edit_uuids;
    // 0 = yes, 1 = no, 2 = omitted.
    int secure_chat_index = 1;
    std::array<char, 512> kick_message{};
    // Imported file path, shown for reference only; the base64 field is what the
    // server actually serves.
    std::string favicon_path;
    std::array<char, 16384> favicon{};
    std::string favicon_error;

    BroadcastForm();
};

// Restores every field to the documented default.
void reset(BroadcastForm& form);

// Empties every field, for after a broadcast has been submitted.
void clear(BroadcastForm& form);

// Fills the form from an existing configuration, for the edit dialog.
void load(BroadcastForm& form, const FakeServerConfig& config);

// Validates the fields and produces a configuration.
bool build(const BroadcastForm& form, FakeServerConfig& config, std::string& error);

// Draws the input rows; the caller adds the buttons underneath.
void draw(BroadcastForm& form, float dpi_scale);

// Server id typed into the form, or 0 when the field is empty or invalid.
int target_id(const BroadcastForm& form);

// Points the form at an existing server, for the edit button.
void set_id(BroadcastForm& form, int id);

} // namespace gui_form

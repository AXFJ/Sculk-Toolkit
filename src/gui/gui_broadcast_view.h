#pragma once

#include "core/fake_server.h"
#include "gui_broadcast_form.h"
#include "gui_status_bar.h"

#include <array>
#include <map>
#include <string>
#include <vector>

// Broadcast tab: the fake-server list with its toolbar on the left and the
// create form on the right, plus the edit dialog.
namespace gui_view {

struct BroadcastPanelState {
    // One reusable form: empty id creates, a filled id overwrites that server.
    gui_form::BroadcastForm form;
    std::vector<int> selection;
    // Anchor for shift-click range selection.
    int anchor_id = 0;
    // Group applied to exports and imports; empty keeps each entry's own group.
    std::array<char, 128> group_input{};
    // Log export path preset by the button, editable by the user.
    std::array<char, 512> export_path{};
    // Groups the user expanded; collapsed is the default.
    std::map<std::string, bool> expanded_groups;
    gui_status::Bar status;
};

void draw_broadcast_panel(FakeServerManager& servers,
                          BroadcastPanelState& state,
                          float dpi_scale);

} // namespace gui_view

#pragma once

#include "core/lan_monitor.h"
#include "gui_scan_view.h"

#include <vector>

namespace gui_view {

// Floating window that lists every analysis item for a single world.
void draw_analyse_details_window(ScanPanelState& state,
                                 const std::vector<LanWorldRecord>& worlds,
                                 float dpi_scale);

} // namespace gui_view

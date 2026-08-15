#pragma once

#include "core/lan_monitor.h"
#include "gui_motd.h"
#include "gui_scan_view.h"

namespace gui_view {

void draw_log_window(LanMonitor& monitor, bool& open, float dpi_scale,
                     gui_motd::Mode mode);
void draw_scan_log_window(LanMonitor& monitor, ScanPanelState& state, float dpi_scale);

} // namespace gui_view

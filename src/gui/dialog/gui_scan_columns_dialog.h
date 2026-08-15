#pragma once

#include "gui_scan_view.h"

namespace gui_view {

struct ColumnInfo {
    const char* label;
    int characters;
};

ColumnInfo column_info(int column);

// Column picker: visibility plus order, applied to the world list.
void draw_columns_window(ScanPanelState& state, float dpi_scale);

} // namespace gui_view

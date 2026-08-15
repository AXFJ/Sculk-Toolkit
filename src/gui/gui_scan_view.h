#pragma once

#include "core/lan_monitor.h"
#include "core/server_probe.h"
#include "gui_favicon.h"

#include <d3d11.h>

#include <array>
#include <string>
#include <vector>

// Widget drawing for the scan screen. Separated from the Win32/D3D11 host so
// layout changes never touch device or message-loop code.
namespace gui_view {

// Columns of the world list, in their default order. Analysis columns follow
// the ordinary columns; only LanWorld is enabled by default.
enum class ScanColumn {
    Motd,
    Ip,
    Port,
    Players,
    Version,
    PlayerList,
    Latency,
    LanWorld,
    IsAvailable,
    IsVanillaVersion,
    IsLanMotdClean,
    IsMotdClean,
    IsRegularLanMotd,
    StrVersion,
    StrServerType,
    IntActualOnline,
    StrActualServerVersion,
    IsDefaultPort,
    IsCorrectOnline,
    IsCorrectProtocol,
    IntLanMotdColorChar,
    IsMotdsEqual,
    Count
};

struct ScanPanelState {
    // Display order and visibility of the list columns. The analysis columns
    // (IsAvailable onward) are present but hidden; only LanWorld starts on.
    std::vector<int> column_order{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                  13, 14, 15, 16, 17, 18, 19, 20, 21};
    std::array<bool, static_cast<std::size_t>(ScanColumn::Count)> column_visible{
        true, true, true, true, true, true, true, true,
        false, false, false, false, false, false, false, false,
        false, false, false, false, false, false};
    bool columns_open = false;
    // Typed scan duration; empty means continuous. The combo writes into it.
    std::array<char, 8> seconds_input{};
    // 0 = UDP only, 1 = UDP plus server-list-ping.
    int mode_index = 1;
    // Index into the automatic re-query presets; 0 disables it.
    int auto_slp_index = 0;
    // 0 = clean, 1 = format, 2 = raw.
    int motd_mode_index = 0;
    // Analysis details popup.
    bool analyse_details_open = false;
    std::string analyse_ip;
    unsigned short analyse_port = 0;
    bool log_open = false;
    // Endpoint typed into the details pane, or filled in by clicking a row.
    std::array<char, 64> endpoint_input{};
    std::string endpoint_error;
    gui_favicon::Texture favicon;
};

// Draws the scan tab contents: the scan/stop button, the duration and mode
// combos, the world table, the counters, and the details pane. All sizes are
// multiplied by dpi_scale so the layout matches the monitor.
void draw_scan_panel(LanMonitor& monitor,
                     ServerProbe& probe,
                     ScanPanelState& state,
                     ID3D11Device* device,
                     float dpi_scale);


} // namespace gui_view

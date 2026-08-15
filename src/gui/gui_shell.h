#pragma once

#include "gui_broadcast_view.h"
#include "gui_database_view.h"
#include "gui_scan_view.h"

struct ID3D11Device;
class LanMonitor;
class ServerProbe;
class FakeServerManager;

// Owns the full-viewport window and the tab bar, and hands each tab to its own
// view module.
namespace gui_shell {

struct Context {
    LanMonitor* monitor = nullptr;
    ServerProbe* probe = nullptr;
    FakeServerManager* servers = nullptr;
    gui_view::ScanPanelState* scan = nullptr;
    gui_view::BroadcastPanelState* broadcast = nullptr;
    gui_view::DatabasePanelState* database = nullptr;
    ID3D11Device* device = nullptr;
    float dpi_scale = 1.0F;
};

void draw(const Context& context);

} // namespace gui_shell

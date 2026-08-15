#pragma once

#include "core/fake_server.h"
#include "motd_mode.h"
#include "core/scan.h"
#include "scan_mode.h"

#include <string>

std::string format_world(const LanWorld& world, ScanMode mode, MotdMode motd_mode);
std::string format_server_status(const std::string& host,
                                 unsigned short port,
                                 const ServerStatus& status,
                                 MotdMode motd_mode);
std::string render_fake_server(const FakeServerSnapshot& server,
                               ScanMode mode,
                               MotdMode motd_mode);

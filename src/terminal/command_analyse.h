#pragma once

#include "motd_mode.h"
#include "scan_mode.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class CommandType {
    Empty,
    Scan,
    PingServer,
    FakeServer,
    Help,
    ListTasks,
    ShowTask,
    RemoveTask,
    Exit,
    Invalid
};

struct ScanOptions {
    int seconds = 10;
    bool export_results = false;
    ScanMode mode = ScanMode::Compact;
    MotdMode motd_mode = MotdMode::Clean;
};

struct PingServerOptions {
    std::string host;
    std::uint16_t port = 25565;
    bool no_favicon = false;
    MotdMode motd_mode = MotdMode::Clean;
};

enum class FakeServerAction {
    New,
    Start,
    Stop,
    Remove,
    Modify,
    List,
    Export,
    Import
};

struct FakeServerCommandOptions {
    FakeServerAction action = FakeServerAction::List;
    std::string mode;
    int id = 0;
    bool all = false;
    std::string attributes_json;
    ScanMode display_mode = ScanMode::Compact;
    MotdMode motd_mode = MotdMode::Clean;
    std::string attribute;
    std::string value;
    std::string filename;
};

struct Command {
    CommandType type = CommandType::Empty;
    ScanOptions scan;
    PingServerOptions ping_server;
    FakeServerCommandOptions fake_server;
    bool asynchronous = false;
    int task_id = 0;
    std::string source;
    std::string error;
};

class CommandParser {
public:
    Command parse(const std::string& input) const;

private:
    Command parse_regular(const std::string& input) const;
};

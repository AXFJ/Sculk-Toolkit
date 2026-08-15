#include "terminal_app.h"
#include "core/fake_server.h"
#include "core/scan.h"
#include "core/status_query.h"
#include "command_analyse.h"
#include "console.h"
#include "favicon.h"
#include "render_world.h"
#include "task_manager.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using OutputHandler = std::function<void(const std::string&)>;
using CancelHandler = std::function<bool()>;
using ProgressHandler = std::function<void(double)>;

std::string information(const std::string& text) {
    return console::colorize("{COLOR_WHITE}" + text + "{COLOR_RESET}");
}

std::string error_message(const std::string& text) {
    return console::colorize("{COLOR_RED}" + text + "{COLOR_RESET}");
}

std::string world_key(const LanWorld& world) {
    return world.ip + ':' + std::to_string(world.port);
}

std::string help_text() {
    return
        "\u53ef\u7528\u547d\u4ee4:\n"
        "  scan\n"
        "  pingserver (ps)\n"
        "  fakeserver (fs): new, start, stop, remove, modify, list, export, import\n"
        "  help\n"
        "  &<command>\n"
        "  &\n"
        "  & <task-id>\n"
        "  & remove <task-id>\n"
        "  exit (quit)\n";
}

void execute_fake_server_command(const Command& command,
                                 FakeServerManager& fake_servers,
                                 const OutputHandler& output) {
    const auto& options = command.fake_server;
    if (options.action == FakeServerAction::New) {
        FakeServerConfig config;
        if (!parse_fake_server_mode(options.mode, config.mode)) {
            output(error_message("无效的假服务器模式。\n"));
            return;
        }
        if (!options.attributes_json.empty()) {
            std::string attributes_error;
            if (!apply_fake_server_attributes_json(config, options.attributes_json,
                                                   attributes_error)) {
                output(error_message("attributes JSON 无效: " + attributes_error + "\n"));
                return;
            }
        }
        const int id = fake_servers.create(std::move(config));
        output(information("已创建假服务器，ID: " + std::to_string(id) + "（未启动）。\n"));
        return;
    }

    if (options.action == FakeServerAction::List) {
        const auto servers = fake_servers.list();
        if (servers.empty()) {
            output(information("当前没有假服务器。\n"));
            return;
        }
        for (const auto& server : servers) {
            output(render_fake_server(server, options.display_mode, options.motd_mode));
            output("\n");
        }
        return;
    }

    if (options.action == FakeServerAction::Modify) {
        std::string modify_error;
        if (!fake_servers.modify(options.id, options.attribute, options.value, modify_error)) {
            output(error_message("修改失败: " + modify_error + "\n"));
        } else {
            output(information("假服务器 " + std::to_string(options.id) + " 已修改。\n"));
        }
        return;
    }

    if (options.action == FakeServerAction::Export) {
        std::string export_error;
        const auto path = fake_servers.export_all(export_error);
        if (path.empty()) output(error_message("导出失败: " + export_error + "\n"));
        else output(information("假服务器已导出到 " + path.generic_string() + "\n"));
        return;
    }

    if (options.action == FakeServerAction::Import) {
        std::string import_error;
        const auto ids = fake_servers.import_file(options.filename, import_error);
        if (!import_error.empty()) {
            output(error_message("导入失败: " + import_error + "\n"));
        } else {
            output(information("已导入 " + std::to_string(ids.size()) + " 个假服务器。\n"));
        }
        return;
    }

    const std::vector<int> targets = options.all
        ? fake_servers.ids() : std::vector<int>{options.id};
    if (targets.empty()) {
        output(information("当前没有假服务器。\n"));
        return;
    }
    for (const int id : targets) {
        std::string action_error;
        bool success = false;
        std::string action_name;
        if (options.action == FakeServerAction::Start) {
            output(error_message("fs start 必须使用 &fs start <ID|all> 异步运行。\n"));
            return;
        } else if (options.action == FakeServerAction::Stop) {
            success = fake_servers.stop(id, action_error);
            action_name = "停止";
        } else {
            success = fake_servers.remove(id, action_error);
            action_name = "删除";
        }
        if (success) {
            output(information("假服务器 " + std::to_string(id) + " 已" + action_name + "。\n"));
        } else {
            output(error_message("假服务器 " + std::to_string(id) + ' ' +
                                 action_name + "失败: " + action_error + "\n"));
        }
    }
}

void run_fake_server_task(int server_id,
                          FakeServerManager& fake_servers,
                          TaskContext& context) {
    context.set_progress_indeterminate();
    std::string start_error;
    TaskContext log_context = context;
    if (!fake_servers.start(
            server_id,
            [server_id, log_context](const std::string& event,
                                      const std::string& ip,
                                      const std::string& player) mutable {
                std::string text;
                if (event == "login") {
                    text = "登录尝试: " + ip + "，玩家 " + player;
                } else {
                    text = "Ping 请求: " + ip;
                }
                log_context.append_output(information(
                    "[Fakeserver #" + std::to_string(server_id) + "] " + text + "\n"));
            },
            start_error)) {
        context.append_output(error_message(
            "假服务器 " + std::to_string(server_id) + " 启动失败: " + start_error + "\n"));
        return;
    }

    context.append_output(information(
        "假服务器 " + std::to_string(server_id) + " 已启动，正在持续收集日志。\n"));
    while (!context.cancelled() && fake_servers.is_running(server_id)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (context.cancelled() && fake_servers.is_running(server_id)) {
        std::string stop_error;
        if (!fake_servers.stop(server_id, stop_error)) {
            context.append_output(error_message(
                "假服务器 " + std::to_string(server_id) + " 停止失败: " + stop_error + "\n"));
        }
        return;
    }

    context.append_output(information(
        "假服务器 " + std::to_string(server_id) + " 已停止。\n"));
    context.set_progress(1.0);
}

void execute_command(const Command& command,
                     FakeServerManager& fake_servers,
                     const OutputHandler& output,
                     const CancelHandler& cancelled,
                     const ProgressHandler& progress) {
    if (command.type == CommandType::Help) {
        output(information(help_text()));
        progress(1.0);
        return;
    }

    if (command.type == CommandType::FakeServer) {
        progress(0.0);
        execute_fake_server_command(command, fake_servers, output);
        progress(1.0);
        return;
    }

    if (command.type == CommandType::PingServer) {
        progress(0.0);
        const std::string endpoint = command.ping_server.host + ':' +
                                     std::to_string(command.ping_server.port);
        output(information("正在查询服务器 " + endpoint + "...\n"));
        const ServerStatus status = query_server_status(
            command.ping_server.host,
            command.ping_server.port,
            cancelled,
            !command.ping_server.no_favicon);
        output(format_server_status(command.ping_server.host,
                                    command.ping_server.port,
                                    status,
                                    command.ping_server.motd_mode));

        if (!command.ping_server.no_favicon && status.available) {
            if (status.favicon.empty()) {
                output(information("服务器未提供 favicon。\n"));
            } else {
                std::string favicon_error;
                const std::string favicon = render_favicon(status.favicon, favicon_error);
                if (favicon.empty()) {
                    output(error_message("favicon 输出失败: " + favicon_error + "\n"));
                } else {
                    output(information("Favicon:\n"));
                    output(favicon);
                }
            }
        }
        progress(1.0);
        return;
    }

    if (command.type != CommandType::Scan) {
        output(error_message("该命令不能作为后台任务执行。\n"));
        progress(1.0);
        return;
    }

    const bool only_udp = command.scan.mode == ScanMode::OnlyUdp;
    output(information("开始监听 Minecraft 局域网广播，持续 " +
                       std::to_string(command.scan.seconds) +
                       (only_udp ? " 秒（仅 UDP）...\n" : " 秒（UDP + TCP 状态查询）...\n")));

    std::mutex output_mutex;
    std::mutex statuses_mutex;
    std::unordered_map<std::string, ServerStatus> statuses;
    std::vector<std::future<void>> queries;

    const auto emit_world = [&](const LanWorld& world) {
        std::lock_guard lock(output_mutex);
        output(format_world(world, command.scan.mode, command.scan.motd_mode));
        output("\n");
    };

    auto report = scan_lan_worlds(
        std::chrono::seconds(command.scan.seconds),
        [&](const LanWorld& world) {
            if (only_udp) {
                emit_world(world);
                return;
            }

            queries.push_back(std::async(std::launch::async, [&, world] {
                LanWorld queried_world = world;
                try {
                    queried_world.status = query_server_status(world.ip, world.port, cancelled, false);
                } catch (const std::exception& exception) {
                    ServerStatus status;
                    status.error = std::string("TCP 查询异常: ") + exception.what();
                    queried_world.status = std::move(status);
                } catch (...) {
                    ServerStatus status;
                    status.error = "TCP 查询发生未知异常";
                    queried_world.status = std::move(status);
                }

                {
                    std::lock_guard lock(statuses_mutex);
                    statuses.insert_or_assign(world_key(world), *queried_world.status);
                }
                if (!cancelled()) {
                    emit_world(queried_world);
                }
            }));
        },
        cancelled,
        progress);

    for (auto& query : queries) {
        query.get();
    }

    if (!only_udp) {
        std::lock_guard lock(statuses_mutex);
        for (auto& world : report.worlds) {
            const auto iterator = statuses.find(world_key(world));
            if (iterator != statuses.end()) {
                world.status = iterator->second;
            }
        }
    }

    if (!report.error.empty()) {
        output(error_message("扫描失败: " + report.error + "\n"));
        return;
    }

    if (report.cancelled) {
        output(information("扫描已取消。\n"));
        return;
    }

    if (report.worlds.empty()) {
        output(information("扫描结束，未发现公开的局域网世界。\n"));
    } else {
        output(information("扫描结束，共发现 " + std::to_string(report.worlds.size()) + " 个世界。\n"));
    }

    if (command.scan.export_results) {
        std::string export_error;
        const auto path = export_scan_results(report.worlds, export_error);
        if (path.empty()) {
            output(error_message("导出失败: " + export_error + "\n"));
        } else {
            output(information("扫描结果已导出到 " + path.generic_string() + "\n"));
        }
    }
}

void print_task_list(TaskManager& tasks) {
    const auto snapshots = tasks.list();
    if (snapshots.empty()) {
        std::cout << information("当前没有异步任务。\n");
        return;
    }

    std::cout << console::colorize("{COLOR_GRAY}ID    状态       进度    命令\n{COLOR_RESET}");
    for (const auto& task : snapshots) {
        const std::string progress = task.progress_available
            ? std::to_string(static_cast<int>(task.progress * 100.0)) + "%"
            : "N/A";
        std::cout << information(
            std::to_string(task.id) + "     " + task_status_text(task.status) + "       " +
            progress + "     " + task.command + '\n');
    }
}

} // namespace

int run_terminal() {
    if (!console::initialize()) {
        return EXIT_FAILURE;
    }

    CommandParser parser;
    FakeServerManager fake_servers;
    TaskManager tasks;

    std::cout << console::colorize("{COLOR_WHITE}Sculk Toolkit\n{COLOR_RESET}")
              << information("输入 help 查看命令。\n");

    std::string line;
    while (true) {
        std::cout << console::colorize("{COLOR_BLUE}> {COLOR_RESET}") << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }

        const Command command = parser.parse(line);
        if (command.type == CommandType::Empty) {
            continue;
        }
        if (command.type == CommandType::Invalid) {
            std::cout << error_message("命令错误: " + command.error + '\n');
            continue;
        }
        if (command.type == CommandType::Exit) {
            break;
        }
        if (command.type == CommandType::ListTasks) {
            print_task_list(tasks);
            continue;
        }
        if (command.type == CommandType::ShowTask) {
            const auto snapshot = tasks.snapshot(command.task_id);
            if (!snapshot) {
                std::cout << error_message("未找到该任务。\n");
                continue;
            }
            if (!snapshot->output.empty()) {
                std::cout << snapshot->output;
            } else {
                std::cout << information("该任务暂无输出。\n");
            }
            std::cout << information(
                "任务状态: " + task_status_text(snapshot->status) +
                "，进度 " + (snapshot->progress_available
                    ? std::to_string(static_cast<int>(snapshot->progress * 100.0)) + "%"
                    : "N/A") + "\n");
            continue;
        }
        if (command.type == CommandType::RemoveTask) {
            if (tasks.remove(command.task_id)) {
                std::cout << information("任务 " + std::to_string(command.task_id) + " 已终止并删除。\n");
            } else {
                std::cout << error_message("未找到该任务。\n");
            }
            continue;
        }

        if (command.type == CommandType::FakeServer &&
            command.fake_server.action == FakeServerAction::Start) {
            const std::vector<int> server_ids = command.fake_server.all
                ? fake_servers.ids() : std::vector<int>{command.fake_server.id};
            if (server_ids.empty()) {
                std::cout << information("当前没有假服务器。\n");
                continue;
            }
            for (const int server_id : server_ids) {
                const int task_id = tasks.start(
                    "Fakeserver (" + std::to_string(server_id) + ")",
                    [server_id, &fake_servers](TaskContext& context) {
                        run_fake_server_task(server_id, fake_servers, context);
                    },
                    false);
                std::cout << information(
                    "假服务器 " + std::to_string(server_id) +
                    " 已提交后台任务，任务 ID: " + std::to_string(task_id) + '\n');
            }
            continue;
        }

        if (command.asynchronous) {
            const int task_id = tasks.start(
                command.source,
                [command, &fake_servers](TaskContext& context) {
                    execute_command(
                        command,
                        fake_servers,
                        [&](const std::string& text) { context.append_output(text); },
                        [&] { return context.cancelled(); },
                        [&](double value) { context.set_progress(value); });
                });
            std::cout << information("任务已在后台启动，ID: " + std::to_string(task_id) + '\n');
            continue;
        }

        execute_command(
            command,
            fake_servers,
            [](const std::string& text) { std::cout << text << std::flush; },
            [] { return false; },
            [](double) {});
    }

    std::cout << information("正在退出...\n");
    return 0;
}

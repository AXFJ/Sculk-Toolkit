#include "gui_scan_log_dialog.h"

#include "gui_scan_columns_dialog.h"
#include "gui_text.h"

#include "imgui.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace gui_view {

namespace {

std::string clock_text(std::chrono::system_clock::time_point at) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(at);
    std::tm parts{};
    localtime_s(&parts, &raw);
    char text[16]{};
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d",
                  parts.tm_hour, parts.tm_min, parts.tm_sec);
    return text;
}

const char* event_label(LanEventType type) {
    switch (type) {
        case LanEventType::Started: return gui_text::event_started;
        case LanEventType::WorldFound: return gui_text::event_world_found;
        case LanEventType::WorldLost: return gui_text::event_world_lost;
        case LanEventType::WorldReturned: return gui_text::event_world_returned;
        case LanEventType::Stopped: return gui_text::event_stopped;
        default: return gui_text::event_failed;
    }
}

} // namespace

void draw_log_window(LanMonitor& monitor, bool& open, float dpi_scale,
                     gui_motd::Mode mode) {
    if (!open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0F * dpi_scale, 420.0F * dpi_scale),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(gui_text::log_window_title, &open)) {
        ImGui::End();
        return;
    }

    const std::vector<LanScanEvent> events = monitor.log();
    if (ImGui::Button(gui_text::log_clear_button)) {
        monitor.clear_log();
    }
    ImGui::Separator();

    if (events.empty()) {
        ImGui::TextUnformatted(gui_text::log_empty);
        ImGui::End();
        return;
    }

    ImGui::BeginChild("log_lines");
    for (const LanScanEvent& event : events) {
        const std::string target = event.ip.empty()
            ? event.detail
            : event.ip + ':' + std::to_string(event.port) + "  " +
                  gui_motd::flatten(event.detail, mode);
        ImGui::Text(gui_text::log_line_format, clock_text(event.at).c_str(),
                    event_label(event.type), target.c_str());
    }
    // Follow new entries while the newest line is already in view.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0F);
    }
    ImGui::EndChild();
    ImGui::End();
}

void draw_scan_log_window(LanMonitor& monitor, ScanPanelState& state, float dpi_scale) {
    const auto mode = [](int index) -> gui_motd::Mode {
        if (index == 1) return gui_motd::Mode::Format;
        if (index == 2) return gui_motd::Mode::Raw;
        return gui_motd::Mode::Clean;
    };
    draw_log_window(monitor, state.log_open, dpi_scale, mode(state.motd_mode_index));
    draw_columns_window(state, dpi_scale);
}

} // namespace gui_view

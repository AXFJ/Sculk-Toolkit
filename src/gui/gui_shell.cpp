#include "gui_shell.h"

#include "dialog/gui_about_dialog.h"
#include "dialog/gui_scan_log_dialog.h"
#include "gui_text.h"

#include "imgui.h"

namespace gui_shell {

namespace {
// The about window outlives any single frame, so it lives here rather than in
// the per-frame Context.
bool about_open = false;
} // namespace

void draw(const Context& context) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(gui_text::panel_id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Lighter tabs; the padding inside the labels widens them without changing
    // the frame padding every widget inside the tabs would inherit.
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.32F, 0.36F, 0.44F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.45F, 0.52F, 0.64F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.52F, 0.60F, 0.74F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.28F, 0.31F, 0.38F, 1.0F));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ImVec4(0.44F, 0.50F, 0.62F, 1.0F));

    const float tab_row_y = ImGui::GetCursorPosY();
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(gui_text::tab_scan)) {
            gui_view::draw_scan_panel(*context.monitor, *context.probe, *context.scan,
                                      context.device, context.dpi_scale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(gui_text::tab_broadcast)) {
            gui_view::draw_broadcast_panel(*context.servers, *context.broadcast,
                                           context.dpi_scale);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(gui_text::tab_database)) {
            gui_view::draw_database_panel(*context.database, context.dpi_scale);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // Draw the about button over the empty right end of the tab-bar row, so it
    // shares the tabs' line at the far right without claiming a separate region.
    const float about_width = ImGui::CalcTextSize(gui_text::about_button).x +
                              ImGui::GetStyle().FramePadding.x * 2.0F +
                              8.0F * context.dpi_scale;
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - about_width,
                               tab_row_y));
    if (ImGui::Button(gui_text::about_button)) {
        about_open = true;
    }

    ImGui::PopStyleColor(5);
    ImGui::End();

    // The log and about windows are floating windows drawn outside the panel.
    gui_view::draw_scan_log_window(*context.monitor, *context.scan, context.dpi_scale);
    gui_view::draw_about_window(about_open, context.dpi_scale);
}

} // namespace gui_shell

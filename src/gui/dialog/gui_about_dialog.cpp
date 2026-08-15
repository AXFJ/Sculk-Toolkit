#include "gui_about_dialog.h"

#include "gui_fonts.h"
#include "gui_text.h"

#include "imgui.h"

#include <algorithm>

namespace gui_view {

void draw_about_window(bool& open, float dpi_scale) {
    if (!open) {
        return;
    }

    // A generous fixed window, centred on the main viewport.
    const ImVec2 size(420.0F * dpi_scale, 300.0F * dpi_scale);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5F),
                            ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    if (!ImGui::Begin(gui_text::about_title, &open,
                      ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }

    // Centre the few lines of text in the window.
    const float text_height = ImGui::GetTextLineHeightWithSpacing() * 4.0F +
                              ImGui::GetStyle().ItemSpacing.y * 2.0F;
    const float padding = (ImGui::GetContentRegionAvail().y - text_height) * 0.5F;
    ImGui::Dummy(ImVec2(0.0F, std::max(0.0F, padding)));

    const float text_width = std::max({
        ImGui::CalcTextSize(gui_text::panel_id).x,
        ImGui::CalcTextSize(gui_text::about_version).x,
        ImGui::CalcTextSize(gui_text::about_author).x,
    });
    const float indent = (ImGui::GetContentRegionAvail().x - text_width) * 0.5F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, indent));

    ImGui::PushFont(gui_fonts::bold(), 0.0F);
    ImGui::TextUnformatted(gui_text::panel_id);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Text("%s", gui_text::about_version);
    ImGui::Text("%s", gui_text::about_author);

    ImGui::End();
}

} // namespace gui_view

#include "gui_status_bar.h"

#include "gui_text.h"

#include "imgui.h"

namespace gui_status {

void show(Bar& bar, std::string message) {
    bar.message = std::move(message);
    bar.error.clear();
    bar.shown_at = ImGui::GetTime();
}

void show_error(Bar& bar, std::string error) {
    bar.error = std::move(error);
    bar.message.clear();
    bar.shown_at = ImGui::GetTime();
}

void clear(Bar& bar) {
    bar.message.clear();
    bar.error.clear();
    bar.shown_at = 0.0;
}

void draw(Bar& bar, float timeout_seconds) {
    ImGui::Separator();

    const double now = ImGui::GetTime();
    if (bar.shown_at != 0.0 && now - bar.shown_at >= timeout_seconds) {
        clear(bar);
    }

    if (!bar.error.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.4F, 1.0F), "%s", bar.error.c_str());
    } else if (!bar.message.empty()) {
        ImGui::TextColored(ImVec4(0.30F, 0.85F, 0.35F, 1.0F), "%s",
                           bar.message.c_str());
    } else {
        ImGui::TextUnformatted(gui_text::status_ready);
        return;
    }

    // A click on the message reverts it to "就绪".
    if (ImGui::IsItemClicked()) {
        clear(bar);
    }
}

} // namespace gui_status

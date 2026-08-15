#include "gui_analyse_details_dialog.h"

#include "core/server_analyse_items.h"
#include "gui_text.h"

#include "imgui.h"

#include <string>

namespace gui_view {

void draw_analyse_details_window(ScanPanelState& state,
                                 const std::vector<LanWorldRecord>& worlds,
                                 float dpi_scale) {
    if (!state.analyse_details_open) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420.0F * dpi_scale, 0.0F),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(gui_text::analyse_details_title, &state.analyse_details_open)) {
        ImGui::End();
        return;
    }

    // Find the world matching the requested endpoint.
    const LanWorldRecord* record = nullptr;
    for (const auto& w : worlds) {
        if (w.world.ip == state.analyse_ip && w.world.port == state.analyse_port) {
            record = &w;
            break;
        }
    }
    if (record == nullptr) {
        ImGui::TextUnformatted("找不到对应的世界。");
        ImGui::End();
        return;
    }

    const ServerAnalyseItems items = compute_server_analyse_items(*record);

    // Show the LAN MOTD of the world being inspected.
    ImGui::TextUnformatted("Lan MOTD: ");
    ImGui::SameLine();
    ImGui::TextUnformatted(record->world.lan_motd.c_str());

    // Draws a "名称 (id)" label with the Chinese name in the default (white)
    // colour and the identifier part in grey, on one line.
    const auto draw_label = [](const char* label, bool highlight) {
        std::string text(label);
        const std::size_t open = text.find(" (sa_");
        if (open == std::string::npos) {
            if (highlight) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30F, 0.85F, 0.50F, 1.0F));
            }
            ImGui::TextUnformatted(label);
            if (highlight) {
                ImGui::PopStyleColor();
            }
            return;
        }
        if (highlight) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30F, 0.85F, 0.50F, 1.0F));
        }
        ImGui::TextUnformatted(text.substr(0, open).c_str());
        if (highlight) {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55F, 0.55F, 0.60F, 1.0F));
        ImGui::TextUnformatted(text.substr(open).c_str());
        ImGui::PopStyleColor();
    };

    const auto draw_field = [dpi_scale, &draw_label](const char* label, const char* value,
                                        bool highlight = false,
                                        bool bg_highlight = false) {
        ImGui::TableNextRow();
        if (bg_highlight) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImVec4(0.15F, 0.45F, 0.20F, 0.50F)));
        }
        ImGui::TableNextColumn();
        draw_label(label, highlight);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value);
    };

    const auto draw_bool = [&](const char* label, bool value, bool bg = false) {
        ImGui::TableNextRow();
        if (bg) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImVec4(0.15F, 0.45F, 0.20F, 0.50F)));
        }
        ImGui::TableNextColumn();
        draw_label(label, false);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text,
            value ? ImVec4(0.30F, 0.85F, 0.35F, 1.0F)
                  : ImVec4(0.95F, 0.30F, 0.30F, 1.0F));
        ImGui::TextUnformatted(value ? gui_text::bool_yes_icon : gui_text::bool_no_icon);
        ImGui::PopStyleColor();
    };

    const auto draw_short = [&](const char* label, short value) {
        const std::string text = std::to_string(value);
        draw_field(label, text.c_str(), false, false);
    };

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("analyse_fields", 2, table_flags)) {
        ImGui::TableSetupColumn("##field", ImGuiTableColumnFlags_WidthFixed,
                                310.0F * dpi_scale);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        // ---- Basic items ----
        draw_bool("可用 (sa_is_available)", items.sa_is_available);
        draw_bool("原版服务端 (sa_is_vanilla_version)", items.sa_is_vanilla_version);
        draw_bool("干净的Lan MOTD (sa_is_lan_motd_clean)", items.sa_is_lan_motd_clean);
        draw_bool("干净的MOTD (sa_is_motd_clean)", items.sa_is_motd_clean);
        draw_bool("Lan广播正常MOTD (sa_is_regular_lan_motd)", items.sa_is_regular_lan_motd);
        draw_field("服务端版本 (sa_str_version)", items.sa_str_version.c_str());
        draw_field("服务端类型 (sa_str_server_type)", items.sa_str_server_type.c_str());
        draw_short("真实在线数 (sa_int_actual_online)", items.sa_int_actual_online);
        draw_field("真实游戏版本 (sa_str_actual_server_version)",
                   items.sa_str_actual_server_version.c_str());
        draw_bool("默认端口号 (sa_is_default_port)", items.sa_is_default_port);
        draw_bool("正确在线数 (sa_is_correct_online)", items.sa_is_correct_online);
        draw_bool("正确协议版本 (sa_is_correct_protocol)", items.sa_is_correct_protocol);
        draw_short("Lan MOTD颜色字符数 (sa_int_lan_motd_color_char)",
                   items.sa_int_lan_motd_color_char);
        draw_bool("MOTD相同 (sa_is_motds_equal)", items.sa_is_motds_equal);

        // ---- Advanced item (light-green background) ----
        draw_bool("局域网世界 (sa_is_lan_world)", items.sa_is_lan_world, true);

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace gui_view

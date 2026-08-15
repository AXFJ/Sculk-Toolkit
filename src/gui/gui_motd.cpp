#include "gui_motd.h"

#include "gui_fonts.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <vector>

namespace {

struct Style {
    ImVec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    bool has_color = false;
    bool bold = false;
    bool underline = false;
    bool strikethrough = false;
};

struct Segment {
    std::string text;
    Style style;
};

// The 16 legacy Minecraft colors, in code order 0-9 then a-f.
constexpr std::array<unsigned int, 16> legacy_colors{
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xFFAA00, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

ImVec4 to_color(unsigned int rgb) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0F,
                  static_cast<float>((rgb >> 8) & 0xFF) / 255.0F,
                  static_cast<float>(rgb & 0xFF) / 255.0F, 1.0F);
}

int color_index(char code) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(code)));
    if (lower >= '0' && lower <= '9') return lower - '0';
    if (lower >= 'a' && lower <= 'f') return lower - 'a' + 10;
    return -1;
}

int hexadecimal_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool is_section(const std::string& text, std::size_t index) {
    return static_cast<unsigned char>(text[index]) == 0xC2 && index + 2 < text.size() &&
           static_cast<unsigned char>(text[index + 1]) == 0xA7;
}

// Splits the text into styled segments, one vector per line.
std::vector<std::vector<Segment>> parse(const std::string& text, gui_motd::Mode mode) {
    std::vector<std::vector<Segment>> lines{{}};
    Segment current;
    const auto flush = [&] {
        if (!current.text.empty()) {
            lines.back().push_back(current);
            current.text.clear();
        }
    };

    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        if (mode != gui_motd::Mode::Raw && is_section(text, index)) {
            const char code = text[index + 2];
            if (mode == gui_motd::Mode::Clean) {
                index += 2;
                continue;
            }
            flush();
            // Bungee hex colors arrive as §x§R§R§G§G§B§B.
            if ((code == 'x' || code == 'X') && index + 20 < text.size()) {
                int rgb = 0;
                bool valid = true;
                std::size_t position = index + 3;
                for (int digit = 0; digit < 6; ++digit, position += 3) {
                    if (position + 2 >= text.size() || !is_section(text, position)) {
                        valid = false;
                        break;
                    }
                    const int value = hexadecimal_value(text[position + 2]);
                    if (value < 0) {
                        valid = false;
                        break;
                    }
                    rgb = (rgb << 4) | value;
                }
                if (valid) {
                    current.style = Style{};
                    current.style.color = to_color(static_cast<unsigned int>(rgb));
                    current.style.has_color = true;
                    index += 20;
                    continue;
                }
            }
            const int color = color_index(code);
            const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(code)));
            if (color >= 0) {
                current.style = Style{};
                current.style.color = to_color(legacy_colors[static_cast<std::size_t>(color)]);
                current.style.has_color = true;
            } else if (lower == 'l') {
                current.style.bold = true;
            } else if (lower == 'n') {
                current.style.underline = true;
            } else if (lower == 'm') {
                current.style.strikethrough = true;
            } else if (lower == 'r') {
                current.style = Style{};
            }
            // Italic (o) and obfuscated (k) have no font to switch to.
            index += 2;
            continue;
        }

        if (character == '\n') {
            flush();
            lines.emplace_back();
            continue;
        }
        if (character == '\t') {
            current.text.push_back(' ');
            continue;
        }
        if (character >= 0x20 || character >= 0x80) {
            current.text.push_back(static_cast<char>(character));
        }
    }
    flush();
    return lines;
}

void draw_segment(const Segment& segment, bool monochrome) {
    ImFont* bold = segment.style.bold && !monochrome ? gui_fonts::bold() : nullptr;
    if (bold != nullptr) {
        ImGui::PushFont(bold, 0.0F);
    }
    const bool colored = segment.style.has_color && !monochrome;
    if (colored) {
        ImGui::PushStyleColor(ImGuiCol_Text, segment.style.color);
    }
    ImGui::TextUnformatted(segment.text.c_str());
    if (colored) {
        ImGui::PopStyleColor();
    }
    if (bold != nullptr) {
        ImGui::PopFont();
    }

    // ImGui has no underline or strikethrough, so draw them over the text.
    if (segment.style.underline || segment.style.strikethrough) {
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const ImU32 color = ImGui::GetColorU32(colored
            ? segment.style.color : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        if (segment.style.underline) {
            draw_list->AddLine(ImVec2(minimum.x, maximum.y - 1.0F),
                               ImVec2(maximum.x, maximum.y - 1.0F), color);
        }
        if (segment.style.strikethrough) {
            const float middle = (minimum.y + maximum.y) * 0.5F;
            draw_list->AddLine(ImVec2(minimum.x, middle), ImVec2(maximum.x, middle), color);
        }
    }
}

std::size_t sequence_length(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xE0U) == 0xC0U) return 2;
    if ((lead & 0xF0U) == 0xE0U) return 3;
    if ((lead & 0xF8U) == 0xF0U) return 4;
    return 1;
}

// Shortens a segment so the whole line fits, leaving room for the dots.
std::string clip_to_width(const std::string& text, float available, float reserved) {
    std::string clipped;
    float used = 0.0F;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t length = std::min(sequence_length(
            static_cast<unsigned char>(text[index])), text.size() - index);
        const std::string glyph = text.substr(index, length);
        const float glyph_width = ImGui::CalcTextSize(glyph.c_str()).x;
        if (used + glyph_width + reserved > available) {
            break;
        }
        clipped += glyph;
        used += glyph_width;
        index += length;
    }
    return clipped;
}

// Draws one line of segments, clipping the tail to the available width.
void draw_segments(const std::vector<Segment>& segments, bool monochrome) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float dots = ImGui::CalcTextSize("...").x;
    float used = 0.0F;
    bool first = true;
    bool clipped = false;

    for (const Segment& segment : segments) {
        const float width = ImGui::CalcTextSize(segment.text.c_str()).x;
        if (!first) {
            ImGui::SameLine(0.0F, 0.0F);
        }
        if (used + width <= available) {
            draw_segment(segment, monochrome);
            used += width;
            first = false;
            continue;
        }

        Segment tail = segment;
        tail.text = clip_to_width(segment.text, available - used, dots);
        if (!tail.text.empty()) {
            draw_segment(tail, monochrome);
            first = false;
        } else if (first) {
            // Nothing fits: still emit an item so the row keeps its height.
            ImGui::TextUnformatted("");
            first = false;
        }
        clipped = true;
        break;
    }

    if (clipped) {
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::TextUnformatted("...");
    } else if (first) {
        ImGui::TextUnformatted("");
    }
}

// Draws one line of segments without clipping, letting ImGui wrap long runs at
// the edge of the current region.
void draw_segments_wrapped(const std::vector<Segment>& segments, bool monochrome) {
    if (segments.empty()) {
        ImGui::TextUnformatted("");
        return;
    }
    ImGui::PushTextWrapPos(0.0F);
    bool first = true;
    for (const Segment& segment : segments) {
        if (!first) {
            ImGui::SameLine(0.0F, 0.0F);
        }
        draw_segment(segment, monochrome);
        first = false;
    }
    ImGui::PopTextWrapPos();
}

} // namespace

namespace gui_motd {

std::string flatten(const std::string& text, Mode mode) {
    const std::vector<std::vector<Segment>> lines = parse(text, mode);
    std::string output;
    for (const std::vector<Segment>& line : lines) {
        if (!output.empty()) {
            output += " / ";
        }
        for (const Segment& segment : line) {
            output += segment.text;
        }
    }
    return output;
}

void draw_line(const std::string& text, Mode mode, bool monochrome) {
    const std::vector<std::vector<Segment>> lines = parse(text, mode);
    std::vector<Segment> single;
    for (const std::vector<Segment>& line : lines) {
        if (!single.empty()) {
            single.push_back(Segment{" / ", Style{}});
        }
        single.insert(single.end(), line.begin(), line.end());
    }
    draw_segments(single, monochrome);
}

void draw_multiline(const std::string& text, Mode mode, bool monochrome) {
    const std::vector<std::vector<Segment>> lines = parse(text, mode);
    for (const std::vector<Segment>& line : lines) {
        draw_segments_wrapped(line, monochrome);
    }
}

} // namespace gui_motd

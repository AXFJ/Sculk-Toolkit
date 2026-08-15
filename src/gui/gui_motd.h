#pragma once

#include <string>

// Minecraft MOTDs carry U+00A7 formatting codes. The GUI renders them in one of
// three global modes, so parsing and drawing live in one place.
namespace gui_motd {

enum class Mode {
    Clean,   // strip formatting codes
    Format,  // render colors and styles
    Raw      // show the text exactly as received
};

// Single-line form for table cells and logs; newlines become " / ".
std::string flatten(const std::string& text, Mode mode);

// Draws one line of text at the cursor, clipped to the available width and
// ending in "..." when it does not fit. Colors and styles are applied in
// Format mode. With monochrome set, colors and the bold face are suppressed so
// the caller's pushed text color and font win — used for dimmed rows.
void draw_line(const std::string& text, Mode mode, bool monochrome = false);

// Draws every line of the text, so a two-line MOTD occupies two rows.
void draw_multiline(const std::string& text, Mode mode, bool monochrome = false);

} // namespace gui_motd

#pragma once

struct ImFont;

// Resolves installed desktop fonts and registers them with the active Dear ImGui
// context. Kept separate from the host so window/D3D code stays free of font and
// registry details.
namespace gui_fonts {

// Loads Cascadia Mono (falling back to Cascadia Code, then the built-in font)
// and merges a Chinese face so MOTD glyphs render. Rasterizes at the monitor
// scale so text stays sharp on scaled displays.
void load(float dpi_scale);

// Bold companion face used for field labels, or nullptr when none was found.
ImFont* bold();

} // namespace gui_fonts

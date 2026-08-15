#pragma once

#include <string>

// Bottom status line shared by the broadcast and database pages: a separator
// above a one-line message that reverts to "就绪" after a timeout or a click.
namespace gui_status {

struct Bar {
    std::string message;
    std::string error;
    // ImGui::GetTime() when the message was last set; 0 when never set.
    double shown_at = 0.0;
};

// Sets a green status message (clears any pending error).
void show(Bar& bar, std::string message);

// Sets a red error message (clears any pending status).
void show_error(Bar& bar, std::string error);

// Clears the message back to "就绪".
void clear(Bar& bar);

// Draws the separator plus the status line. A message older than
// timeout_seconds, or one that was clicked, reverts to "就绪".
void draw(Bar& bar, float timeout_seconds = 30.0F);

} // namespace gui_status

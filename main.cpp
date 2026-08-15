#include "gui_app.h"
#include "terminal_app.h"

int main(int argc, char**) {
    return argc == 1 ? run_gui() : run_terminal();
}

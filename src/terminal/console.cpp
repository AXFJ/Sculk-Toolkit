#include "console.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <cstdint>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

bool colors_enabled = true;

void replace_all(std::string& text, std::string_view token, std::string_view replacement) {
    std::size_t position = 0;
    while ((position = text.find(token, position)) != std::string::npos) {
        text.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}

#ifdef _WIN32

struct StandardStream {
    DWORD identifier;
    int descriptor;
    int open_mode;
    const wchar_t* console_name;
    HANDLE inherited = nullptr;
    bool inherited_valid = false;
    bool inherited_console = false;
};

bool handle_is_valid(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(handle);
    return type != FILE_TYPE_UNKNOWN || GetLastError() == ERROR_SUCCESS;
}

bool handle_is_console(HANDLE handle) {
    DWORD mode = 0;
    return handle_is_valid(handle) && GetConsoleMode(handle, &mode) != FALSE;
}

bool bind_descriptor(const StandardStream& stream, HANDLE handle) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return false;
    }

    const int temporary = _open_osfhandle(
        reinterpret_cast<std::intptr_t>(duplicate), stream.open_mode | _O_TEXT);
    if (temporary == -1) {
        CloseHandle(duplicate);
        return false;
    }

    const bool success = _dup2(temporary, stream.descriptor) == 0;
    _close(temporary);
    if (!success) {
        return false;
    }

    return _setmode(stream.descriptor, _O_TEXT) != -1;
}

bool reopen_console_stream(const StandardStream& stream) {
    const wchar_t* mode = stream.open_mode == _O_RDONLY ? L"r" : L"w";
    FILE* reopened = nullptr;
    if (_wfreopen_s(&reopened, stream.console_name, mode,
                    stream.descriptor == 0 ? stdin
                    : stream.descriptor == 1 ? stdout
                                             : stderr) != 0 || reopened == nullptr) {
        return false;
    }

    const int descriptor = _fileno(reopened);
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    return descriptor != -1 && handle_is_valid(handle) &&
           SetStdHandle(stream.identifier, handle) != FALSE &&
           _setmode(descriptor, _O_TEXT) != -1;
}

bool initialize_windows_console() {
    std::array<StandardStream, 3> streams{{
        {STD_INPUT_HANDLE, 0, _O_RDONLY, L"CONIN$"},
        {STD_OUTPUT_HANDLE, 1, _O_WRONLY, L"CONOUT$"},
        {STD_ERROR_HANDLE, 2, _O_WRONLY, L"CONOUT$"},
    }};

    bool complete_redirection = true;
    for (auto& stream : streams) {
        stream.inherited = GetStdHandle(stream.identifier);
        stream.inherited_valid = handle_is_valid(stream.inherited);
        stream.inherited_console = handle_is_console(stream.inherited);
        complete_redirection = complete_redirection &&
                               stream.inherited_valid && !stream.inherited_console;
    }

    const bool attach_parent = std::getenv("SCULK_CLI_ATTACH_PARENT") != nullptr;
    bool attached = false;
    bool allocated = false;
    if (attach_parent) {
        attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        if (!attached && GetLastError() == ERROR_ACCESS_DENIED) {
            attached = true;
        }
    } else {
        allocated = AllocConsole() != FALSE;
        attached = allocated;
    }

    if (!attached && !complete_redirection) {
        return false;
    }

    std::fflush(nullptr);
    for (auto& stream : streams) {
        if (allocated) {
            if (!reopen_console_stream(stream)) {
                return false;
            }
            continue;
        }

        HANDLE selected = nullptr;
        if (stream.inherited_valid) {
            selected = stream.inherited;
            if (!SetStdHandle(stream.identifier, selected)) {
                return false;
            }
        } else {
            selected = allocated
                ? CreateFileW(stream.console_name,
                              stream.open_mode == _O_RDONLY ? GENERIC_READ : GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr)
                : GetStdHandle(stream.identifier);
            if (!handle_is_valid(selected)) {
                if (!attached) {
                    return false;
                }
                selected = CreateFileW(stream.console_name,
                                       stream.open_mode == _O_RDONLY
                                           ? GENERIC_READ
                                           : GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
                if (!handle_is_valid(selected)) {
                    return false;
                }
            }
            if (!SetStdHandle(stream.identifier, selected)) {
                if (allocated && handle_is_valid(selected)) {
                    CloseHandle(selected);
                }
                return false;
            }
        }

        if (!bind_descriptor(stream, selected)) {
            return false;
        }
    }

    clearerr(stdin);
    clearerr(stdout);
    clearerr(stderr);
    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    std::clog.clear();
    std::wcin.clear();
    std::wcout.clear();
    std::wcerr.clear();
    std::wclog.clear();

    if (attached) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }

    const HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD mode = 0;
    const bool virtual_terminal_enabled =
        handle_is_valid(output) &&
        GetConsoleMode(output, &mode) &&
        SetConsoleMode(output, mode | ENABLE_PROCESSED_OUTPUT |
                                    ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (handle_is_valid(output)) {
        CloseHandle(output);
    }
    colors_enabled = colors_enabled && virtual_terminal_enabled;
    return true;
}

#endif

} // namespace

namespace console {

bool initialize() {
    colors_enabled = std::getenv("NO_COLOR") == nullptr;

#ifdef _WIN32
    return initialize_windows_console();
#else
    return true;
#endif
}

std::string colorize(std::string text) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> colors{{
        {"{COLOR_RESET}", "\x1b[0m"},
        {"{COLOR_WHITE}", "\x1b[97m"},
        {"{COLOR_GRAY}", "\x1b[90m"},
        {"{COLOR_GREEN}", "\x1b[92m"},
        {"{COLOR_DARK_GREEN}", "\x1b[32m"},
        {"{COLOR_BLUE}", "\x1b[94m"},
        {"{COLOR_MAGENTA}", "\x1b[95m"},
        {"{COLOR_LIGHT_YELLOW}", "\x1b[93m"},
        {"{COLOR_RED}", "\x1b[91m"},
        {"{COLOR_YELLOW}", "\x1b[93m"},
    }};

    for (const auto& [token, ansi] : colors) {
        replace_all(text, token, colors_enabled ? ansi : std::string_view{});
    }
    return text;
}

} // namespace console

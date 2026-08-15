#include "gui_fonts.h"

#include "imgui.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

ImFont* bold_font = nullptr;

std::wstring environment_path(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return {};
    }
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    value.resize(written);
    return value;
}

bool contains_ignoring_case(const std::wstring& text, const std::wstring& needle) {
    const auto lower = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t character) {
                           return static_cast<wchar_t>(
                               std::towlower(static_cast<std::wint_t>(character)));
                       });
        return value;
    };
    return lower(text).find(lower(needle)) != std::wstring::npos;
}

// Font entries store either a bare file name resolved against the Windows font
// directory or a full path (per-user installations).
std::wstring resolve_font_file(const std::wstring& entry) {
    if (entry.find(L':') != std::wstring::npos) {
        return entry;
    }
    const std::wstring windows_directory = environment_path(L"WINDIR");
    if (windows_directory.empty()) {
        return {};
    }
    return windows_directory + L"\\Fonts\\" + entry;
}

std::wstring registry_font_path(HKEY root, const std::wstring& family) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }

    std::wstring found;
    for (DWORD index = 0; found.empty(); ++index) {
        wchar_t name[256]{};
        wchar_t data[MAX_PATH]{};
        DWORD name_length = static_cast<DWORD>(std::size(name));
        DWORD data_size = sizeof(data);
        DWORD type = 0;
        const LSTATUS status = RegEnumValueW(key, index, name, &name_length, nullptr,
                                            &type, reinterpret_cast<LPBYTE>(data), &data_size);
        if (status != ERROR_SUCCESS) {
            break;
        }
        if (type != REG_SZ || !contains_ignoring_case(name, family)) {
            continue;
        }
        const std::wstring path = resolve_font_file(data);
        std::error_code code;
        if (!path.empty() && std::filesystem::exists(path, code)) {
            found = path;
        }
    }

    RegCloseKey(key);
    return found;
}

std::wstring installed_font_path(const std::wstring& family) {
    const std::wstring user = registry_font_path(HKEY_CURRENT_USER, family);
    return user.empty() ? registry_font_path(HKEY_LOCAL_MACHINE, family) : user;
}

// Cascadia Mono is preferred; Cascadia Code shares its metrics and is the
// variant Windows Terminal installs, so it is an acceptable substitute.
std::wstring monospace_font_path() {
    for (const wchar_t* family : {L"Cascadia Mono", L"Cascadia Code"}) {
        const std::wstring path = installed_font_path(family);
        if (!path.empty()) {
            return path;
        }
    }
    return {};
}

// Bold monospace face for labels; Consolas Bold ships with Windows.
std::wstring bold_monospace_font_path() {
    for (const wchar_t* family : {L"Cascadia Mono SemiBold", L"Cascadia Mono Bold",
                                  L"Cascadia Code Bold", L"Consolas Bold"}) {
        const std::wstring path = installed_font_path(family);
        if (!path.empty()) {
            return path;
        }
    }
    const std::wstring windows_directory = environment_path(L"WINDIR");
    if (!windows_directory.empty()) {
        const std::wstring fallback = windows_directory + L"\\Fonts\\consolab.ttf";
        std::error_code code;
        if (std::filesystem::exists(fallback, code)) {
            return fallback;
        }
    }
    return {};
}

std::string narrow_path(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow.data(), length, nullptr, nullptr);
    return narrow;
}

} // namespace

namespace gui_fonts {

void load(float dpi_scale) {
    ImGuiIO& io = ImGui::GetIO();
    constexpr float font_size = 18.0F;

    const std::string cascadia = narrow_path(monospace_font_path());
    const bool loaded = !cascadia.empty() &&
                        io.Fonts->AddFontFromFileTTF(cascadia.c_str(), font_size) != nullptr;
    if (!loaded) {
        io.Fonts->AddFontDefault();
    }

    // Cascadia has no CJK coverage, so merge a system Chinese face for MOTDs.
    // The arrows, triangles, and Dingbats symbols used by the UI live outside
    // the Chinese ranges, so they are requested explicitly.
    static ImVector<ImWchar> chinese_ranges;
    if (chinese_ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        for (ImWchar code = 0x2190; code <= 0x21FF; ++code) {
            builder.AddChar(code);
        }
        for (ImWchar code = 0x25A0; code <= 0x25FF; ++code) {
            builder.AddChar(code);
        }
        for (ImWchar code = 0x2700; code <= 0x27BF; ++code) {
            builder.AddChar(code);
        }
        builder.BuildRanges(&chinese_ranges);
    }
    const std::string chinese = narrow_path(installed_font_path(L"Microsoft YaHei"));
    if (!chinese.empty()) {
        ImFontConfig config;
        config.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(chinese.c_str(), font_size, &config,
                                     chinese_ranges.Data);
    }

    // Segoe UI Symbol covers symbol blocks (⚙ ✗ ✓ etc.) that Microsoft YaHei
    // lacks.  Merge it on top so those glyphs render correctly.
    const std::string symbol_font = narrow_path(
        installed_font_path(L"Segoe UI Symbol"));
    if (!symbol_font.empty()) {
        ImFontConfig symbol_config;
        symbol_config.MergeMode = true;
        static ImVector<ImWchar> symbol_ranges;
        if (symbol_ranges.empty()) {
            ImFontGlyphRangesBuilder builder;
            for (ImWchar code = 0x2600; code <= 0x26FF; ++code) {
                builder.AddChar(code);  // Miscellaneous Symbols (⚙…)
            }
            for (ImWchar code = 0x2700; code <= 0x27BF; ++code) {
                builder.AddChar(code);  // Dingbats (✓✗…)
            }
            builder.BuildRanges(&symbol_ranges);
        }
        io.Fonts->AddFontFromFileTTF(symbol_font.c_str(), font_size, &symbol_config,
                                     symbol_ranges.Data);
    }

    const std::string bold_latin = narrow_path(bold_monospace_font_path());
    if (!bold_latin.empty()) {
        bold_font = io.Fonts->AddFontFromFileTTF(bold_latin.c_str(), font_size);
        const std::string bold_chinese = narrow_path(
            installed_font_path(L"Microsoft YaHei Bold"));
        if (bold_font != nullptr && !bold_chinese.empty()) {
            ImFontConfig config;
            config.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(bold_chinese.c_str(), font_size, &config,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
    }

    ImGui::GetStyle().FontScaleDpi = dpi_scale;
}

ImFont* bold() {
    return bold_font;
}

} // namespace gui_fonts

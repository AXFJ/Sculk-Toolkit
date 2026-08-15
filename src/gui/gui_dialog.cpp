#include "gui_dialog.h"

#include <windows.h>
#include <commdlg.h>

#include <array>
#include <cwchar>

namespace gui_dialog {

std::wstring open_png_file() {
    std::array<wchar_t, MAX_PATH> path{};
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.lpstrFilter = L"PNG \u56fe\u7247 (*.png)\0*.png\0\u6240\u6709\u6587\u4ef6\0*.*\0";
    open.lpstrFile = path.data();
    open.nMaxFile = static_cast<DWORD>(path.size());
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&open) == FALSE) {
        return {};
    }
    return path.data();
}

std::wstring open_json_file() {
    std::array<wchar_t, MAX_PATH> path{};
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.lpstrFilter = L"JSON (*.json)\0*.json\0所有文件\0*.*\0";
    open.lpstrFile = path.data();
    open.nMaxFile = static_cast<DWORD>(path.size());
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&open) == FALSE) {
        return {};
    }
    return path.data();
}

std::wstring save_json_file() {
    return save_json_file(L"fakeserver.json");
}

std::wstring save_json_file(const std::wstring& default_name) {
    std::array<wchar_t, MAX_PATH> path{};
    wcscpy_s(path.data(), path.size(), default_name.c_str());
    OPENFILENAMEW save{};
    save.lStructSize = sizeof(save);
    save.lpstrFilter = L"JSON (*.json)\0*.json\0";
    save.lpstrDefExt = L"json";
    save.lpstrFile = path.data();
    save.nMaxFile = static_cast<DWORD>(path.size());
    save.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&save) == FALSE) {
        return {};
    }
    return path.data();
}

std::string to_utf8(const std::wstring& path) {
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

std::wstring from_utf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring wide(static_cast<std::size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), length);
    return wide;
}

} // namespace gui_dialog

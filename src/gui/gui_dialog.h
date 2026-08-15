#pragma once

#include <string>

// Win32 common-dialog wrappers used by the GUI forms.
namespace gui_dialog {

// Shows the Open File dialog for PNG images. Returns an empty string when the
// user cancels. The path stays wide so files with non-ASCII names open reliably.
std::wstring open_png_file();

// Open File dialog for broadcast preset files.
std::wstring open_json_file();

// Save File dialog for broadcast preset files; the caller picks folder and name.
std::wstring save_json_file();
std::wstring save_json_file(const std::wstring& default_name);

// UTF-8 form of a path, for display in the form.
std::string to_utf8(const std::wstring& path);
// Reverse: wide string from a UTF-8 encoded string.
std::wstring from_utf8(const std::string& utf8);

} // namespace gui_dialog

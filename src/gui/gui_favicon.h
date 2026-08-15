#pragma once

#include <d3d11.h>

#include <string>

// Decodes the base64 PNG a server reports as its favicon into a D3D11 texture
// for the details pane.
namespace gui_favicon {

struct Texture {
    ID3D11ShaderResourceView* view = nullptr;
    int width = 0;
    int height = 0;
    // Favicon the texture was built from, so callers can skip redundant decodes.
    std::string source;

    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    ~Texture();

    void release();
};

// Replaces texture with a decode of favicon when the source differs. A blank or
// undecodable favicon leaves the texture empty.
void update(Texture& texture, const std::string& favicon, ID3D11Device* device);

// Reads a 64x64 PNG through the wide Win32 API — so non-ASCII paths work — and
// returns it as a "data:image/png;base64,..." string. Returns an empty string
// and fills error when the file is unusable.
std::string encode_png_data_uri(const std::wstring& path, std::string& error);

} // namespace gui_favicon

#include "gui_favicon.h"

#include "gui_text.h"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

template <typename Interface>
void release_com(Interface*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

bool decode_base64(std::string_view encoded, std::vector<std::uint8_t>& output) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> values{};
    values.fill(-1);
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }

    int accumulator = 0;
    int bits = -8;
    output.clear();
    for (const unsigned char character : encoded) {
        if (std::isspace(character)) {
            continue;
        }
        if (character == '=') {
            break;
        }
        const int value = values[character];
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return !output.empty();
}

// Favicons arrive as "data:image/png;base64,<payload>".
std::string_view favicon_payload(const std::string& favicon) {
    const std::size_t comma = favicon.find(',');
    return comma == std::string::npos
        ? std::string_view{favicon}
        : std::string_view{favicon}.substr(comma + 1);
}

std::string encode_base64(const std::vector<std::uint8_t>& data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < data.size(); index += 3) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(data[index]) << 16) |
            (index + 1 < data.size() ? static_cast<std::uint32_t>(data[index + 1]) << 8 : 0) |
            (index + 2 < data.size() ? static_cast<std::uint32_t>(data[index + 2]) : 0);
        output.push_back(alphabet[(value >> 18) & 0x3F]);
        output.push_back(alphabet[(value >> 12) & 0x3F]);
        output.push_back(index + 1 < data.size() ? alphabet[(value >> 6) & 0x3F] : '=');
        output.push_back(index + 2 < data.size() ? alphabet[value & 0x3F] : '=');
    }
    return output;
}

bool decode_png(const std::vector<std::uint8_t>& png,
                std::vector<std::uint8_t>& pixels,
                UINT& width,
                UINT& height) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized) || initialized == S_FALSE;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool success = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),
                                               static_cast<DWORD>(png.size()))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr,
                                                   WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0) {
        pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
        success = SUCCEEDED(converter->CopyPixels(
            nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    }

    release_com(converter);
    release_com(frame);
    release_com(decoder);
    release_com(stream);
    release_com(factory);
    if (uninitialize) {
        CoUninitialize();
    }
    return success;
}

} // namespace

namespace gui_favicon {

void Texture::release() {
    release_com(view);
    width = 0;
    height = 0;
    source.clear();
}

Texture::~Texture() {
    release();
}

void update(Texture& texture, const std::string& favicon, ID3D11Device* device) {
    if (texture.source == favicon) {
        return;
    }

    texture.release();
    texture.source = favicon;
    if (favicon.empty() || device == nullptr) {
        return;
    }

    std::vector<std::uint8_t> png;
    std::vector<std::uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    if (!decode_base64(favicon_payload(favicon), png) ||
        !decode_png(png, pixels, width, height)) {
        return;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = width * 4;

    ID3D11Texture2D* resource = nullptr;
    if (FAILED(device->CreateTexture2D(&description, &initial, &resource))) {
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
    view_description.Format = description.Format;
    view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_description.Texture2D.MipLevels = 1;
    if (SUCCEEDED(device->CreateShaderResourceView(resource, &view_description, &texture.view))) {
        texture.width = static_cast<int>(width);
        texture.height = static_cast<int>(height);
    }
    release_com(resource);
}

std::string encode_png_data_uri(const std::wstring& path, std::string& error) {
    error.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = gui_text::favicon_read_failed;
        return {};
    }

    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    DWORD read = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
           read > 0) {
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
        if (bytes.size() > 4 * 1024 * 1024) {
            break;
        }
    }
    CloseHandle(file);

    static constexpr std::array<std::uint8_t, 8> signature{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 24 ||
        !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        error = gui_text::favicon_not_png;
        return {};
    }
    const auto read_u32 = [&bytes](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
               static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    // Minecraft only accepts a 64x64 icon.
    if (read_u32(16) != 64 || read_u32(20) != 64) {
        error = gui_text::favicon_wrong_size;
        return {};
    }
    return "data:image/png;base64," + encode_base64(bytes);
}

} // namespace gui_favicon

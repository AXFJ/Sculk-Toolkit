#include "favicon.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
#include <wincodec.h>
#endif

namespace {

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

#ifdef _WIN32
template <typename Interface>
void release_com(Interface*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

std::uint8_t composite(std::uint8_t component, std::uint8_t alpha) {
    return static_cast<std::uint8_t>((static_cast<unsigned int>(component) * alpha) / 255);
}
#endif

} // namespace

std::string render_favicon(const std::string& data_uri, std::string& error) {
    constexpr std::string_view prefix = "data:image/png;base64,";
    if (!data_uri.starts_with(prefix)) {
        error = "favicon 不是受支持的 PNG Data URI";
        return {};
    }

    std::vector<std::uint8_t> png;
    if (!decode_base64(std::string_view(data_uri).substr(prefix.size()), png)) {
        error = "favicon Base64 数据无效";
        return {};
    }

#ifndef _WIN32
    error = "当前平台暂不支持在终端渲染 favicon";
    return {};
#else
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool should_uninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        error = "无法初始化 Windows 图像组件";
        return {};
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromMemory(png.data(), static_cast<DWORD>(png.size()));
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromStream(stream, nullptr,
                                                  WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);

    UINT source_width = 0;
    UINT source_height = 0;
    if (SUCCEEDED(result)) result = frame->GetSize(&source_width, &source_height);
    if (SUCCEEDED(result) && (source_width == 0 || source_height == 0)) result = E_FAIL;

    const UINT target_width = std::min<UINT>(32, source_width);
    const UINT target_height = std::max<UINT>(1, source_height * target_width / source_width);
    if (SUCCEEDED(result)) result = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(result)) {
        result = scaler->Initialize(frame, target_width, target_height,
                                    WICBitmapInterpolationModeFant);
    }
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) {
        result = converter->Initialize(scaler, GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom);
    }

    std::vector<std::uint8_t> pixels;
    if (SUCCEEDED(result)) {
        pixels.resize(static_cast<std::size_t>(target_width) * target_height * 4);
        result = converter->CopyPixels(nullptr, target_width * 4,
                                       static_cast<UINT>(pixels.size()), pixels.data());
    }

    release_com(converter);
    release_com(scaler);
    release_com(frame);
    release_com(decoder);
    release_com(stream);
    release_com(factory);
    if (should_uninitialize) CoUninitialize();

    if (FAILED(result)) {
        error = "无法解码服务器 favicon PNG";
        return {};
    }

    std::ostringstream output;
    for (UINT y = 0; y < target_height; y += 2) {
        output << "    ";
        for (UINT x = 0; x < target_width; ++x) {
            const std::size_t upper_index = (static_cast<std::size_t>(y) * target_width + x) * 4;
            const std::size_t lower_index = y + 1 < target_height
                ? (static_cast<std::size_t>(y + 1) * target_width + x) * 4
                : upper_index;
            const auto upper_alpha = pixels[upper_index + 3];
            const auto lower_alpha = y + 1 < target_height ? pixels[lower_index + 3] : 0;
            output << "\x1b[38;2;" << static_cast<int>(composite(pixels[upper_index], upper_alpha))
                   << ';' << static_cast<int>(composite(pixels[upper_index + 1], upper_alpha))
                   << ';' << static_cast<int>(composite(pixels[upper_index + 2], upper_alpha))
                   << "m\x1b[48;2;" << static_cast<int>(composite(pixels[lower_index], lower_alpha))
                   << ';' << static_cast<int>(composite(pixels[lower_index + 1], lower_alpha))
                   << ';' << static_cast<int>(composite(pixels[lower_index + 2], lower_alpha))
                   << "m▀";
        }
        output << "\x1b[0m\n";
    }
    return output.str();
#endif
}

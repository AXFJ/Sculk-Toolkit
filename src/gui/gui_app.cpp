#include "gui_app.h"

#include "gui_broadcast_view.h"
#include "gui_fonts.h"
#include "gui_scan_view.h"
#include "gui_shell.h"
#include "gui_text.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param);

namespace {

constexpr wchar_t window_class_name[] = L"SculkSensorGui";

template <typename T>
void release(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

std::wstring error_text(const wchar_t* stage, unsigned long code) {
    std::wostringstream stream;
    stream << stage << L" failed (0x" << std::uppercase << std::hex
           << std::setw(8) << std::setfill(L'0') << code << L").";
    return stream.str();
}

void report_error(const std::wstring& message) {
    std::wcerr << gui_text::error_prefix << message << L'\n';
    MessageBoxW(nullptr, message.c_str(), gui_text::error_caption, MB_OK | MB_ICONERROR);
}

struct GuiHost {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* device_context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    UINT resize_width = 0;
    UINT resize_height = 0;
    bool class_registered = false;
    bool running = true;
    bool minimized = false;
    bool imgui_context_created = false;
    bool win32_backend_initialized = false;
    bool dx11_backend_initialized = false;

    ~GuiHost() {
        cleanup();
    }

    void cleanup() {
        if (dx11_backend_initialized) {
            ImGui_ImplDX11_Shutdown();
            dx11_backend_initialized = false;
        }
        if (win32_backend_initialized) {
            ImGui_ImplWin32_Shutdown();
            win32_backend_initialized = false;
        }
        if (imgui_context_created) {
            ImGui::DestroyContext();
            imgui_context_created = false;
        }

        release(render_target);
        release(swap_chain);
        release(device_context);
        release(device);

        if (window != nullptr && IsWindow(window)) {
            DestroyWindow(window);
        }
        window = nullptr;

        if (class_registered) {
            UnregisterClassW(window_class_name, instance);
            class_registered = false;
        }
    }

    HRESULT create_render_target() {
        ID3D11Texture2D* back_buffer = nullptr;
        HRESULT result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
        if (SUCCEEDED(result)) {
            result = device->CreateRenderTargetView(back_buffer, nullptr, &render_target);
        }
        release(back_buffer);
        return result;
    }

    HRESULT create_device_and_swap_chain() {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        constexpr D3D_FEATURE_LEVEL feature_levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0,
        };

        D3D_FEATURE_LEVEL selected_feature_level{};
        HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            feature_levels,
            static_cast<UINT>(std::size(feature_levels)),
            D3D11_SDK_VERSION,
            &description,
            &swap_chain,
            &device,
            &selected_feature_level,
            &device_context);

        if (FAILED(result)) {
            release(swap_chain);
            release(device_context);
            release(device);
            result = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                0,
                feature_levels,
                static_cast<UINT>(std::size(feature_levels)),
                D3D11_SDK_VERSION,
                &description,
                &swap_chain,
                &device,
                &selected_feature_level,
                &device_context);
        }
        if (FAILED(result)) {
            return result;
        }

        return create_render_target();
    }

    HRESULT apply_pending_resize() {
        if (resize_width == 0 || resize_height == 0) {
            return S_OK;
        }

        device_context->OMSetRenderTargets(0, nullptr, nullptr);
        release(render_target);
        const HRESULT resize_result = swap_chain->ResizeBuffers(
            0, resize_width, resize_height, DXGI_FORMAT_UNKNOWN, 0);
        resize_width = 0;
        resize_height = 0;
        if (FAILED(resize_result)) {
            return resize_result;
        }
        return create_render_target();
    }
};

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    GuiHost* host = reinterpret_cast<GuiHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        host = static_cast<GuiHost*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        host->window = window;
    }

    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param)) {
        return 1;
    }

    switch (message) {
        case WM_SIZE:
            if (host != nullptr) {
                host->minimized = w_param == SIZE_MINIMIZED ||
                                  LOWORD(l_param) == 0 || HIWORD(l_param) == 0;
                if (!host->minimized) {
                    host->resize_width = LOWORD(l_param);
                    host->resize_height = HIWORD(l_param);
                }
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(l_param);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            if (host != nullptr) {
                host->running = false;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, w_param, l_param);
    }
}

bool initialize_window(GuiHost& host, std::wstring& error) {
    // Must run before the window exists so Windows never scales our pixels.
    ImGui_ImplWin32_EnableDpiAwareness();
    host.instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_CLASSDC;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = host.instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = window_class_name;
    if (RegisterClassExW(&window_class) == 0) {
        error = error_text(gui_text::stage_register_class, GetLastError());
        return false;
    }
    host.class_registered = true;

    RECT rectangle{0, 0, 1280, 720};
    constexpr DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rectangle, style, FALSE);
    host.window = CreateWindowExW(
        0,
        window_class_name,
        gui_text::window_title,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rectangle.right - rectangle.left,
        rectangle.bottom - rectangle.top,
        nullptr,
        nullptr,
        host.instance,
        &host);
    if (host.window == nullptr) {
        error = error_text(gui_text::stage_create_window, GetLastError());
        return false;
    }

    // The logical 1280x720 layout needs proportionally more physical pixels on
    // a scaled monitor.
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(host.window);
    if (scale > 1.0F) {
        SetWindowPos(host.window, nullptr, 0, 0,
                     static_cast<int>((rectangle.right - rectangle.left) * scale),
                     static_cast<int>((rectangle.bottom - rectangle.top) * scale),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return true;
}

} // namespace

int run_gui() {
    GuiHost host;
    LanMonitor monitor;
    ServerProbe probe;
    FakeServerManager fake_servers;
    gui_view::ScanPanelState panel_state;
    gui_view::BroadcastPanelState broadcast_state;
    gui_view::DatabasePanelState database_state;
    std::wstring error;
    if (!initialize_window(host, error)) {
        report_error(error);
        return EXIT_FAILURE;
    }

    HRESULT result = host.create_device_and_swap_chain();
    if (FAILED(result)) {
        report_error(error_text(gui_text::stage_device_and_swap_chain,
                                static_cast<unsigned long>(result)));
        return EXIT_FAILURE;
    }
    host.resize_width = 0;
    host.resize_height = 0;

    try {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        host.imgui_context_created = true;
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        // Dialogs must be fully opaque so form fields stay readable.
        ImGui::GetStyle().Colors[ImGuiCol_PopupBg].w = 1.0F;
        ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0F;
        gui_fonts::load(ImGui_ImplWin32_GetDpiScaleForHwnd(host.window));
    } catch (const std::exception& exception) {
        std::wstring message = gui_text::imgui_context_failure;
        const std::string detail = exception.what();
        message.append(detail.begin(), detail.end());
        report_error(message);
        return EXIT_FAILURE;
    } catch (...) {
        report_error(gui_text::imgui_context_unknown_failure);
        return EXIT_FAILURE;
    }

    if (!ImGui_ImplWin32_Init(host.window)) {
        report_error(gui_text::win32_backend_failure);
        return EXIT_FAILURE;
    }
    host.win32_backend_initialized = true;

    if (!ImGui_ImplDX11_Init(host.device, host.device_context)) {
        report_error(gui_text::dx11_backend_failure);
        return EXIT_FAILURE;
    }
    host.dx11_backend_initialized = true;

    ShowWindow(host.window, SW_SHOWDEFAULT);
    UpdateWindow(host.window);

    const ImGuiStyle base_style = ImGui::GetStyle();
    float applied_scale = 0.0F;

    while (host.running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                host.running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!host.running) {
            break;
        }

        if (host.minimized) {
            WaitMessage();
            continue;
        }

        result = host.apply_pending_resize();
        if (FAILED(result)) {
            report_error(error_text(gui_text::stage_swap_chain_resize,
                                    static_cast<unsigned long>(result)));
            return EXIT_FAILURE;
        }

        // Re-derive spacing and glyph scale whenever the window changes monitor.
        const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(host.window);
        if (scale != applied_scale) {
            ImGuiStyle& style = ImGui::GetStyle();
            style = base_style;
            style.ScaleAllSizes(scale);
            style.FontScaleDpi = scale;
            applied_scale = scale;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        gui_shell::Context shell;
        shell.monitor = &monitor;
        shell.probe = &probe;
        shell.servers = &fake_servers;
        shell.scan = &panel_state;
        shell.broadcast = &broadcast_state;
        shell.database = &database_state;
        shell.device = host.device;
        shell.dpi_scale = applied_scale;
        gui_shell::draw(shell);

        ImGui::Render();
        constexpr float clear_color[4] = {0.08F, 0.09F, 0.11F, 1.0F};
        host.device_context->OMSetRenderTargets(1, &host.render_target, nullptr);
        host.device_context->ClearRenderTargetView(host.render_target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Render any window that has been dragged outside the main viewport.
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        result = host.swap_chain->Present(1, 0);
        if (result == DXGI_STATUS_OCCLUDED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (FAILED(result)) {
            report_error(error_text(gui_text::stage_swap_chain_present,
                                    static_cast<unsigned long>(result)));
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

cmake_minimum_required(VERSION 3.20)
project(Sculk_Toolkit)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT WIN32)
    message(FATAL_ERROR "The GUI host currently requires Windows.")
endif()

add_library(sculk_imgui STATIC
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_win32.cpp
        third_party/imgui/backends/imgui_impl_dx11.cpp
)

target_include_directories(sculk_imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
)

target_compile_definitions(sculk_imgui PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
)

target_link_libraries(sculk_imgui PRIVATE
        d3d11
        d3dcompiler
        dwmapi
        gdi32
)

add_library(sculk_core STATIC
        src/core/database.cpp
        src/core/fake_server.cpp
        src/core/lan_listener.cpp
        src/core/lan_monitor.cpp
        src/core/scan.cpp
        src/core/server_probe.cpp
        src/core/status_query.cpp
)

target_include_directories(sculk_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(sculk_core PUBLIC
        ws2_32
)

add_library(sculk_gui_app STATIC
        src/gui/gui_app.cpp
        src/gui/gui_broadcast_form.cpp
        src/gui/gui_broadcast_view.cpp
        src/gui/gui_database_view.cpp
        src/gui/gui_dialog.cpp
        src/gui/dialog/gui_about_dialog.cpp
        src/gui/dialog/gui_analyse_details_dialog.cpp
        src/gui/dialog/gui_scan_columns_dialog.cpp
        src/gui/dialog/gui_scan_log_dialog.cpp
        src/gui/gui_favicon.cpp
        src/gui/gui_fonts.cpp
        src/gui/gui_motd.cpp
        src/gui/gui_scan_view.cpp
        src/gui/gui_shell.cpp
        src/gui/gui_status_bar.cpp
        src/gui/gui_text.cpp
)

target_include_directories(sculk_gui_app PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
)

target_compile_definitions(sculk_gui_app PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
)

target_link_libraries(sculk_gui_app PRIVATE
        sculk_imgui
        sculk_core
        d3d11
        d3dcompiler
        dxgi
        user32
        gdi32
        dwmapi
        advapi32
        ole32
        windowscodecs
        comdlg32
)

add_library(sculk_terminal_app STATIC
        src/terminal/terminal_app.cpp
        src/terminal/command_analyse.cpp
        src/terminal/console.cpp
        src/terminal/favicon.cpp
        src/terminal/render_world.cpp
        src/terminal/task_manager.cpp
)

target_include_directories(sculk_terminal_app
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/terminal
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(sculk_terminal_app PRIVATE
        sculk_core
        ws2_32
        ole32
        windowscodecs
)

add_executable(Sculk_Toolkit WIN32
        main.cpp
)

target_link_libraries(Sculk_Toolkit PRIVATE
        sculk_gui_app
        sculk_terminal_app
)

if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(Sculk_Toolkit PRIVATE
            $<$<CONFIG:Release>:-static>
            $<$<CONFIG:Release>:-static-libgcc>
            $<$<CONFIG:Release>:-static-libstdc++>
    )
endif()

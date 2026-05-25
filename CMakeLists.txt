cmake_minimum_required(VERSION 3.20)
project(Silencer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── ImGui path (set via -DIMGUI_DIR=... or edit here) ──────────
if(NOT DEFINED IMGUI_DIR)
    set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/imgui"
        CACHE PATH "Path to the Dear ImGui source directory")
endif()

# ── Sources ────────────────────────────────────────────────────
set(SOURCES
    main.cpp
    resources.rc          # app icon

    # Dear ImGui core
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp

    # Win32 + DX11 backend
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

add_executable(Silencer WIN32 ${SOURCES})

target_include_directories(Silencer PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(Silencer PRIVATE
    d3d11 dxgi comdlg32 shell32
)

# ── MSVC-specific flags ────────────────────────────────────────
if(MSVC)
    target_compile_options(Silencer PRIVATE /W3 /MP /utf-8)
    # Static runtime so the .exe runs without MSVC redistributables
    set_property(TARGET Silencer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── GCC/Clang (MinGW-w64) flags ───────────────────────────────
if(MINGW)
    target_compile_options(Silencer PRIVATE -Wall -Wextra)
    target_link_options(Silencer PRIVATE -static -mwindows)
endif()

# ── Post-build: remind user to copy ffmpeg/ffprobe ────────────
add_custom_command(TARGET Silencer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
        ">> Remember to copy ffmpeg.exe and ffprobe.exe next to Silencer.exe"
)

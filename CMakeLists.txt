cmake_minimum_required(VERSION 3.20)
project(Silencer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── MSVC-Runtime global auf statisch setzen (VOR FetchContent!) ─
# Stellt sicher, dass alle Deps (vorbis, ogg, ebur128) dieselbe
# Runtime verwenden wie das Hauptprojekt → behebt LNK2019 / LNK4098
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "" FORCE)
endif()

# ── Audio-Bibliotheken automatisch laden (FetchContent) ────────
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# libebur128 (für die LUFS Analyse)
FetchContent_Declare(libebur128 GIT_REPOSITORY https://github.com/jiixyj/libebur128.git GIT_TAG master)
FetchContent_MakeAvailable(libebur128)

# libogg (wird für libvorbis benötigt)
FetchContent_Declare(libogg GIT_REPOSITORY https://github.com/xiph/ogg.git GIT_TAG master)
FetchContent_MakeAvailable(libogg)

# libvorbis (Der OGG Encoder)
FetchContent_Declare(libvorbis GIT_REPOSITORY https://github.com/xiph/vorbis.git GIT_TAG master)
FetchContent_MakeAvailable(libvorbis)

# ── ImGui path (set via -DIMGUI_DIR=... or edit here) ──────────
if(NOT DEFINED IMGUI_DIR)
    set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/imgui"
        CACHE PATH "Path to the Dear ImGui source directory")
endif()

# ── Sources ────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
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

# ── Include-Verzeichnisse ──────────────────────────────────────
target_include_directories(Silencer PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${libebur128_SOURCE_DIR}/ebur128
    ${libogg_SOURCE_DIR}/include
    ${libvorbis_SOURCE_DIR}/include
)

# ── Linker-Bibliotheken ────────────────────────────────────────
target_link_libraries(Silencer PRIVATE
    # Windows / DirectX
    d3d11 dxgi comdlg32 shell32
    # Neue native Audio Engine
    ebur128 vorbisenc vorbis ogg
)

# ── MSVC-specific flags ────────────────────────────────────────
if(MSVC)
    target_compile_options(Silencer PRIVATE
        /W3 /MP /utf-8
        /D_CRT_SECURE_NO_WARNINGS   # unterdrückt fopen-Warnung (C4996)
    )
    # Redundant durch den globalen Cache-Wert oben, schadet aber nicht
    set_property(TARGET Silencer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── GCC/Clang (MinGW-w64) flags ───────────────────────────────
if(MINGW)
    target_compile_options(Silencer PRIVATE -Wall -Wextra)
    target_link_options(Silencer PRIVATE -static -mwindows)
endif()

# ── Font Tool kompilieren ─────────────────────────────────────
add_executable(font_tool "${IMGUI_DIR}/misc/fonts/binary_to_compressed_c.cpp")

# Konsolen-Anwendung erzwingen und MSVC-Flags übernehmen
if(MSVC)
    target_compile_options(font_tool PRIVATE /W3 /utf-8)
    set_property(TARGET font_tool PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
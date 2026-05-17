cmake_minimum_required(VERSION 3.15)
project(celsmooth_wasm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── SDL3 ────────────────────────────────────────────────────────────────────
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.14
    GIT_SHALLOW    TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

# ── Dear ImGui (docking branch) ────────────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3)

# ── CelSmooth core library ─────────────────────────────────────────────────
add_library(celsmooth_lib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mlaa.cpp
)
target_include_directories(celsmooth_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# ── WASM GUI executable ────────────────────────────────────────────────────
add_executable(celsmooth
    ${CMAKE_CURRENT_SOURCE_DIR}/../gui/gui_main.cpp
)
target_include_directories(celsmooth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendor
)
target_link_libraries(celsmooth PRIVATE
    celsmooth_lib
    imgui_lib
    SDL3::SDL3
)

# ── Emscripten-specific flags ──────────────────────────────────────────────
if(EMSCRIPTEN)
    set(SHELL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")

    target_link_options(celsmooth PRIVATE
        -sALLOW_MEMORY_GROWTH=1
        -sMAX_WEBGL_VERSION=2
        -sMIN_WEBGL_VERSION=2
        -sEXPORTED_FUNCTIONS=_main,_wasm_receive_file,_malloc,_free
        "-sEXPORTED_RUNTIME_METHODS=stringToNewUTF8,UTF8ToString,FS"
        --shell-file=${SHELL_FILE}
    )

    # Output as .html so Emscripten produces the trio: .html + .js + .wasm
    set_target_properties(celsmooth PROPERTIES SUFFIX ".html")
endif()

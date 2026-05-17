find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(celsmooth_gui
    gui_main.cpp
)

target_link_libraries(celsmooth_gui PRIVATE
    celsmooth_lib
    SDL3::SDL3
    imgui::imgui
)

target_include_directories(celsmooth_gui PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
)

# On Windows, hide console window (SDL_main.h handles WinMain->main)
if(WIN32)
    set_target_properties(celsmooth_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(celsmooth_gui PRIVATE /ENTRY:mainCRTStartup)
endif()

# Workaround: vcpkg's static SDL3 on Linux compiles with IBus support but does
# not propagate the ibus-1.0 link dependency to consumers. Pull it in ourselves.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(IBUS QUIET IMPORTED_TARGET ibus-1.0)
        if(IBUS_FOUND)
            target_link_libraries(celsmooth_gui PRIVATE PkgConfig::IBUS)
        endif()
    endif()
endif()

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

# On Windows, hide console window (SDL_main.h handles WinMain→main)
if(WIN32)
    set_target_properties(celsmooth_gui PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(celsmooth_gui PRIVATE /ENTRY:mainCRTStartup)
endif()

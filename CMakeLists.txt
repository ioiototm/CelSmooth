cmake_minimum_required(VERSION 3.15)
project(celsmooth VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- OpenMP (optional, pass -DENABLE_OPENMP=ON to enable) ---
option(ENABLE_OPENMP "Enable OpenMP parallelization" OFF)

if(ENABLE_OPENMP)
    find_package(OpenMP REQUIRED)
endif()

add_library(celsmooth_lib STATIC
    src/mlaa.cpp
)
target_include_directories(celsmooth_lib PUBLIC include)
target_compile_definitions(celsmooth_lib PUBLIC CELSMOOTH_VERSION="${PROJECT_VERSION}")

if(ENABLE_OPENMP)
    target_link_libraries(celsmooth_lib PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(celsmooth_lib PUBLIC CELSMOOTH_OPENMP=1)
endif()

add_executable(celsmooth src/main.cpp)
target_link_libraries(celsmooth PRIVATE celsmooth_lib)
target_include_directories(celsmooth PRIVATE vendor)

# --- GUI (optional, pass -DBUILD_GUI=ON to enable) ---
option(BUILD_GUI "Build the real-time GUI application (requires SDL3 + Dear ImGui via vcpkg)" OFF)

if(BUILD_GUI)
    add_subdirectory(gui)
endif()

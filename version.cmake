# Single source of truth for the CelSmooth version.
# Both the top-level build and the standalone WASM build include this file.
# Note: vcpkg.json has its own hardcoded "version" field that must be bumped
# manually to match (vcpkg consumes JSON, not CMake).
set(CELSMOOTH_VERSION 0.4.0)

# Absolute path to the celsmooth project root. CMAKE_CURRENT_LIST_DIR always
# evaluates to the directory of THIS file, so this resolves correctly no matter
# which CMakeLists includes it (top-level or wasm/).
set(CELSMOOTH_ROOT ${CMAKE_CURRENT_LIST_DIR})

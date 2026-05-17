# CelSmooth

A free, standalone Morphological Anti-Aliasing (MLAA) tool for cel art and anime-style images. Replicates the OLM Smoother workflow used in Japanese animation production, without requiring Adobe Creative Cloud.

## What it does

MLAA detects aliased edges in flat-color artwork, decomposes them into geometric patterns (L-shapes, Z-shapes, U-shapes, diagonals), and blends pixels to reconstruct a smoothed edge. The result preserves the hard, graphic look of cel art while eliminating staircase jaggies.

**Important:** This tool is designed for images with hard, aliased edges (no existing anti-aliasing). It works best on flat-color cel art, pixel art, vector renders, and anime production frames. If your image already has smooth anti-aliased edges or photographic gradients, MLAA will detect those transitions as edges and try to re-blend them, producing mushy or doubled smoothing artifacts. Feed it crisp, aliased input for best results.

## V1 and V2

CelSmooth implements two generations of the algorithm:

**V1 (classic)** is based on the original [Intel MLAA paper by Reshetov (SIGGRAPH Asia 2009)](https://dl.acm.org/doi/pdf/10.1145/1572769.1572787). It covers the core six-stage pipeline: luma extraction, edge detection, segment building, crossing classification, L-shape decomposition, and blending.

**V2** adds the extensions described in the [OLM Digital SIGGRAPH Asia 2024 paper](https://dl.acm.org/doi/10.1145/3681758.3697990), which documents the algorithm behind the OLM Smoother tool used in professional anime production. V2 adds T/cross-shape handling, diagonal line detection, sRGB gamma-correct blending, smoothness control, and U-shape rounding. All V2 features are on by default. Pass `--classic` to run V1 only.

## Features

- **T/Cross shapes** - correct handling of line intersections and T-junctions
- **Diagonal detection** - smooths 45-degree staircase patterns in pixel art
- **sRGB gamma correction** - blends in linear light for perceptually even gradients
- **Smoothness control** - adjustable falloff from sharp to very soft
- **U-shape rounding** - rounder corners where edges meet on the same side
- **Real-time GUI** - drag-and-drop image, live sliders, instant preview with split wipe or toggle comparison
- **One-click save** - save the processed result next to the original with a single button
- **OpenMP support** - optional multi-threaded build (~1.7x faster on a 1440x1080 image)
- **Zero-dependency library** - pure C++17 STL, easy to embed in other tools

## Screenshots

*Coming soon*

## Getting started

### Prerequisites

- CMake 3.15+
- A C++17 compiler (MSVC, GCC, Clang)
- **GUI only:** [vcpkg](https://github.com/microsoft/vcpkg) with SDL3 + Dear ImGui (auto-installed via manifest)

### Build: CLI only (no external deps)

```bash
cmake -B build
cmake --build build --config Release
```

### Build: CLI + GUI

```bash
cmake -B build -DBUILD_GUI=ON -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Build: with OpenMP (optional speedup)

Add `-DENABLE_OPENMP=ON` to either build above.

### Run

```bash
# CLI
celsmooth input.png output.png

# GUI
celsmooth_gui
celsmooth_gui input.png      # open with image pre-loaded
```

## CLI options

```
celsmooth <input.png> <output.png> [options]

Core:
  --threshold <float>       Edge sensitivity (default: 0.1)
  --search-distance <int>   Max segment length (default: 32)
  --strength <float>        Blend intensity 0.0-1.0 (default: 1.0)

V2 features (all on by default):
  --classic                 Disable all V2 features (original 2009 algorithm)
  --no-t-cross              Disable T/cross-shape handling
  --no-diagonals            Disable diagonal detection
  --no-gamma                Disable sRGB gamma correction
  --extended-gamma <float>  Thin stroke boost (default: 1.0)
  --smoothness <float>      Falloff shape 0.0-2.0 (default: 1.0)
  --no-u-rounding           Disable U-shape rounding

Debug:
  --debug-edges <path>      Edge map (red=H, blue=V, yellow=both)
  --debug-segments <path>   Segments colored by pattern type
  --debug-weights <path>    Blend weight heatmap
  --debug-diff <path>       Amplified difference (10x boosted)
  --debug-diagonals <path>  Diagonal edge map
  --debug-all <prefix>      All debug images at once
```

## Project structure

```
celsmooth/
  CMakeLists.txt
  vcpkg.json                # GUI dependencies manifest
  UNLICENSE
  include/celsmooth/
    mlaa.h                  # Public API
    mlaa_types.h            # Data structures
  src/
    mlaa.cpp                # Core algorithm (zero deps, C++17)
    main.cpp                # CLI + stb image I/O + debug visualizations
  gui/
    CMakeLists.txt
    gui_main.cpp            # Dear ImGui + SDL3 real-time GUI
  vendor/
    stb_image.h
    stb_image_write.h
```

## Embedding the library

`celsmooth_lib` has no dependencies beyond the C++17 standard library. To use it in your own project:

```cpp
#include "celsmooth/mlaa.h"

celsmooth::MlaaParams params;
params.threshold = 0.08f;

celsmooth::mlaa_process(pixels_rgba, output_rgba, width, height, width * 4, params);
```

## Roadmap

- **Batch processing** - process entire folders of animation frames in one pass
- **SIMD/AVX intrinsics** - vectorized edge detection and blending for further CPU speedup
- **GPU acceleration** - GLSL/HLSL compute shader port for real-time use in game engines and compositing software
- **WebAssembly build** - run in the browser with zero install (the zero-dep library design already supports this)
- **Bilateral polish pass** - optional second pass to smooth residual noise without blurring edges
- **Prebuilt binaries** - GitHub releases with ready-to-run Windows/Linux/macOS executables

## Acknowledgements

This project was implemented with the help of [Claude Opus 4.6](https://www.anthropic.com/claude) (Anthropic). The algorithm research, C++ implementation, GUI, and build system were all developed in collaboration with the AI assistant.

## License

Public domain. See [UNLICENSE](UNLICENSE).

The stb libraries in `vendor/` are also public domain (Sean Barrett).
SDL3 and Dear ImGui are used as runtime dependencies under their respective licenses (zlib and MIT).

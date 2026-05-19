#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "celsmooth/mlaa.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

// --- PNG color profile preservation ---
//
// stb_image strips ICC profiles when loading. Without them, viewers may
// display colors differently (especially on HDR / wide-gamut monitors).
// We extract the color-related chunks from the original PNG and inject
// them into every output PNG to preserve the intended appearance.

struct PngColorProfile {
    std::vector<uint8_t> chunks;  // raw PNG chunks (iCCP, sRGB, gAMA, cHRM)
};

static PngColorProfile extract_png_color_profile(const char* path) {
    PngColorProfile profile;
    FILE* f = fopen(path, "rb");
    if (!f) return profile;

    // Verify PNG signature
    uint8_t sig[8];
    if (fread(sig, 1, 8, f) != 8) { fclose(f); return profile; }
    const uint8_t png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (memcmp(sig, png_sig, 8) != 0) { fclose(f); return profile; }

    // Scan chunks until IDAT (all metadata comes before image data)
    while (!feof(f)) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;
        uint32_t length = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                        | ((uint32_t)hdr[2] << 8)  |  (uint32_t)hdr[3];
        char type[5] = { (char)hdr[4], (char)hdr[5], (char)hdr[6], (char)hdr[7], 0 };

        // Read chunk data + CRC (4 bytes)
        std::vector<uint8_t> body(length + 4);
        if (fread(body.data(), 1, length + 4, f) != length + 4) break;

        // Keep color-related chunks
        if (strcmp(type, "iCCP") == 0 || strcmp(type, "sRGB") == 0 ||
            strcmp(type, "gAMA") == 0 || strcmp(type, "cHRM") == 0) {
            profile.chunks.insert(profile.chunks.end(), hdr, hdr + 8);
            profile.chunks.insert(profile.chunks.end(), body.begin(), body.end());
        }

        // Stop at IDAT — no color chunks appear after this
        if (strcmp(type, "IDAT") == 0) break;
    }
    fclose(f);
    return profile;
}

// sRGB fallback chunk for when the source has no color profile
static std::vector<uint8_t> make_srgb_chunk() {
    uint8_t td[5] = { 's', 'R', 'G', 'B', 0x00 };
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < 5; i++) {
        crc ^= td[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1)));
    }
    crc ^= 0xFFFFFFFF;
    return { 0,0,0,1, 's','R','G','B', 0x00,
             (uint8_t)(crc>>24), (uint8_t)(crc>>16),
             (uint8_t)(crc>>8),  (uint8_t)(crc) };
}

static bool save_png_with_profile(const char* path, int w, int h, int comp,
                                  const void* data, int stride,
                                  const PngColorProfile& profile) {
    std::vector<uint8_t> png;
    int ok = stbi_write_png_to_func([](void* ctx, void* d, int sz) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->insert(v->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + sz);
    }, &png, w, h, comp, data, stride);
    if (!ok || png.size() < 33) return false;

    // Insert color profile after IHDR (offset 33)
    if (!profile.chunks.empty()) {
        png.insert(png.begin() + 33, profile.chunks.begin(), profile.chunks.end());
    } else {
        auto srgb = make_srgb_chunk();
        png.insert(png.begin() + 33, srgb.begin(), srgb.end());
    }

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return written == png.size();
}

// Convenience wrapper for debug images (no source profile, just tag as sRGB)
static bool save_png_srgb(const char* path, int w, int h, int comp, const void* data, int stride) {
    PngColorProfile empty;
    return save_png_with_profile(path, w, h, comp, data, stride, empty);
}

static void print_usage(const char* prog) {
    fprintf(stderr,
        "CelSmooth v" CELSMOOTH_VERSION " — Morphological Anti-Aliasing for cel art\n"
        "\n"
        "Usage: %s <input.png> <output.png> [options]\n"
        "\n"
        "Options:\n"
        "  --threshold <float>        Edge detection threshold (default: 0.1)\n"
        "  --search-distance <int>    Max edge segment length (default: 32)\n"
        "  --strength <float>         Blend strength 0.0-1.0 (default: 1.0)\n"
        "\n"
        "V2 features (on by default):\n"
        "  --classic                  Disable all V2 features (original algorithm)\n"
        "  --no-t-cross               Disable T/cross-shape handling\n"
        "  --no-diagonals             Disable diagonal line detection\n"
        "  --no-gamma                 Disable sRGB gamma correction\n"
        "  --extended-gamma <float>   Thin stroke boost (default: 1.0, >1 = stronger)\n"
        "  --smoothness <float>       Smoothing intensity 0.0-2.0 (default: 1.0)\n"
        "  --no-u-rounding            Disable U-shape rounding\n"
        "\n"
        "Debug visualizations:\n"
        "  --debug-edges <path.png>   Edge map (red=H, blue=V, yellow=both)\n"
        "  --debug-segments <path.png> Segments colored by pattern on dimmed original\n"
        "  --debug-weights <path.png> Blend weight heatmap\n"
        "  --debug-diff <path.png>    Amplified difference (output vs input)\n"
        "  --debug-all <prefix>       Export all debug images as <prefix>_edges.png etc.\n"
        "  --help                     Show this help message\n",
        prog);
}

static void save_edge_debug(const celsmooth::EdgeMap& edges, const char* path) {
    int w = edges.width, h = edges.height;
    std::vector<uint8_t> img(w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* px = img.data() + (y * w + x) * 4;
            bool he = edges.h(x, y);
            bool ve = edges.v(x, y);
            if (he && ve) { px[0] = 255; px[1] = 255; px[2] = 0; }   // yellow: both
            else if (he)  { px[0] = 255; px[1] = 60;  px[2] = 60; }  // red: horizontal
            else if (ve)  { px[0] = 60;  px[1] = 120; px[2] = 255; } // blue: vertical
            else          { px[0] = 20;  px[1] = 20;  px[2] = 20; }  // dark gray: none
            px[3] = 255;
        }
    }
    save_png_srgb(path, w, h, 4, img.data(), w * 4);
    fprintf(stderr, "Edge map saved to: %s\n", path);
}

static void save_segment_debug(
    const uint8_t* pixels, int w, int h, int stride,
    const std::vector<celsmooth::EdgeSegment>& segments,
    const char* path)
{
    std::vector<uint8_t> img(w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* src = pixels + y * stride + x * 4;
            uint8_t* dst = img.data() + (y * w + x) * 4;
            dst[0] = src[0] / 3;
            dst[1] = src[1] / 3;
            dst[2] = src[2] / 3;
            dst[3] = 255;
        }
    }

    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x >= 0 && x < w && y >= 0 && y < h) {
            uint8_t* px = img.data() + (y * w + x) * 4;
            px[0] = r; px[1] = g; px[2] = b;
        }
    };

    using CT = celsmooth::EdgeSegment::CrossType;
    for (const auto& seg : segments) {
        bool s_both = (seg.start_cross == CT::Both);
        bool e_both = (seg.end_cross == CT::Both);
        bool has_s = (seg.start_cross != CT::None);
        bool has_e = (seg.end_cross != CT::None);
        uint8_t r, g, b;
        if (s_both && e_both) {
            r = 255; g = 255; b = 255; // white: cross-shape
        } else if (s_both || e_both) {
            r = 255; g = 165; b = 0;   // orange: T-shape
        } else if (has_s && has_e && seg.start_cross != seg.end_cross) {
            r = 0; g = 220; b = 255;   // cyan: Z-shape
        } else if (has_s && has_e) {
            r = 220; g = 60; b = 255;  // magenta: U-shape
        } else if (has_s || has_e) {
            r = 60; g = 255; b = 100;  // green: L-shape
        } else {
            r = 120; g = 120; b = 120; // gray: no pattern
        }

        if (seg.dir == celsmooth::EdgeSegment::Dir::Horizontal) {
            int y = seg.fixed_coord;
            for (int x = seg.start; x <= seg.end; ++x) {
                put(x, y, r, g, b);
                put(x, y - 1, r, g, b);
            }
        } else {
            int x = seg.fixed_coord;
            for (int y = seg.start; y <= seg.end; ++y) {
                put(x, y, r, g, b);
                put(x - 1, y, r, g, b);
            }
        }
    }

    save_png_srgb(path, w, h, 4, img.data(), w * 4);
    fprintf(stderr, "Segment map saved to: %s\n", path);
}

static void save_weights_debug(
    const celsmooth::BlendWeights& weights,
    const char* path)
{
    int w = weights.width, h = weights.height;
    std::vector<uint8_t> img(w * h * 4, 0);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int idx = y * w + x;
            float wh = std::max(weights.above[idx], weights.below[idx]);
            float wv = std::max(weights.left[idx], weights.right[idx]);

            // H-blending in red channel, V-blending in blue, both in yellow
            uint8_t* px = img.data() + idx * 4;
            auto to_byte = [](float v) -> uint8_t {
                float boosted = v * 4.0f;
                return static_cast<uint8_t>(std::min(boosted * 255.0f, 255.0f));
            };

            uint8_t rr = to_byte(wh);
            uint8_t bb = to_byte(wv);
            px[0] = std::min(255, rr + bb / 3);
            px[1] = std::min(255, rr / 3 + bb / 3);
            px[2] = std::min(255, rr / 3 + bb);
            px[3] = 255;
        }
    }

    save_png_srgb(path, w, h, 4, img.data(), w * 4);
    fprintf(stderr, "Weight heatmap saved to: %s\n", path);
}

static void save_diff_debug(
    const uint8_t* pixels_in, const uint8_t* pixels_out,
    int w, int h, int stride,
    const char* path)
{
    std::vector<uint8_t> img(w * h * 4);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* src = pixels_in  + y * stride + x * 4;
            const uint8_t* dst = pixels_out + y * stride + x * 4;
            uint8_t* px = img.data() + (y * w + x) * 4;

            int dr = std::abs((int)dst[0] - (int)src[0]);
            int dg = std::abs((int)dst[1] - (int)src[1]);
            int db = std::abs((int)dst[2] - (int)src[2]);
            int maxd = std::max({dr, dg, db});

            // 10x amplification with heat palette
            float t = std::min(maxd * 10.0f / 255.0f, 1.0f);
            if (t < 0.001f) {
                px[0] = 15; px[1] = 15; px[2] = 15;
            } else if (t < 0.33f) {
                float s = t / 0.33f;
                px[0] = static_cast<uint8_t>(s * 255);
                px[1] = 0;
                px[2] = 0;
            } else if (t < 0.66f) {
                float s = (t - 0.33f) / 0.33f;
                px[0] = 255;
                px[1] = static_cast<uint8_t>(s * 255);
                px[2] = 0;
            } else {
                float s = (t - 0.66f) / 0.34f;
                px[0] = 255;
                px[1] = 255;
                px[2] = static_cast<uint8_t>(s * 255);
            }
            px[3] = 255;
        }
    }

    save_png_srgb(path, w, h, 4, img.data(), w * 4);
    fprintf(stderr, "Diff map saved to: %s\n", path);
}

static void save_diagonal_debug(const celsmooth::DiagonalMap& diag, const char* path) {
    int w = diag.width, h = diag.height;
    std::vector<uint8_t> img(w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* px = img.data() + (y * w + x) * 4;
            bool dr = diag.dr(x, y);
            bool dl = diag.dl(x, y);
            if (dr && dl) { px[0] = 255; px[1] = 255; px[2] = 0; }   // yellow: both
            else if (dr)  { px[0] = 60;  px[1] = 255; px[2] = 100; } // green: DownRight
            else if (dl)  { px[0] = 0;   px[1] = 220; px[2] = 255; } // cyan: DownLeft
            else          { px[0] = 20;  px[1] = 20;  px[2] = 20; }
            px[3] = 255;
        }
    }
    save_png_srgb(path, w, h, 4, img.data(), w * 4);
    fprintf(stderr, "Diagonal edge map saved to: %s\n", path);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char* input_path  = argv[1];
    const char* output_path = argv[2];
    const char* debug_edges_path    = nullptr;
    const char* debug_segments_path = nullptr;
    const char* debug_weights_path  = nullptr;
    const char* debug_diff_path     = nullptr;
    const char* debug_diag_path     = nullptr;
    std::string debug_all_prefix;
    celsmooth::MlaaParams params;

    if (strcmp(input_path, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threshold" && i + 1 < argc) {
            params.threshold = std::stof(argv[++i]);
        } else if (arg == "--search-distance" && i + 1 < argc) {
            params.max_distance = std::stoi(argv[++i]);
        } else if (arg == "--strength" && i + 1 < argc) {
            params.strength = std::stof(argv[++i]);
        } else if (arg == "--debug-edges" && i + 1 < argc) {
            debug_edges_path = argv[++i];
        } else if (arg == "--debug-segments" && i + 1 < argc) {
            debug_segments_path = argv[++i];
        } else if (arg == "--debug-weights" && i + 1 < argc) {
            debug_weights_path = argv[++i];
        } else if (arg == "--debug-diff" && i + 1 < argc) {
            debug_diff_path = argv[++i];
        } else if (arg == "--debug-diagonals" && i + 1 < argc) {
            debug_diag_path = argv[++i];
        } else if (arg == "--debug-all" && i + 1 < argc) {
            debug_all_prefix = argv[++i];
        } else if (arg == "--classic") {
            params.classic_mode = true;
        } else if (arg == "--no-t-cross") {
            params.enable_t_cross = false;
        } else if (arg == "--no-diagonals") {
            params.enable_diagonals = false;
        } else if (arg == "--no-gamma") {
            params.enable_gamma = false;
        } else if (arg == "--extended-gamma" && i + 1 < argc) {
            params.extended_gamma = std::stof(argv[++i]);
        } else if (arg == "--smoothness" && i + 1 < argc) {
            params.smoothness = std::stof(argv[++i]);
        } else if (arg == "--no-u-rounding") {
            params.u_rounding = false;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    // --debug-all expands to all individual debug paths
    std::string da_edges, da_segments, da_weights, da_diff, da_diag;
    if (!debug_all_prefix.empty()) {
        da_edges    = debug_all_prefix + "_edges.png";
        da_segments = debug_all_prefix + "_segments.png";
        da_weights  = debug_all_prefix + "_weights.png";
        da_diff     = debug_all_prefix + "_diff.png";
        da_diag     = debug_all_prefix + "_diagonals.png";
        if (!debug_edges_path)    debug_edges_path    = da_edges.c_str();
        if (!debug_segments_path) debug_segments_path = da_segments.c_str();
        if (!debug_weights_path)  debug_weights_path  = da_weights.c_str();
        if (!debug_diff_path)     debug_diff_path     = da_diff.c_str();
        if (!debug_diag_path)     debug_diag_path     = da_diag.c_str();
    }

    // Load image
    int w, h, channels;
    uint8_t* pixels = stbi_load(input_path, &w, &h, &channels, 4); // force RGBA
    if (!pixels) {
        fprintf(stderr, "Error: failed to load '%s': %s\n", input_path, stbi_failure_reason());
        return 1;
    }
    int stride = w * 4;
    fprintf(stderr, "Loaded: %s (%dx%d, %d channels)\n", input_path, w, h, channels);

    // Extract color profile from the original to preserve it in the output
    PngColorProfile color_profile = extract_png_color_profile(input_path);

    bool any_debug = debug_edges_path || debug_segments_path || debug_weights_path || debug_diff_path || debug_diag_path;

    std::vector<uint8_t> output(static_cast<size_t>(stride) * h);
    auto t0 = std::chrono::high_resolution_clock::now();

    if (!any_debug) {
        celsmooth::mlaa_process(pixels, output.data(), w, h, stride, params);
    } else {
        // Run pipeline stage by stage so we can tap intermediate results
        std::vector<float> luma;
        celsmooth::mlaa_compute_luma(pixels, w, h, stride, luma);

        celsmooth::EdgeMap edges;
        celsmooth::mlaa_detect_edges(luma.data(), w, h, params.threshold, edges);
        if (debug_edges_path)
            save_edge_debug(edges, debug_edges_path);

        // Diagonal detection (before orthogonal segment building)
        bool do_diag = params.enable_diagonals && !params.classic_mode;
        celsmooth::DiagonalMap diagonals;
        std::vector<celsmooth::DiagonalSegment> diag_segments;
        if (do_diag) {
            float diag_thresh = params.diagonal_threshold > 0
                                ? params.diagonal_threshold : params.threshold;
            celsmooth::mlaa_detect_diagonal_edges(luma.data(), w, h, diag_thresh, diagonals);
            if (debug_diag_path)
                save_diagonal_debug(diagonals, debug_diag_path);
            celsmooth::mlaa_build_diagonal_segments(diagonals, params.diagonal_max_distance, diag_segments);
            celsmooth::mlaa_suppress_orthogonal_overlaps(edges, diag_segments);
        }

        std::vector<celsmooth::EdgeSegment> segments;
        celsmooth::mlaa_build_segments(edges, params.max_distance, segments);
        celsmooth::mlaa_classify_segments(edges, segments, params);
        if (debug_segments_path)
            save_segment_debug(pixels, w, h, stride, segments, debug_segments_path);

        std::vector<celsmooth::LShape> lshapes;
        celsmooth::mlaa_decompose_to_lshapes(segments, lshapes);

        celsmooth::BlendWeights weights;
        celsmooth::mlaa_compute_blend_weights(lshapes, w, h, params, weights);

        if (do_diag)
            celsmooth::mlaa_compute_diagonal_blend_weights(diag_segments, w, h, params.strength, weights);

        if (debug_weights_path)
            save_weights_debug(weights, debug_weights_path);

        celsmooth::mlaa_apply_blending(pixels, output.data(), w, h, stride, weights, params);

        if (debug_diff_path)
            save_diff_debug(pixels, output.data(), w, h, stride, debug_diff_path);

        fprintf(stderr, "Pipeline: %zu segments, %zu L-shapes, %zu diagonal segments\n",
                segments.size(), lshapes.size(), diag_segments.size());
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    fprintf(stderr, "Processed in %.1f ms\n", ms);

    // Save
    if (!save_png_with_profile(output_path, w, h, 4, output.data(), stride, color_profile)) {
        fprintf(stderr, "Error: failed to write '%s'\n", output_path);
        stbi_image_free(pixels);
        return 1;
    }
    fprintf(stderr, "Saved: %s\n", output_path);

    stbi_image_free(pixels);
    return 0;
}

#include "celsmooth/mlaa.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef CELSMOOTH_OPENMP
#include <omp.h>
#endif

namespace celsmooth {

// --- Stage 1: Luma ---

void mlaa_compute_luma(
    const uint8_t* pixels, int width, int height, int stride,
    std::vector<float>& luma_out)
{
    luma_out.resize(static_cast<size_t>(width) * height);
    #ifdef CELSMOOTH_OPENMP
    #pragma omp parallel for schedule(static)
    #endif
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = pixels + y * stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t* px = row + x * 4;
            luma_out[y * width + x] =
                (0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]) / 255.0f;
        }
    }
}

// --- Stage 2: Edge detection ---

void mlaa_detect_edges(
    const float* luma, int width, int height,
    float threshold,
    EdgeMap& edges_out)
{
    edges_out.width  = width;
    edges_out.height = height;
    size_t n = static_cast<size_t>(width) * height;
    edges_out.h_edges.assign(n, 0);
    edges_out.v_edges.assign(n, 0);

    #ifdef CELSMOOTH_OPENMP
    #pragma omp parallel for schedule(static)
    #endif
    for (int y = 1; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (std::abs(luma[y * width + x] - luma[(y - 1) * width + x]) > threshold)
                edges_out.h_edges[y * width + x] = 1;
        }
    }
    #ifdef CELSMOOTH_OPENMP
    #pragma omp parallel for schedule(static)
    #endif
    for (int y = 0; y < height; ++y) {
        for (int x = 1; x < width; ++x) {
            if (std::abs(luma[y * width + x] - luma[y * width + (x - 1)]) > threshold)
                edges_out.v_edges[y * width + x] = 1;
        }
    }
}

// --- Stage 3: Build segments ---

void mlaa_build_segments(
    const EdgeMap& edges, int max_distance,
    std::vector<EdgeSegment>& segments_out)
{
    segments_out.clear();
    int w = edges.width;
    int h = edges.height;

    // Horizontal segments: sweep each row boundary
    for (int y = 1; y < h; ++y) {
        int x = 0;
        while (x < w) {
            if (edges.h(x, y)) {
                int start = x;
                while (x < w && edges.h(x, y)) ++x;
                int end = x - 1;
                if (end - start + 1 <= max_distance) {
                    EdgeSegment seg;
                    seg.dir         = EdgeSegment::Dir::Horizontal;
                    seg.fixed_coord = y;
                    seg.start       = start;
                    seg.end         = end;
                    segments_out.push_back(seg);
                }
            } else {
                ++x;
            }
        }
    }

    // Vertical segments: sweep each column boundary
    for (int x = 1; x < w; ++x) {
        int y = 0;
        while (y < h) {
            if (edges.v(x, y)) {
                int start = y;
                while (y < h && edges.v(x, y)) ++y;
                int end = y - 1;
                if (end - start + 1 <= max_distance) {
                    EdgeSegment seg;
                    seg.dir         = EdgeSegment::Dir::Vertical;
                    seg.fixed_coord = x;
                    seg.start       = start;
                    seg.end         = end;
                    segments_out.push_back(seg);
                }
            } else {
                ++y;
            }
        }
    }
}

// --- Stage 4: Classify crossing edges ---

static EdgeSegment::CrossType check_h_crossing(
    const EdgeMap& edges, int x, int y, bool enable_t_cross)
{
    int w = edges.width;
    if (x < 1 || x >= w) return EdgeSegment::CrossType::None;

    bool above = (y - 1 >= 0) && edges.v(x, y - 1);
    bool below = (y < edges.height) && edges.v(x, y);

    if (above && !below) return EdgeSegment::CrossType::Above;
    if (below && !above) return EdgeSegment::CrossType::Below;
    if (above && below && enable_t_cross)
        return EdgeSegment::CrossType::Both;
    return EdgeSegment::CrossType::None;
}

static EdgeSegment::CrossType check_v_crossing(
    const EdgeMap& edges, int x, int y, bool enable_t_cross)
{
    int h = edges.height;
    if (y < 1 || y >= h) return EdgeSegment::CrossType::None;

    bool left  = (x - 1 >= 0) && edges.h(x - 1, y);
    bool right = (x < edges.width) && edges.h(x, y);

    if (left && !right)  return EdgeSegment::CrossType::Above;
    if (right && !left)  return EdgeSegment::CrossType::Below;
    if (left && right && enable_t_cross)
        return EdgeSegment::CrossType::Both;
    return EdgeSegment::CrossType::None;
}

void mlaa_classify_segments(
    const EdgeMap& edges,
    std::vector<EdgeSegment>& segments,
    const MlaaParams& params)
{
    bool t_cross = params.enable_t_cross && !params.classic_mode;
    for (auto& seg : segments) {
        if (seg.dir == EdgeSegment::Dir::Horizontal) {
            seg.start_cross = check_h_crossing(edges, seg.start, seg.fixed_coord, t_cross);
            seg.end_cross = check_h_crossing(edges, seg.end + 1, seg.fixed_coord, t_cross);
        } else {
            seg.start_cross = check_v_crossing(edges, seg.fixed_coord, seg.start, t_cross);
            seg.end_cross = check_v_crossing(edges, seg.fixed_coord, seg.end + 1, t_cross);
        }
    }
}

// --- Stage 5: Decompose to L-shapes ---

static void add_lshape(
    std::vector<LShape>& out,
    EdgeSegment::Dir dir, int fixed_coord,
    int start, int length,
    bool secondary_at_start,
    EdgeSegment::CrossType cross_side,
    bool u_shape = false)
{
    LShape ls;
    ls.dir                      = dir;
    ls.fixed_coord              = fixed_coord;
    ls.start                    = start;
    ls.length                   = length;
    ls.secondary_at_start       = secondary_at_start;
    ls.blend_with_above_or_left = (cross_side == EdgeSegment::CrossType::Below);
    ls.from_u_shape             = u_shape;
    out.push_back(ls);
}

void mlaa_decompose_to_lshapes(
    const std::vector<EdgeSegment>& segments,
    std::vector<LShape>& lshapes_out)
{
    lshapes_out.clear();
    using CT = EdgeSegment::CrossType;

    auto opposite = [](CT c) -> CT {
        if (c == CT::Above) return CT::Below;
        if (c == CT::Below) return CT::Above;
        return CT::None;
    };

    for (const auto& seg : segments) {
        CT sc = seg.start_cross;
        CT ec = seg.end_cross;
        int len = seg.length();

        bool s_simple = (sc == CT::Above || sc == CT::Below);
        bool e_simple = (ec == CT::Above || ec == CT::Below);
        bool s_both   = (sc == CT::Both);
        bool e_both   = (ec == CT::Both);

        // Resolve effective cross types for start and end:
        // - Simple crossings (Above/Below) pass through unchanged
        // - Both (T-junction): pick direction opposing the other endpoint → Z-shape
        //   If other endpoint is None, emit two L-shapes (one per direction)
        CT eff_s = sc;
        CT eff_e = ec;

        if (s_both && e_simple) {
            eff_s = opposite(ec);
        } else if (e_both && s_simple) {
            eff_e = opposite(sc);
        } else if (s_both && e_both) {
            // Cross-shape: paper §2.2.1 — use Z-shape reconstruction
            eff_s = CT::Above;
            eff_e = CT::Below;
        } else if (s_both && ec == CT::None) {
            // T at start, nothing at end: emit two half-length L-shapes
            int half = len / 2;
            int rest = len - half;
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start, half, true, CT::Above);
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start + half, rest, true, CT::Below);
            continue;
        } else if (e_both && sc == CT::None) {
            // T at end, nothing at start: emit two half-length L-shapes
            int half = len / 2;
            int rest = len - half;
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start, half, false, CT::Above);
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start + half, rest, false, CT::Below);
            continue;
        }

        bool has_start = (eff_s == CT::Above || eff_s == CT::Below);
        bool has_end   = (eff_e == CT::Above || eff_e == CT::Below);

        if (!has_start && !has_end) {
            continue;
        }

        if (has_start && !has_end) {
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start, len, true, eff_s);
        }
        else if (!has_start && has_end) {
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start, len, false, eff_e);
        }
        else {
            // Both ends have crossings → split at midpoint
            bool is_u = (eff_s == eff_e);
            int half = len / 2;
            int rest = len - half;
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start, half, true, eff_s, is_u);
            add_lshape(lshapes_out, seg.dir, seg.fixed_coord,
                       seg.start + half, rest, false, eff_e, is_u);
        }
    }
}

// --- Stage 5b: Blend weight computation ---

void mlaa_compute_blend_weights(
    const std::vector<LShape>& lshapes,
    int width, int height,
    const MlaaParams& params,
    BlendWeights& weights_out)
{
    bool diag = params.enable_diagonals && !params.classic_mode;
    weights_out.init(width, height, diag);
    float strength = params.strength;

    bool do_smoothness = !params.classic_mode && params.smoothness != 1.0f;
    bool do_u_rounding = !params.classic_mode && params.u_rounding;

    for (const auto& ls : lshapes) {
        int p = ls.length;
        if (p <= 0) continue;

        // Smoothness: scale the effective length
        float eff_p = static_cast<float>(p);
        if (do_smoothness)
            eff_p = eff_p * params.smoothness;
        if (eff_p < 1.0f) eff_p = 1.0f;

        // Extra-smoothing (paper §4 / Fig 16): when smoothness>1, extend the
        // affected pixel range, not just the trapezoid denominator.
        int p_iter = std::max(p, static_cast<int>(std::round(eff_p)));

        for (int i = 0; i < p_iter; ++i) {
            float area = (2.0f * eff_p - 2.0f * i - 1.0f) / (4.0f * eff_p);
            float w = area * strength;

            // U-rounding: w' = 2w² for U-shapes (paper §2.2.3)
            if (do_u_rounding && ls.from_u_shape && w > 0.0f && w <= 0.5f)
                w = 2.0f * w * w;

            if (w <= 0.0f) continue;

            // Which pixel along the segment does index i correspond to?
            int coord;
            if (ls.secondary_at_start) {
                coord = ls.start + i;
            } else {
                coord = ls.start + ls.length - 1 - i;
            }

            if (ls.dir == EdgeSegment::Dir::Horizontal) {
                // Horizontal edge at row boundary fixed_coord (between row fixed_coord-1 and row fixed_coord)
                // The affected pixel row depends on which side the crossing is:
                // blend_with_above_or_left=true → crossing is Above → diagonal cuts into the below row
                //   → pixel at row fixed_coord blends with pixel at row fixed_coord-1
                // blend_with_above_or_left=false → crossing is Below → diagonal cuts into the above row
                //   → pixel at row fixed_coord-1 blends with pixel at row fixed_coord
                int px = coord;
                if (ls.blend_with_above_or_left) {
                    int py = ls.fixed_coord; // the below-row pixel
                    if (py >= 0 && py < height && px >= 0 && px < width) {
                        float& slot = weights_out.above[py * width + px];
                        slot = std::max(slot, w);
                    }
                } else {
                    int py = ls.fixed_coord - 1; // the above-row pixel
                    if (py >= 0 && py < height && px >= 0 && px < width) {
                        float& slot = weights_out.below[py * width + px];
                        slot = std::max(slot, w);
                    }
                }
            } else {
                // Vertical edge at column boundary fixed_coord (between col fixed_coord-1 and col fixed_coord)
                // blend_with_above_or_left=true → crossing is Left → diagonal cuts into the right column
                //   → pixel at col fixed_coord blends with pixel at col fixed_coord-1
                // blend_with_above_or_left=false → crossing is Right → diagonal cuts into the left column
                //   → pixel at col fixed_coord-1 blends with pixel at col fixed_coord
                int py = coord;
                if (ls.blend_with_above_or_left) {
                    int px = ls.fixed_coord; // the right-column pixel
                    if (py >= 0 && py < height && px >= 0 && px < width) {
                        float& slot = weights_out.left[py * width + px];
                        slot = std::max(slot, w);
                    }
                } else {
                    int px = ls.fixed_coord - 1; // the left-column pixel
                    if (py >= 0 && py < height && px >= 0 && px < width) {
                        float& slot = weights_out.right[py * width + px];
                        slot = std::max(slot, w);
                    }
                }
            }
        }
    }
}

// --- Gamma helpers ---

static float srgb_lut[256];
static bool  srgb_lut_ready = false;

static void init_srgb_lut() {
    if (srgb_lut_ready) return;
    for (int i = 0; i < 256; ++i) {
        float s = i / 255.0f;
        srgb_lut[i] = (s <= 0.04045f) ? s / 12.92f
                                       : std::pow((s + 0.055f) / 1.055f, 2.4f);
    }
    srgb_lut_ready = true;
}

static inline float srgb_to_linear(uint8_t v) {
    return srgb_lut[v];
}

static inline uint8_t linear_to_srgb(float l) {
    float s = (l <= 0.0031308f) ? l * 12.92f
                                : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::min(std::max(s * 255.0f + 0.5f, 0.0f), 255.0f));
}

// --- Stage 6: Apply blending ---

static inline uint8_t blend_channel(uint8_t a, uint8_t b, float t) {
    float result = a * (1.0f - t) + b * t;
    return static_cast<uint8_t>(std::min(std::max(result + 0.5f, 0.0f), 255.0f));
}

static inline uint8_t blend_channel_linear(uint8_t a, uint8_t b, float t, float gamma) {
    float la = srgb_to_linear(a);
    float lb = srgb_to_linear(b);
    if (gamma != 1.0f) {
        la = std::pow(la, gamma);
        lb = std::pow(lb, gamma);
    }
    float result = la * (1.0f - t) + lb * t;
    if (gamma != 1.0f)
        result = std::pow(result, 1.0f / gamma);
    return linear_to_srgb(result);
}

void mlaa_apply_blending(
    const uint8_t* pixels_in,
    uint8_t*       pixels_out,
    int width, int height, int stride,
    const BlendWeights& weights,
    const MlaaParams& params)
{
    bool use_gamma = params.enable_gamma && !params.classic_mode;
    float ext_gamma = use_gamma ? params.extended_gamma : 1.0f;
    if (use_gamma) init_srgb_lut();

    auto blend = [use_gamma, ext_gamma](uint8_t a, uint8_t b, float t) -> uint8_t {
        return use_gamma ? blend_channel_linear(a, b, t, ext_gamma)
                         : blend_channel(a, b, t);
    };

    #ifdef CELSMOOTH_OPENMP
    #pragma omp parallel for schedule(static)
    #endif
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            const uint8_t* src = pixels_in + y * stride + x * 4;

            uint8_t r = src[0], g = src[1], b = src[2], a = src[3];

            // Horizontal-edge blending (vertical neighbors)
            float wa = weights.above[idx];
            float wb = weights.below[idx];
            if (wa > 0.0f || wb > 0.0f) {
                if (wa >= wb && y > 0) {
                    const uint8_t* nb = pixels_in + (y - 1) * stride + x * 4;
                    r = blend(r, nb[0], wa);
                    g = blend(g, nb[1], wa);
                    b = blend(b, nb[2], wa);
                    a = blend_channel(a, nb[3], wa);
                } else if (wb > wa && y + 1 < height) {
                    const uint8_t* nb = pixels_in + (y + 1) * stride + x * 4;
                    r = blend(r, nb[0], wb);
                    g = blend(g, nb[1], wb);
                    b = blend(b, nb[2], wb);
                    a = blend_channel(a, nb[3], wb);
                }
            }

            // Vertical-edge blending (horizontal neighbors)
            float wl = weights.left[idx];
            float wr = weights.right[idx];
            if (wl > 0.0f || wr > 0.0f) {
                if (wl >= wr && x > 0) {
                    const uint8_t* nb = pixels_in + y * stride + (x - 1) * 4;
                    r = blend(r, nb[0], wl);
                    g = blend(g, nb[1], wl);
                    b = blend(b, nb[2], wl);
                    a = blend_channel(a, nb[3], wl);
                } else if (wr > wl && x + 1 < width) {
                    const uint8_t* nb = pixels_in + y * stride + (x + 1) * 4;
                    r = blend(r, nb[0], wr);
                    g = blend(g, nb[1], wr);
                    b = blend(b, nb[2], wr);
                    a = blend_channel(a, nb[3], wr);
                }
            }

            // Diagonal blending
            if (weights.has_diagonal()) {
                float wal = weights.above_left[idx];
                float wbr = weights.below_right[idx];
                float war = weights.above_right[idx];
                float wbl = weights.below_left[idx];
                float max_diag = std::max({wal, wbr, war, wbl});
                if (max_diag > 0.0f) {
                    const uint8_t* nb = nullptr;
                    float wd = 0.0f;
                    if (wal == max_diag && x > 0 && y > 0) {
                        nb = pixels_in + (y - 1) * stride + (x - 1) * 4;
                        wd = wal;
                    } else if (wbr == max_diag && x + 1 < width && y + 1 < height) {
                        nb = pixels_in + (y + 1) * stride + (x + 1) * 4;
                        wd = wbr;
                    } else if (war == max_diag && x + 1 < width && y > 0) {
                        nb = pixels_in + (y - 1) * stride + (x + 1) * 4;
                        wd = war;
                    } else if (wbl == max_diag && x > 0 && y + 1 < height) {
                        nb = pixels_in + (y + 1) * stride + (x - 1) * 4;
                        wd = wbl;
                    }
                    if (nb) {
                        r = blend(r, nb[0], wd);
                        g = blend(g, nb[1], wd);
                        b = blend(b, nb[2], wd);
                        a = blend_channel(a, nb[3], wd);
                    }
                }
            }

            uint8_t* dst = pixels_out + y * stride + x * 4;
            dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
        }
    }
}

// --- Diagonal pipeline ---

void mlaa_detect_diagonal_edges(
    const float* luma, int width, int height,
    float threshold,
    DiagonalMap& diag_out)
{
    diag_out.width  = width;
    diag_out.height = height;
    size_t n = static_cast<size_t>(width) * height;
    diag_out.dr_edges.assign(n, 0);
    diag_out.dl_edges.assign(n, 0);

    // Pass 1: mark all candidate diagonal edges
    #ifdef CELSMOOTH_OPENMP
    #pragma omp parallel for schedule(static)
    #endif
    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width; ++x) {
            float l = luma[y * width + x];
            // DownRight: (x,y) vs (x+1,y+1)
            if (x + 1 < width) {
                if (std::abs(l - luma[(y + 1) * width + (x + 1)]) > threshold)
                    diag_out.dr_edges[y * width + x] = 1;
            }
            // DownLeft: (x,y) vs (x-1,y+1)
            if (x - 1 >= 0) {
                if (std::abs(l - luma[(y + 1) * width + (x - 1)]) > threshold)
                    diag_out.dl_edges[y * width + x] = 1;
            }
        }
    }

    // Pass 2: filter to chains of length >= 2
    // A diagonal edge is valid only if at least one neighbor in the same direction also has an edge
    std::vector<uint8_t> dr_valid(n, 0), dl_valid(n, 0);
    #ifdef CELSMOOTH_OPENMP
    #pragma omp parallel for schedule(static)
    #endif
    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            if (diag_out.dr_edges[idx]) {
                bool prev = (x > 0 && y > 0) && diag_out.dr_edges[(y - 1) * width + (x - 1)];
                bool next = (x + 1 < width && y + 1 < height - 1) && diag_out.dr_edges[(y + 1) * width + (x + 1)];
                if (prev || next) dr_valid[idx] = 1;
            }
            if (diag_out.dl_edges[idx]) {
                bool prev = (x + 1 < width && y > 0) && diag_out.dl_edges[(y - 1) * width + (x + 1)];
                bool next = (x - 1 >= 0 && y + 1 < height - 1) && diag_out.dl_edges[(y + 1) * width + (x - 1)];
                if (prev || next) dl_valid[idx] = 1;
            }
        }
    }
    diag_out.dr_edges = std::move(dr_valid);
    diag_out.dl_edges = std::move(dl_valid);
}

void mlaa_suppress_orthogonal_overlaps(
    EdgeMap& edges,
    const std::vector<DiagonalSegment>& diag_segments)
{
    int w = edges.width, h = edges.height;
    // Only suppress orthogonal edges covered by confirmed diagonal segments
    for (const auto& seg : diag_segments) {
        for (int i = 0; i < seg.length; ++i) {
            int px, py;
            if (seg.dir == DiagonalSegment::Dir::DownRight) {
                px = seg.start_x + i;
                py = seg.start_y + i;
            } else {
                px = seg.start_x - i;
                py = seg.start_y + i;
            }
            // Suppress H/V edges at and around this diagonal step
            if (px >= 0 && px < w && py >= 0 && py < h) {
                edges.h_edges[py * w + px] = 0;
                edges.v_edges[py * w + px] = 0;
            }
            // Also the next step's pixel
            int nx, ny;
            if (seg.dir == DiagonalSegment::Dir::DownRight) {
                nx = px + 1; ny = py + 1;
            } else {
                nx = px - 1; ny = py + 1;
            }
            if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
                edges.h_edges[ny * w + nx] = 0;
                edges.v_edges[ny * w + nx] = 0;
            }
        }
    }
}

void mlaa_build_diagonal_segments(
    const DiagonalMap& diagonals, int max_distance,
    std::vector<DiagonalSegment>& segments_out)
{
    segments_out.clear();
    int w = diagonals.width, h = diagonals.height;

    // Track which diagonal edges have been visited
    size_t n = static_cast<size_t>(w) * h;
    std::vector<uint8_t> dr_visited(n, 0), dl_visited(n, 0);

    // DownRight diagonals: trace from each unvisited edge
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!diagonals.dr(x, y) || dr_visited[y * w + x]) continue;
            int sx = x, sy = y, len = 0;
            int cx = x, cy = y;
            while (cx < w && cy < h && diagonals.dr(cx, cy) && !dr_visited[cy * w + cx]) {
                dr_visited[cy * w + cx] = 1;
                ++len; ++cx; ++cy;
            }
            if (len >= 3 && len <= max_distance) {
                DiagonalSegment seg;
                seg.dir = DiagonalSegment::Dir::DownRight;
                seg.start_x = sx;
                seg.start_y = sy;
                seg.length = len;
                segments_out.push_back(seg);
            }
        }
    }

    // DownLeft diagonals
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!diagonals.dl(x, y) || dl_visited[y * w + x]) continue;
            int sx = x, sy = y, len = 0;
            int cx = x, cy = y;
            while (cx >= 0 && cy < h && diagonals.dl(cx, cy) && !dl_visited[cy * w + cx]) {
                dl_visited[cy * w + cx] = 1;
                ++len; --cx; ++cy;
            }
            if (len >= 3 && len <= max_distance) {
                DiagonalSegment seg;
                seg.dir = DiagonalSegment::Dir::DownLeft;
                seg.start_x = sx;
                seg.start_y = sy;
                seg.length = len;
                segments_out.push_back(seg);
            }
        }
    }
}

void mlaa_compute_diagonal_blend_weights(
    const std::vector<DiagonalSegment>& segments,
    int width, int height, float strength,
    BlendWeights& weights)
{
    for (const auto& seg : segments) {
        int p = seg.length;
        if (p < 2) continue;

        for (int i = 0; i < p; ++i) {
            // Linear falloff: area(i) = (p - i) / (2p) from the start,
            // and (i + 1) / (2p) from the end
            // Use the smaller of the two (symmetrical falloff from both ends)
            float area_from_start = static_cast<float>(p - i) / (2.0f * p);
            float area_from_end   = static_cast<float>(i + 1) / (2.0f * p);
            float area = std::min(area_from_start, area_from_end);
            float w = area * strength;
            if (w <= 0.0f) continue;

            int px, py;
            if (seg.dir == DiagonalSegment::Dir::DownRight) {
                px = seg.start_x + i;
                py = seg.start_y + i;
            } else {
                px = seg.start_x - i;
                py = seg.start_y + i;
            }

            if (px < 0 || px >= width || py < 0 || py >= height) continue;
            int idx = py * width + px;

            if (seg.dir == DiagonalSegment::Dir::DownRight) {
                // Pixel (px,py) sits on a DownRight diagonal boundary
                // Blend toward (px+1,py+1) and (px-1,py-1) depending on which
                // side needs anti-aliasing. Use both directions symmetrically.
                float& slot_br = weights.below_right[idx];
                slot_br = std::max(slot_br, w);

                // Also affect the diagonal neighbor on the other side
                int nx = px + 1, ny = py + 1;
                if (nx < width && ny < height) {
                    int nidx = ny * width + nx;
                    float& slot_al = weights.above_left[nidx];
                    slot_al = std::max(slot_al, w);
                }
            } else {
                // DownLeft diagonal
                float& slot_bl = weights.below_left[idx];
                slot_bl = std::max(slot_bl, w);

                int nx = px - 1, ny = py + 1;
                if (nx >= 0 && ny < height) {
                    int nidx = ny * width + nx;
                    float& slot_ar = weights.above_right[nidx];
                    slot_ar = std::max(slot_ar, w);
                }
            }
        }
    }
}

// --- Top-level ---

void mlaa_process(
    const uint8_t* pixels_in,
    uint8_t*       pixels_out,
    int width, int height, int stride,
    const MlaaParams& params)
{
    // Stage 1: Luma
    std::vector<float> luma;
    mlaa_compute_luma(pixels_in, width, height, stride, luma);

    // Stage 2a: Orthogonal edge detection
    EdgeMap edges;
    mlaa_detect_edges(luma.data(), width, height, params.threshold, edges);

    // Stage 2b: Diagonal edge detection → segment building → then suppress orthogonal
    DiagonalMap diagonals;
    std::vector<DiagonalSegment> diag_segments;
    bool do_diag = params.enable_diagonals && !params.classic_mode;
    if (do_diag) {
        float diag_thresh = params.diagonal_threshold > 0
                            ? params.diagonal_threshold : params.threshold;
        mlaa_detect_diagonal_edges(luma.data(), width, height, diag_thresh, diagonals);
        mlaa_build_diagonal_segments(diagonals, params.diagonal_max_distance, diag_segments);
        mlaa_suppress_orthogonal_overlaps(edges, diag_segments);
    }

    // Stage 3: Build orthogonal segments
    std::vector<EdgeSegment> segments;
    mlaa_build_segments(edges, params.max_distance, segments);

    // Stage 4: Classify crossing edges
    mlaa_classify_segments(edges, segments, params);

    // Stage 5: Decompose to L-shapes + compute blend weights
    std::vector<LShape> lshapes;
    mlaa_decompose_to_lshapes(segments, lshapes);

    BlendWeights weights;
    mlaa_compute_blend_weights(lshapes, width, height, params, weights);

    // Stage 5b: Diagonal blend weights
    if (do_diag) {
        mlaa_compute_diagonal_blend_weights(diag_segments, width, height, params.strength, weights);
    }

    // Stage 6: Blend
    mlaa_apply_blending(pixels_in, pixels_out, width, height, stride, weights, params);
}

} // namespace celsmooth

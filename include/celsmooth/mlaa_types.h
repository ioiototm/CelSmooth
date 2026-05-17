#pragma once
#include <cstdint>
#include <vector>

namespace celsmooth {

struct MlaaParams {
    float threshold    = 0.1f;
    int   max_distance = 32;
    float strength     = 1.0f;

    // V2: T/Cross shapes (Paper §2.2)
    bool  enable_t_cross     = true;

    // V2: Diagonal detection (Paper §2.3)
    bool  enable_diagonals   = true;
    float diagonal_threshold = -1.0f;  // -1 = use main threshold
    int   diagonal_max_distance = 32;

    // V2: Gamma correction (Paper §3)
    bool  enable_gamma       = true;
    float extended_gamma     = 1.0f;   // >1 boosts thin dark strokes

    // V2: Smoothness (Paper §4)
    float smoothness         = 1.0f;   // 0..2
    bool  u_rounding         = true;   // w'=2w² for U-shapes

    // Disable all V2 features
    bool  classic_mode       = false;
};

struct EdgeMap {
    std::vector<uint8_t> h_edges; // width * height; h_edges[y*width+x] = edge between row y-1 and row y at column x
    std::vector<uint8_t> v_edges; // width * height; v_edges[y*width+x] = edge between col x-1 and col x at row y
    int width  = 0;
    int height = 0;

    bool h(int x, int y) const { return h_edges[y * width + x] != 0; }
    bool v(int x, int y) const { return v_edges[y * width + x] != 0; }
};

struct EdgeSegment {
    enum class Dir : uint8_t { Horizontal, Vertical };
    enum class CrossType : uint8_t { None, Above, Below, Both };

    Dir       dir;
    int       fixed_coord; // y for Horizontal, x for Vertical
    int       start;       // first coord along segment (x for H, y for V)
    int       end;         // last coord (inclusive)
    CrossType start_cross = CrossType::None;
    CrossType end_cross   = CrossType::None;

    int length() const { return end - start + 1; }
};

struct LShape {
    EdgeSegment::Dir dir;
    int  fixed_coord;
    int  start;                    // start of affected pixel range
    int  length;                   // pixels affected
    bool secondary_at_start;       // true = crossing edge is at the 'start' end
    bool blend_with_above_or_left; // true = affected pixels blend toward above (H) or left (V)
    bool from_u_shape = false;
};

struct DiagonalMap {
    std::vector<uint8_t> dr_edges; // DownRight: edge between (x,y) and (x+1,y+1)
    std::vector<uint8_t> dl_edges; // DownLeft: edge between (x,y) and (x-1,y+1)
    int width  = 0;
    int height = 0;

    bool dr(int x, int y) const { return dr_edges[y * width + x] != 0; }
    bool dl(int x, int y) const { return dl_edges[y * width + x] != 0; }
};

struct DiagonalSegment {
    enum class Dir : uint8_t { DownRight, DownLeft };
    Dir dir;
    int start_x, start_y;
    int length;
    EdgeSegment::CrossType start_cross = EdgeSegment::CrossType::None;
    EdgeSegment::CrossType end_cross   = EdgeSegment::CrossType::None;
};

struct BlendWeights {
    std::vector<float> above; // width * height
    std::vector<float> below;
    std::vector<float> left;
    std::vector<float> right;
    // Diagonal blend weights
    std::vector<float> above_left;  // blend toward pixel at (-1,-1)
    std::vector<float> below_right; // blend toward pixel at (+1,+1)
    std::vector<float> above_right; // blend toward pixel at (+1,-1)
    std::vector<float> below_left;  // blend toward pixel at (-1,+1)
    int width  = 0;
    int height = 0;

    void init(int w, int h, bool diag = false) {
        width = w; height = h;
        size_t n = static_cast<size_t>(w) * h;
        above.assign(n, 0.0f);
        below.assign(n, 0.0f);
        left.assign(n, 0.0f);
        right.assign(n, 0.0f);
        if (diag) {
            above_left.assign(n, 0.0f);
            below_right.assign(n, 0.0f);
            above_right.assign(n, 0.0f);
            below_left.assign(n, 0.0f);
        }
    }

    bool has_diagonal() const { return !above_left.empty(); }
};

} // namespace celsmooth

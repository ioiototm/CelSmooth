#pragma once
#include "mlaa_types.h"

namespace celsmooth {

void mlaa_process(
    const uint8_t* pixels_in,
    uint8_t*       pixels_out,
    int width, int height, int stride,
    const MlaaParams& params);

void mlaa_compute_luma(
    const uint8_t* pixels, int width, int height, int stride,
    std::vector<float>& luma_out);

void mlaa_detect_edges(
    const float* luma, int width, int height,
    float threshold,
    EdgeMap& edges_out);

void mlaa_build_segments(
    const EdgeMap& edges, int max_distance,
    std::vector<EdgeSegment>& segments_out);

void mlaa_classify_segments(
    const EdgeMap& edges,
    std::vector<EdgeSegment>& segments,
    const MlaaParams& params);

void mlaa_decompose_to_lshapes(
    const std::vector<EdgeSegment>& segments,
    std::vector<LShape>& lshapes_out);

void mlaa_compute_blend_weights(
    const std::vector<LShape>& lshapes,
    int width, int height,
    const MlaaParams& params,
    BlendWeights& weights_out);

void mlaa_apply_blending(
    const uint8_t* pixels_in,
    uint8_t*       pixels_out,
    int width, int height, int stride,
    const BlendWeights& weights,
    const MlaaParams& params);

// Diagonal pipeline
void mlaa_detect_diagonal_edges(
    const float* luma, int width, int height,
    float threshold,
    DiagonalMap& diag_out);

void mlaa_suppress_orthogonal_overlaps(
    EdgeMap& edges,
    const std::vector<DiagonalSegment>& diag_segments);

void mlaa_build_diagonal_segments(
    const DiagonalMap& diagonals, int max_distance,
    std::vector<DiagonalSegment>& segments_out);

void mlaa_compute_diagonal_blend_weights(
    const std::vector<DiagonalSegment>& segments,
    int width, int height, float strength,
    BlendWeights& weights);

} // namespace celsmooth

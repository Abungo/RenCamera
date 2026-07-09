#pragma once
#include "../pipeline.h"

/// ToneMapStage — bilateral-grid HDR tone mapping
///
/// Reads:   ctx.colorImage     (RGB24 from DebayerStage)
/// Writes:  ctx.processedImage (RGB24, tone-mapped)
///
/// Algorithm (bilateral grid):
///   1. Compute luminance L = 0.299R + 0.587G + 0.114B per pixel.
///   2. Build a 3-D sparse grid: [gridY][gridX][gridI].
///      spatial_sigma ≈ 16 px, intensity_bins = 16.
///   3. Blur the grid (spatial Gaussian + intensity Gaussian).
///   4. For each pixel, trilinearly interpolate the grid → base_L.
///   5. detail_L = L - base_L.
///   6. Compressed L = S_curve(base_L) + alpha * detail_L.
///   7. Scale R, G, B by (compressedL / L) → preserve hue.
///   8. Slight saturation boost in HSL space.
class ToneMapStage final : public PipelineStage {
public:
    std::string_view name() const override { return "tonemap"; }
    bool process(FrameContext& ctx) override;
};

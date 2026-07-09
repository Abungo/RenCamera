#pragma once
#include "../pipeline.h"

/// FusionStage — temporal noise reduction via weighted frame merging
///
/// Reads:   ctx.inputFrames  (12 raw YUV frames)
///          ctx.motionFields (11 MotionField objects from AlignStage)
/// Writes:  ctx.fusedY, ctx.fusedU, ctx.fusedV (uint8, I420-layout)
///          ctx.fusedWidth, ctx.fusedHeight
///
/// Algorithm:
///   For each output pixel:
///   1. Warp each non-reference frame using its block motion vector (bilinear).
///   2. Compute a similarity weight: w = exp(−residual² / σ²).
///   3. Accumulate weighted pixels; reference frame gets weight = 1.0.
///   4. Divide by accumulated weight → noise-reduced output.
class FusionStage final : public PipelineStage {
public:
    std::string_view name() const override { return "fuse"; }
    bool process(FrameContext& ctx) override;
};

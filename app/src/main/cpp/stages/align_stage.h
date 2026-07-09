#pragma once
#include "../pipeline.h"

/// AlignStage — Pyramidal block alignment (anti-ghosting)
///
/// Reads:   ctx.inputFrames  (12 raw YUV frames)
/// Writes:  ctx.motionFields (11 MotionField objects, one per non-reference frame)
///
/// Algorithm:
///   1. Convert reference frame Y-plane to float.
///   2. Build a 3-level Gaussian pyramid for the reference.
///   3. For each of the 11 non-reference frames:
///      a. Build its 3-level Gaussian pyramid.
///      b. Coarse-to-fine block matching: estimate at level 2, refine at 1 and 0.
///   Output motion fields are in full-resolution pixel units.
class AlignStage final : public PipelineStage {
public:
    std::string_view name() const override { return "align"; }
    bool process(FrameContext& ctx) override;
};

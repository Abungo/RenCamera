#pragma once
#include "../pipeline.h"

/// DefectPixelStage — Stuck/hot pixel filtering on RAW sensor frames
///
/// Reads and writes in-place: ctx.inputFrames (modifies RAW data directly)
class DefectPixelStage final : public PipelineStage {
public:
    std::string_view name() const override { return "defect_pixel"; }
    bool process(FrameContext& ctx) override;
};

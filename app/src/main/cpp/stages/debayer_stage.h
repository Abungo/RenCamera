#pragma once
#include "../pipeline.h"

/// DebayerStage — YUV I420 → RGB24 color reconstruction
///
/// Reads:   ctx.fusedY, ctx.fusedU, ctx.fusedV  (packed I420 uint8 planes)
///          ctx.fusedWidth, ctx.fusedHeight
/// Writes:  ctx.colorImage  (RgbImage, interleaved RGB24)
///
/// Uses Highway-vectorized BT.601 YUV→RGB conversion.
/// No external library dependency.
class DebayerStage final : public PipelineStage {
public:
    std::string_view name() const override { return "debayer"; }
    bool process(FrameContext& ctx) override;
};

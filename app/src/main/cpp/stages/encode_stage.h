#pragma once
#include "../pipeline.h"

/// EncodeStage — compress processed RGB24 to JPEG using libjpeg-turbo
///
/// Reads:   ctx.processedImage   (RGB24)
///          ctx.metadata["jpeg_quality"]  (int, default 95)
/// Writes:  ctx.jpegOutput       (JPEG bytes)
class EncodeStage final : public PipelineStage {
public:
    std::string_view name() const override { return "encode"; }
    bool process(FrameContext& ctx) override;
};

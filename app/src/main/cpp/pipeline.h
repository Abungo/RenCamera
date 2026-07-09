#pragma once

#include "frame_context.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <android/log.h>

#include <functional>

#define LOG_TAG "RenCamera/Pipeline"
#define LOGI(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─────────────────────────────────────────────────────────────────────────────
// PipelineStage
//   Every processing step implements this interface.
//   Naming convention: stage names are lowercase snake_case ("align", "denoise").
//
//   To add a new stage:
//     1. Create stages/my_stage.cpp implementing PipelineStage.
//     2. Call pipeline.addStage(std::make_unique<MyStage>()) in gcam_core.cpp.
//     3. (Optional) Toggle it from Kotlin via PipelineConfig.
// ─────────────────────────────────────────────────────────────────────────────
class PipelineStage {
public:
    virtual ~PipelineStage() = default;

    /// Unique name used for enable/disable lookups and logging.
    virtual std::string_view name() const = 0;

    /// Execute this stage. Return false to abort the pipeline (error path).
    virtual bool process(FrameContext& ctx) = 0;

    bool enabled = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline
//   An ordered list of PipelineStage objects. Runs enabled stages in order.
//   Supports named insertion for dynamic stage injection without re-ordering
//   the entire list.
// ─────────────────────────────────────────────────────────────────────────────
class Pipeline {
public:
    // ── Registration ─────────────────────────────────────────────────────────

    /// Append a stage at the end of the pipeline.
    void addStage(std::unique_ptr<PipelineStage> stage);

    /// Insert a stage immediately after the stage named `after`.
    /// If `after` is not found, appends at the end.
    void insertAfter(std::string_view after, std::unique_ptr<PipelineStage> stage);

    /// Insert a stage immediately before the stage named `before`.
    /// If `before` is not found, prepends at the start.
    void insertBefore(std::string_view before, std::unique_ptr<PipelineStage> stage);

    // ── Runtime control ───────────────────────────────────────────────────────

    /// Enable or disable a named stage at runtime (called from JNI config).
    void setEnabled(std::string_view name, bool enabled);

    // ── Execution ─────────────────────────────────────────────────────────────

    /// Run all enabled stages in order.
    /// Returns false if any stage reports failure.
    bool run(FrameContext& ctx, std::function<void(const std::string&, int)> progressCallback = nullptr);

    // ── Inspection ────────────────────────────────────────────────────────────

    /// Number of registered stages (including disabled ones).
    size_t size() const { return stages_.size(); }

private:
    std::vector<std::unique_ptr<PipelineStage>> stages_;

    /// Find a stage index by name. Returns stages_.size() if not found.
    size_t indexOf(std::string_view name) const;
};
